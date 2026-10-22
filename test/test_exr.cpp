/* Tests for the OpenEXR driver in src/im_format_exr.cpp.
 *
 * Float channels with a lossless compression are exact, so those cases
 * demand zero difference. Half keeps 11 significant bits, about 1 part in
 * 2048. The fixtures were written by OpenEXR's own Python bindings and its
 * C++ RgbaOutputFile -- see make_float_fixtures.py -- so the reader is
 * checked against writers it shares no code with.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_exr.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

std::string out_path(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

std::string fixture(const char* name)
{
  return std::string(IM_TEST_FIXTURE_DIR) + "/" + name;
}

/* The formula make_float_fixtures.py writes. */
double fixture_value(int x, int y, int c)
{
  return pow(2.0, (x - 16) / 4.0) * (1.0 + c / 4.0) + y / 100.0;
}

int plane_count(const imImage* image)
{
  return image->depth + (image->has_alpha? 1: 0);
}

float* plane(const imImage* image, int c)
{
  return ((float**)image->data)[c];
}

/* A pixel by file coordinates, (0,0) at the top left. IM keeps rows bottom
   up in memory -- row 0 is the bottom of the picture -- so anything compared
   against a fixture written top down goes through this. */
float& at(const imImage* image, int c, int x, int y)
{
  return plane(image, c)[(image->height - 1 - y) * image->width + x];
}

/* fixture_value() for colour, and for alpha something in 0..1 that is not a
   round binary fraction, plus a negative value and one far past 1 -- a float
   format has to carry both. */
void fill_pattern(imImage* image)
{
  for (int c = 0; c < plane_count(image); c++)
    for (int y = 0; y < image->height; y++)
      for (int x = 0; x < image->width; x++)
      {
        float v;
        if (image->has_alpha && c == image->depth)
          v = (float)((x + y) % 7) / 6.3f;
        else
          v = (float)fixture_value(x, y, c);
        plane(image, c)[y * image->width + x] = v;
      }

  plane(image, 0)[0] = -2.5f;
  plane(image, 0)[1] = 1.0e6f;
}

double worst_relative_error(const imImage* a, const imImage* b)
{
  double worst = 0;
  int total = a->width * a->height;
  for (int c = 0; c < plane_count(a); c++)
    for (int i = 0; i < total; i++)
    {
      double expected = plane(a, c)[i];
      double scale = fabs(expected) > 1e-3? fabs(expected): 1e-3;
      double error = fabs(expected - plane(b, c)[i]) / scale;
      if (error > worst)
        worst = error;
    }
  return worst;
}

int count_differences(const imImage* a, const imImage* b)
{
  int total = a->width * a->height, different = 0;
  for (int c = 0; c < plane_count(a); c++)
    for (int i = 0; i < total; i++)
      if (plane(a, c)[i] != plane(b, c)[i])
        different++;
  return different;
}

imImage* round_trip(imImage* source, const char* file, const char* compression,
                    int half, int* error_out)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  imFile* handle = imFileNew(path.c_str(), "EXR", &error);
  if (!handle)
  {
    *error_out = error;
    return NULL;
  }
  if (compression)
    imFileSetInfo(handle, compression);
  imFileSetAttribute(handle, "Half", IM_INT, 1, &half);

  error = imFileSaveImage(handle, source);
  imFileClose(handle);
  if (error != IM_ERR_NONE)
  {
    *error_out = error;
    return NULL;
  }

  imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
  *error_out = loaded? IM_ERR_NONE: error;
  return loaded;
}

struct RegisterOnce
{
  RegisterOnce() { imFormatRegisterEXR(); }
};

/* imFormatRegisterEXR allocates and registers a driver on every call, so
   register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("EXR: CanWrite accepts float gray and RGB, with or without alpha")
{
  CHECK(imFormatCanWriteImage("EXR", "ZIP", IM_RGB, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("EXR", "", IM_RGB | IM_ALPHA, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("EXR", "DWAB", IM_GRAY, IM_FLOAT) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("EXR", "", IM_RGB, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("EXR", "", IM_RGB, IM_DOUBLE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("EXR", "", IM_XYZ, IM_FLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("EXR", "", IM_MAP, IM_FLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("EXR", "LZW", IM_RGB, IM_FLOAT) == IM_ERR_COMPRESS);
}

TEST_CASE("EXR: float channels are exact under every lossless compression")
{
  const char* compressions[5] = { "NONE", "RLE", "ZIPS", "ZIP", "PIZ" };
  const int modes[4] = { IM_RGB, IM_RGB | IM_ALPHA, IM_GRAY, IM_GRAY | IM_ALPHA };

  for (int k = 0; k < 5; k++)
  {
    for (int m = 0; m < 4; m++)
    {
      CAPTURE(compressions[k]);
      CAPTURE(modes[m]);

      imImage* source = imImageCreate(37, 23, modes[m], IM_FLOAT);
      REQUIRE(source != NULL);
      fill_pattern(source);

      int error = IM_ERR_NONE;
      imImage* loaded = round_trip(source, "rt_lossless.exr", compressions[k], 0, &error);
      REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

      CHECK(loaded->width == 37);
      CHECK(loaded->height == 23);
      CHECK(loaded->data_type == IM_FLOAT);
      CHECK(imColorModeSpace(loaded->color_space) == imColorModeSpace(modes[m]));
      CHECK((loaded->has_alpha != 0) == (imColorModeHasAlpha(modes[m]) != 0));
      CHECK(count_differences(source, loaded) == 0);

      imImageDestroy(loaded);
      imImageDestroy(source);
    }

    /* And the file names its own compression. */
    int error = IM_ERR_NONE;
    imFile* handle = imFileOpen(out_path("rt_lossless.exr").c_str(), &error);
    REQUIRE(handle != NULL);
    char format[16] = { 0 }, compression[16] = { 0 };
    imFileGetInfo(handle, format, compression, NULL);
    CHECK(strcmp(format, "EXR") == 0);
    CHECK(strcmp(compression, compressions[k]) == 0);
    imFileClose(handle);
  }
}

TEST_CASE("EXR: Half=1 writes half channels, accurate to 11 bits")
{
  imImage* source = imImageCreate(37, 23, IM_RGB | IM_ALPHA, IM_FLOAT);
  REQUIRE(source != NULL);
  fill_pattern(source);
  plane(source, 0)[1] = 60000.0f;   /* 1e6 is past half's 65504 */

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_half.exr", "ZIP", 1, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  double worst = worst_relative_error(source, loaded);
  CAPTURE(worst);
  CHECK(worst <= 1.0 / 1024);

  /* Half, so not every value can have survived exactly. */
  CHECK(count_differences(source, loaded) > 0);

  /* Half files are half the size: a check the attribute was not ignored. */
  imImage* full = round_trip(source, "rt_full.exr", "NONE", 0, &error);
  REQUIRE(full != NULL);
  imImageDestroy(full);
  imImage* halved = round_trip(source, "rt_halved.exr", "NONE", 1, &error);
  REQUIRE(halved != NULL);
  imImageDestroy(halved);

  FILE* f1 = fopen(out_path("rt_full.exr").c_str(), "rb");
  FILE* f2 = fopen(out_path("rt_halved.exr").c_str(), "rb");
  REQUIRE(f1 != NULL);
  REQUIRE(f2 != NULL);
  fseek(f1, 0, SEEK_END);
  fseek(f2, 0, SEEK_END);
  CHECK(ftell(f2) < ftell(f1) * 2 / 3);
  fclose(f1);
  fclose(f2);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("EXR: a lossy compression keeps geometry and stays close")
{
  const char* compressions[3] = { "DWAA", "B44", "PXR24" };
  for (int k = 0; k < 3; k++)
  {
    CAPTURE(compressions[k]);

    imImage* source = imImageCreate(64, 48, IM_RGB, IM_FLOAT);
    REQUIRE(source != NULL);
    fill_pattern(source);
    plane(source, 0)[0] = 0.5f;     /* keep to values every codec handles */
    plane(source, 0)[1] = 0.5f;

    /* B44 only compresses half; on float it is stored losslessly. */
    int half = (strcmp(compressions[k], "B44") == 0);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, "rt_lossy.exr", compressions[k], half, &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == 64);
    CHECK(loaded->height == 48);

    double mean = 0;
    int total = 64 * 48;
    for (int c = 0; c < 3; c++)
      for (int i = 0; i < total; i++)
        mean += fabs(plane(source, c)[i] - plane(loaded, c)[i]) / fabs(plane(source, c)[i]);
    mean /= 3.0 * total;
    CAPTURE(mean);

    /* A swapped channel or row offset is off by tens of percent. */
    CHECK(mean < 0.02);

    imImageDestroy(loaded);
    imImageDestroy(source);
  }
}

TEST_CASE("EXR: files from OpenEXR's own writers decode correctly")
{
  int error = IM_ERR_NONE;

  SUBCASE("RGBA half, PIZ")
  {
    imImage* image = imFileImageLoad(fixture("exr_rgba_half_piz.exr").c_str(), 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    REQUIRE(image->width == 40);
    REQUIRE(image->height == 30);
    CHECK(imColorModeSpace(image->color_space) == IM_RGB);
    REQUIRE(image->has_alpha != 0);

    double worst = 0, worst_alpha = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
      {
        for (int c = 0; c < 3; c++)
        {
          double expected = fixture_value(x, y, c);
          double e = fabs(at(image, c, x, y) - expected) / expected;
          if (e > worst) worst = e;
        }
        double a = fabs(at(image, 3, x, y) - y / 29.0);
        if (a > worst_alpha) worst_alpha = a;
      }
    CHECK(worst <= 1.0 / 1024);
    CHECK(worst_alpha <= 1.0 / 1024);
    imImageDestroy(image);
  }

  SUBCASE("Y float, tiled, is exact gray")
  {
    imImage* image = imFileImageLoad(fixture("exr_y_float_tiled.exr").c_str(), 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    REQUIRE(image->width == 40);
    REQUIRE(image->height == 30);
    CHECK(imColorModeSpace(image->color_space) == IM_GRAY);
    CHECK(image->has_alpha == 0);

    int wrong = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
        if (at(image, 0, x, y) != (float)fixture_value(x, y, 0))
          wrong++;
    CHECK(wrong == 0);
    imImageDestroy(image);
  }

  SUBCASE("a layer inside an offset data window, beside an unrelated channel")
  {
    /* diffuse.R/G/B in a 40x30 data window at (10,5) of a 64x48 display
       window, plus a Z channel that must be ignored. The size is the data
       window's, and value() is relative to its corner. */
    imImage* image = imFileImageLoad(fixture("exr_layer_window.exr").c_str(), 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    REQUIRE(image->width == 40);
    REQUIRE(image->height == 30);
    CHECK(imColorModeSpace(image->color_space) == IM_RGB);
    CHECK(image->has_alpha == 0);

    int wrong = 0;
    for (int c = 0; c < 3; c++)
      for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
          if (at(image, c, x, y) != (float)fixture_value(x, y, c))
            wrong++;
    CHECK(wrong == 0);
    imImageDestroy(image);
  }

  SUBCASE("luminance and subsampled chroma become RGB")
  {
    /* A gray image written through RgbaOutputFile's WRITE_YC: chroma is
       zero, so the reconstruction must give three channels equal to the
       luminance, to half precision plus the YC round trip. */
    imImage* image = imFileImageLoad(fixture("exr_yc.exr").c_str(), 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    REQUIRE(image->width == 40);
    REQUIRE(image->height == 30);
    CHECK(imColorModeSpace(image->color_space) == IM_RGB);

    double worst = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
      {
        double expected = pow(2.0, (x - 16) / 4.0) + y / 100.0;
        for (int c = 0; c < 3; c++)
        {
          double e = fabs(at(image, c, x, y) - expected) / expected;
          if (e > worst) worst = e;
        }
      }
    CAPTURE(worst);
    CHECK(worst < 0.01);
    imImageDestroy(image);
  }
}

TEST_CASE("EXR: channels with no image interpretation are refused")
{
  /* Z and N.x and nothing else: picking either would be a guess. */
  int error = IM_ERR_NONE;
  imImage* image = imFileImageLoad(fixture("exr_unmappable.exr").c_str(), 0, &error);
  CHECK(image == NULL);
  CHECK(error == IM_ERR_DATA);
  if (image)
    imImageDestroy(image);
}

TEST_CASE("EXR: a file that is not EXR is declined")
{
  std::string path = out_path("not_exr.bin");
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  unsigned char junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i * 11 + 1);
  fwrite(junk, 1, sizeof(junk), file);
  fclose(file);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "EXR", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("EXR: a truncated file fails cleanly")
{
  /* The header survives, so Open succeeds and the pixel read finds the data
     missing -- which OpenEXR reports by throwing, and the driver must turn
     into an error code rather than let it escape. */
  std::string whole = fixture("exr_layer_window.exr");
  FILE* in = fopen(whole.c_str(), "rb");
  REQUIRE(in != NULL);
  unsigned char buffer[2048];
  size_t got = fread(buffer, 1, sizeof(buffer), in);
  fclose(in);
  REQUIRE(got == sizeof(buffer));

  std::string cut = out_path("truncated.exr");
  FILE* out = fopen(cut.c_str(), "wb");
  REQUIRE(out != NULL);
  fwrite(buffer, 1, got, out);
  fclose(out);

  int error = IM_ERR_NONE;
  imImage* loaded = imFileImageLoad(cut.c_str(), 0, &error);

  /* IM hands back the allocated image even when the data read fails; the
     error code is what says not to trust it. */
  CHECK(error != IM_ERR_NONE);
  if (loaded)
    imImageDestroy(loaded);
}
