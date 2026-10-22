/* Tests for the JPEG XL driver in src/im_format_jxl.cpp.
 *
 * JPEG XL's lossless mode is exact for every type IM can hand it -- 8 bit,
 * 16 bit and 32 bit float -- so the lossless cases demand zero difference.
 * The lossy cases check geometry and that Quality is wired to something.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_jxl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>

namespace {

const int TEST_WIDTH  = 37;
const int TEST_HEIGHT = 23;

std::string out_path(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

std::string fixture(const char* name)
{
  return std::string(IM_TEST_FIXTURE_DIR) + "/" + name;
}

std::string example(const char* name)
{
  return std::string(IM_TEST_FIXTURE_DIR) + "/../../html/examples/" + name;
}

int plane_count(const imImage* image)
{
  return image->depth + (image->has_alpha? 1: 0);
}

double value_at(const imImage* image, int plane, int i)
{
  switch (image->data_type)
  {
  case IM_USHORT: return ((imushort**)image->data)[plane][i];
  case IM_FLOAT:  return ((float**)image->data)[plane][i];
  default:        return ((imbyte**)image->data)[plane][i];
  }
}

/* Ramps that use the whole range of each type, including values a narrower
   type could not hold -- a 16 bit file silently stored as 8 would lose them. */
void fill_pattern(imImage* image)
{
  int total = image->width * image->height;
  for (int p = 0; p < plane_count(image); p++)
  {
    for (int i = 0; i < total; i++)
    {
      int x = i % image->width, y = i / image->width;
      switch (image->data_type)
      {
      case IM_USHORT:
        ((imushort**)image->data)[p][i] = (imushort)((x * 1771 + y * 901 + p * 9000) & 0xFFFF);
        break;
      case IM_FLOAT:
        /* Not confined to 0..1, and not round binary fractions. */
        ((float**)image->data)[p][i] = (float)(x * 0.0371 - y * 0.013 + p * 0.25);
        break;
      default:
        ((imbyte**)image->data)[p][i] = (imbyte)((x * 6 + y * 3 + p * 50) & 0xFF);
        break;
      }
    }
  }
}

double max_difference(const imImage* a, const imImage* b)
{
  int total = a->width * a->height;
  double worst = 0;
  for (int p = 0; p < plane_count(a); p++)
    for (int i = 0; i < total; i++)
    {
      double diff = fabs(value_at(a, p, i) - value_at(b, p, i));
      if (diff > worst) worst = diff;
    }
  return worst;
}

double mean_difference(const imImage* a, const imImage* b, int planes)
{
  int total = a->width * a->height;
  double sum = 0;
  for (int p = 0; p < planes; p++)
    for (int i = 0; i < total; i++)
      sum += fabs(value_at(a, p, i) - value_at(b, p, i));
  return sum / ((double)total * planes);
}

imImage* round_trip(imImage* source, const char* file, int lossless,
                    int quality, int* error_out)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  imFile* handle = imFileNew(path.c_str(), "JXL", &error);
  if (!handle)
  {
    *error_out = error;
    return NULL;
  }

  imFileSetAttribute(handle, "Lossless", IM_INT, 1, &lossless);
  imFileSetAttribute(handle, "Quality", IM_INT, 1, &quality);

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

long file_size(const std::string& path)
{
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return -1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fclose(file);
  return size;
}

struct RegisterOnce
{
  RegisterOnce() { imFormatRegisterJXL(); }
};

/* imFormatRegisterJXL allocates and registers a driver on every call, so
   register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("JXL: CanWrite accepts what JPEG XL carries and nothing else")
{
  CHECK(imFormatCanWriteImage("JXL", "JXL", IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("JXL", "", IM_GRAY, IM_USHORT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("JXL", "", IM_RGB | IM_ALPHA, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("JXL", "", IM_BINARY, IM_BYTE) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("JXL", "", IM_RGB, IM_INT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("JXL", "", IM_RGB, IM_CFLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("JXL", "", IM_CMYK, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("JXL", "", IM_MAP, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("JXL", "JPEG", IM_RGB, IM_BYTE) == IM_ERR_COMPRESS);
}

TEST_CASE("JXL: lossless is exact for every type and colour space it accepts")
{
  struct Case { int color_mode, data_type; const char* file; };
  const Case cases[] = {
    { IM_GRAY,            IM_BYTE,   "rt_gray8.jxl"    },
    { IM_RGB,             IM_BYTE,   "rt_rgb8.jxl"     },
    { IM_RGB | IM_ALPHA,  IM_BYTE,   "rt_rgba8.jxl"    },
    { IM_GRAY | IM_ALPHA, IM_BYTE,   "rt_graya8.jxl"   },
    { IM_GRAY,            IM_USHORT, "rt_gray16.jxl"   },
    { IM_RGB | IM_ALPHA,  IM_USHORT, "rt_rgba16.jxl"   },
    { IM_GRAY,            IM_FLOAT,  "rt_grayf.jxl"    },
    { IM_RGB,             IM_FLOAT,  "rt_rgbf.jxl"     },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT,
                                    cases[c].color_mode, cases[c].data_type);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, cases[c].file, 1, 90, &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == TEST_WIDTH);
    CHECK(loaded->height == TEST_HEIGHT);
    CHECK(loaded->data_type == cases[c].data_type);
    CHECK(imColorModeSpace(loaded->color_space) == imColorModeSpace(cases[c].color_mode));
    CHECK((loaded->has_alpha != 0) == (imColorModeHasAlpha(cases[c].color_mode) != 0));
    CHECK(max_difference(source, loaded) == 0);

    imImageDestroy(loaded);
    imImageDestroy(source);
  }
}

TEST_CASE("JXL: binary is widened to black and white, not 0 and 1")
{
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_BINARY, IM_BYTE);
  REQUIRE(source != NULL);
  int total = TEST_WIDTH * TEST_HEIGHT;
  for (int i = 0; i < total; i++)
    ((imbyte**)source->data)[0][i] = (imbyte)(((i % TEST_WIDTH) / 4 + i / TEST_WIDTH) & 1);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_binary.jxl", 1, 90, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  /* Read back as gray -- or as binary, if IM's own 0/255 detection on gray
     byte images claims it. Either is right; 0 and 1 stored as gray is not. */
  int wrong = 0;
  int is_binary = imColorModeSpace(loaded->color_space) == IM_BINARY;
  for (int i = 0; i < total; i++)
  {
    double expected = ((imbyte**)source->data)[0][i]? (is_binary? 1: 255): 0;
    if (value_at(loaded, 0, i) != expected)
      wrong++;
  }
  CHECK(wrong == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("JXL: a lossy write keeps geometry, and Quality trades size for error")
{
  imImage* source = imImageCreate(64, 48, IM_RGB, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  const int qualities[2] = { 20, 95 };
  const char* files[2] = { "rt_lossy_low.jxl", "rt_lossy_high.jxl" };
  double error_at[2] = { 0, 0 };
  long size_at[2] = { 0, 0 };

  for (int q = 0; q < 2; q++)
  {
    CAPTURE(qualities[q]);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, files[q], 0, qualities[q], &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == 64);
    CHECK(loaded->height == 48);
    CHECK(loaded->data_type == IM_BYTE);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

    error_at[q] = mean_difference(source, loaded, 3);
    size_at[q] = file_size(out_path(files[q]));

    imImageDestroy(loaded);
  }

  CHECK(error_at[1] < error_at[0]);
  CHECK(size_at[1] > size_at[0]);
  CHECK(error_at[1] < 4.0);

  imImageDestroy(source);
}

TEST_CASE("JXL: files from cjxl decode correctly")
{
  /* Written by libjxl's own cjxl from the example images, so the read path
     is checked against an encoder this driver had no part in. */
  int error = IM_ERR_NONE;

  SUBCASE("lossless gray is exactly the source PNG")
  {
    imImage* png = imFileImageLoad(example("rice.png").c_str(), 0, &error);
    REQUIRE_MESSAGE(png != NULL, "could not load rice.png, error " << error);

    imImage* jxl = imFileImageLoad(fixture("external_gray_lossless.jxl").c_str(), 0, &error);
    REQUIRE_MESSAGE(jxl != NULL, "load failed, error " << error);

    REQUIRE(jxl->width == png->width);
    REQUIRE(jxl->height == png->height);
    CHECK(imColorModeSpace(jxl->color_space) == IM_GRAY);
    CHECK(jxl->data_type == IM_BYTE);
    CHECK(max_difference(png, jxl) == 0);

    imImageDestroy(png);
    imImageDestroy(jxl);
  }

  SUBCASE("16 bit gray reads as IM_USHORT with every bit")
  {
    /* A 200x24 ramp from ImageMagick, 65535 at the left edge to 0 at the
       right, losslessly encoded. Its values are not multiples of 257, so an
       8 bit path widened back to 16 could not reproduce them. */
    imImage* jxl = imFileImageLoad(fixture("external_ramp16_lossless.jxl").c_str(), 0, &error);
    REQUIRE_MESSAGE(jxl != NULL, "load failed, error " << error);

    REQUIRE(jxl->width == 200);
    REQUIRE(jxl->height == 24);
    CHECK(jxl->data_type == IM_USHORT);
    CHECK(imColorModeSpace(jxl->color_space) == IM_GRAY);

    const imushort* row = ((imushort**)jxl->data)[0];
    CHECK(row[0] == 65535);
    CHECK(row[1] == 65206);   /* read off the source PNG by ImageMagick */
    CHECK(row[199] == 0);

    int decreasing = 1;
    for (int x = 1; x < 200; x++)
      if (row[x] >= row[x - 1])
        decreasing = 0;
    CHECK(decreasing == 1);

    imImageDestroy(jxl);
  }

  SUBCASE("a recompressed JPEG decodes close to the JPEG itself")
  {
    /* cjxl's default for JPEG input stores the JPEG's own coefficients, so
       this is the path a real collection of converted photos takes. The two
       decoders differ in their IDCT rounding, not in content. */
    imImage* jpeg = imFileImageLoad(example("flower.jpg").c_str(), 0, &error);
    REQUIRE_MESSAGE(jpeg != NULL, "could not load flower.jpg, error " << error);

    imImage* jxl = imFileImageLoad(fixture("external_jpeg_recompressed.jxl").c_str(), 0, &error);
    REQUIRE_MESSAGE(jxl != NULL, "load failed, error " << error);

    REQUIRE(jxl->width == 184);
    REQUIRE(jxl->height == 148);
    CHECK(imColorModeSpace(jxl->color_space) == IM_RGB);

    double mean = mean_difference(jpeg, jxl, 3);
    CAPTURE(mean);
    CHECK(mean < 2.0);

    imImageDestroy(jpeg);
    imImageDestroy(jxl);
  }
}

TEST_CASE("JXL: a file that is not JPEG XL is declined")
{
  std::string path = out_path("not_jxl.bin");
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  unsigned char junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i * 7 + 3);
  fwrite(junk, 1, sizeof(junk), file);
  fclose(file);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "JXL", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("JXL: a truncated file fails cleanly")
{
  /* The header survives, so the decoder reports the geometry and then runs
     out of data mid-image. Under ASan this is the case that would show a
     read past the buffer. */
  std::string whole = fixture("external_gray_lossless.jxl");
  FILE* in = fopen(whole.c_str(), "rb");
  REQUIRE(in != NULL);
  unsigned char buffer[4096];
  size_t got = fread(buffer, 1, sizeof(buffer), in);
  fclose(in);
  REQUIRE(got == sizeof(buffer));

  std::string cut = out_path("truncated.jxl");
  FILE* out = fopen(cut.c_str(), "wb");
  REQUIRE(out != NULL);
  fwrite(buffer, 1, got, out);
  fclose(out);

  int error = IM_ERR_NONE;
  imImage* loaded = imFileImageLoad(cut.c_str(), 0, &error);

  /* imFileLoadImage hands back the allocated image even when the decode
     fails -- IM's convention, not this driver's -- so the error code is
     what says the pixels are not to be trusted. */
  CHECK(error != IM_ERR_NONE);
  if (loaded)
  {
    CHECK(loaded->width == 256);
    CHECK(loaded->height == 256);
    imImageDestroy(loaded);
  }
}
