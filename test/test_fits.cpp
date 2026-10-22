/* Tests for the FITS driver in src/im_format_fits.cpp.
 *
 * FITS is lossless for every type it holds, so the comparisons are exact.
 * The fixtures come from astropy (see make_qoi_fits_dng_fixtures.py). FITS
 * stores the bottom row first, which is IM's own order, so array row y of
 * the generator is IM row y here -- no flip anywhere in this file.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_fits.h>

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

/* The generator's formula. */
long fits_value(int x, int y, int p = 0)
{
  return (x * 1000L + y * 37L + p * 5000L) % 65536L;
}

double sample(const imImage* image, int c, int i)
{
  switch (image->data_type)
  {
  case IM_BYTE:   return ((imbyte**)image->data)[c][i];
  case IM_SHORT:  return ((short**)image->data)[c][i];
  case IM_USHORT: return ((imushort**)image->data)[c][i];
  case IM_INT:    return ((int**)image->data)[c][i];
  case IM_FLOAT:  return ((float**)image->data)[c][i];
  case IM_DOUBLE: return ((double**)image->data)[c][i];
  }
  return 0;
}

void set_sample(imImage* image, int c, int i, double v)
{
  switch (image->data_type)
  {
  case IM_BYTE:   ((imbyte**)image->data)[c][i] = (imbyte)v; break;
  case IM_SHORT:  ((short**)image->data)[c][i] = (short)v; break;
  case IM_USHORT: ((imushort**)image->data)[c][i] = (imushort)v; break;
  case IM_INT:    ((int**)image->data)[c][i] = (int)v; break;
  case IM_FLOAT:  ((float**)image->data)[c][i] = (float)v; break;
  case IM_DOUBLE: ((double**)image->data)[c][i] = v; break;
  }
}

/* Spans each type's range, signed types going negative. */
void fill_pattern(imImage* image)
{
  int total = image->width * image->height;
  for (int c = 0; c < image->depth; c++)
    for (int i = 0; i < total; i++)
    {
      double v = (double)((i * 37 + c * 101) % 251);
      switch (image->data_type)
      {
      case IM_SHORT:  v = v * 257 - 32000; break;
      case IM_USHORT: v = v * 261; break;
      case IM_INT:    v = v * 8000000.0 - 1000000000.0; break;
      case IM_FLOAT:  v = v / 3.0 - 20.0; break;
      case IM_DOUBLE: v = v / 7.0 + 1e10; break;
      }
      set_sample(image, c, i, v);
    }
}

int count_differences(const imImage* a, const imImage* b)
{
  int total = a->width * a->height, different = 0;
  for (int c = 0; c < a->depth; c++)
    for (int i = 0; i < total; i++)
      if (sample(a, c, i) != sample(b, c, i))
        different++;
  return different;
}

imImage* round_trip(imImage* source, const char* file, const char* compression,
                    int* error_out)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  imFile* handle = imFileNew(path.c_str(), "FITS", &error);
  if (!handle)
  {
    *error_out = error;
    return NULL;
  }
  imFileSetInfo(handle, compression);
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
  RegisterOnce() { imFormatRegisterFITS(); }
};

/* imFormatRegisterFITS allocates and registers a driver on every call, so
   register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("FITS: CanWrite accepts every type FITS stores, Rice only for integers")
{
  CHECK(imFormatCanWriteImage("FITS", "NONE", IM_GRAY, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("FITS", "", IM_RGB, IM_DOUBLE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("FITS", "GZIP", IM_GRAY, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("FITS", "RICE", IM_GRAY, IM_INT) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("FITS", "RICE", IM_GRAY, IM_FLOAT) == IM_ERR_COMPRESS);
  CHECK(imFormatCanWriteImage("FITS", "HCOMPRESS", IM_GRAY, IM_SHORT) == IM_ERR_COMPRESS);
  CHECK(imFormatCanWriteImage("FITS", "", IM_GRAY, IM_CFLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("FITS", "", IM_CMYK, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("FITS", "", IM_MAP, IM_BYTE) == IM_ERR_DATA);
}

TEST_CASE("FITS: every type round-trips exactly, gray and RGB, under each compression")
{
  const int types[6] = { IM_BYTE, IM_SHORT, IM_USHORT, IM_INT, IM_FLOAT, IM_DOUBLE };
  const char* compressions[3] = { "NONE", "GZIP", "RICE" };

  for (int t = 0; t < 6; t++)
    for (int s = 0; s < 2; s++)
      for (int k = 0; k < 3; k++)
      {
        int is_float = (types[t] == IM_FLOAT || types[t] == IM_DOUBLE);
        if (is_float && k == 2)
          continue;   /* Rice is integer-only */

        int space = s? IM_RGB: IM_GRAY;
        CAPTURE(types[t]);
        CAPTURE(space);
        CAPTURE(compressions[k]);

        imImage* source = imImageCreate(37, 23, space, types[t]);
        REQUIRE(source != NULL);
        fill_pattern(source);

        int error = IM_ERR_NONE;
        imImage* loaded = round_trip(source, "rt.fits", compressions[k], &error);
        REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

        CHECK(loaded->width == 37);
        CHECK(loaded->height == 23);
        CHECK(loaded->data_type == types[t]);
        CHECK(imColorModeSpace(loaded->color_space) == space);
        CHECK(count_differences(source, loaded) == 0);

        imImageDestroy(loaded);
        imImageDestroy(source);
      }
}

TEST_CASE("FITS: files from astropy decode exactly")
{
  int error = IM_ERR_NONE;

  SUBCASE("unsigned 16 bit, stored as int16 with BZERO, and its keywords")
  {
    imFile* handle = imFileOpen(fixture("fits_uint16.fits").c_str(), &error);
    REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);
    imImage* image = imFileLoadImage(handle, 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);

    REQUIRE(image->width == 40);
    REQUIRE(image->height == 30);
    CHECK(image->data_type == IM_USHORT);
    CHECK(imColorModeSpace(image->color_space) == IM_GRAY);

    int wrong = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
        if (sample(image, 0, y * 40 + x) != fits_value(x, y))
          wrong++;
    CHECK(wrong == 0);

    int type = 0, count = 0;
    const char* object = (const char*)imFileGetAttribute(handle, "OBJECT", &type, &count);
    REQUIRE(object != NULL);
    CHECK(std::string(object) == "M31");
    const float* exptime = (const float*)imFileGetAttribute(handle, "EXPTIME", &type, &count);
    REQUIRE(exptime != NULL);
    CHECK(type == IM_FLOAT);
    CHECK(*exptime == 30.5f);

    imImageDestroy(image);
    imFileClose(handle);
  }

  SUBCASE("BSCALE 0.5 makes integers into floats")
  {
    imImage* image = imFileImageLoad(fixture("fits_scaled.fits").c_str(), 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    CHECK(image->data_type == IM_FLOAT);

    int wrong = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
      {
        double expected = ((x * 7 + y * 3) % 200 - 100) * 0.5 + 100.0;
        if (sample(image, 0, y * 40 + x) != expected)
          wrong++;
      }
    CHECK(wrong == 0);
    imImageDestroy(image);
  }

  SUBCASE("a float cube in an extension is a sequence of planes")
  {
    imFile* handle = imFileOpen(fixture("fits_float_cube.fits").c_str(), &error);
    REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);

    int image_count = 0;
    imFileGetInfo(handle, NULL, NULL, &image_count);
    REQUIRE(image_count == 4);

    /* Out of order, so the plane chosen comes from the index and not from
       how far reading had got. */
    const int order[4] = { 2, 0, 3, 1 };
    for (int k = 0; k < 4; k++)
    {
      int p = order[k];
      CAPTURE(p);
      imImage* image = imFileLoadImage(handle, p, &error);
      REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
      CHECK(image->data_type == IM_FLOAT);
      CHECK(imColorModeSpace(image->color_space) == IM_GRAY);

      int wrong = 0;
      for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
          if (sample(image, 0, y * 40 + x) != (float)(fits_value(x, y, p) / 7.0))
            wrong++;
      CHECK(wrong == 0);
      imImageDestroy(image);
    }
    imFileClose(handle);
  }

  SUBCASE("a three plane cube is one RGB image")
  {
    imFile* handle = imFileOpen(fixture("fits_rgb.fits").c_str(), &error);
    REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);
    int image_count = 0;
    imFileGetInfo(handle, NULL, NULL, &image_count);
    CHECK(image_count == 1);

    imImage* image = imFileLoadImage(handle, 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    CHECK(imColorModeSpace(image->color_space) == IM_RGB);
    CHECK(image->data_type == IM_BYTE);

    int wrong = 0;
    for (int c = 0; c < 3; c++)
      for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++)
          if (sample(image, c, y * 40 + x) != (x * 6 + c * 40 + y) % 256)
            wrong++;
    CHECK(wrong == 0);
    imImageDestroy(image);
    imFileClose(handle);
  }

  SUBCASE("Rice tile compression is read, and reported")
  {
    imFile* handle = imFileOpen(fixture("fits_rice.fits").c_str(), &error);
    REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);
    imImage* image = imFileLoadImage(handle, 0, &error);
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);

    char compression[16] = { 0 };
    imFileGetInfo(handle, NULL, compression, NULL);
    CHECK(std::string(compression) == "RICE");
    CHECK(image->data_type == IM_INT);

    int wrong = 0;
    for (int y = 0; y < 30; y++)
      for (int x = 0; x < 40; x++)
        if (sample(image, 0, y * 40 + x) != fits_value(x, y) * 1000 - 20000000)
          wrong++;
    CHECK(wrong == 0);
    imImageDestroy(image);
    imFileClose(handle);
  }
}

TEST_CASE("FITS: a file that is not FITS is declined")
{
  std::string path = out_path("not_fits.bin");
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  unsigned char junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i * 19 + 7);
  fwrite(junk, 1, sizeof(junk), file);
  fclose(file);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "FITS", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("FITS: a truncated file fails cleanly")
{
  /* Keep the header block (2880 bytes) and a little of the data. */
  std::string whole = fixture("fits_uint16.fits");
  FILE* in = fopen(whole.c_str(), "rb");
  REQUIRE(in != NULL);
  unsigned char buffer[3000];
  size_t got = fread(buffer, 1, sizeof(buffer), in);
  fclose(in);
  REQUIRE(got == sizeof(buffer));

  std::string cut = out_path("truncated.fits");
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
