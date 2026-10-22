/* Tests for the Radiance HDR driver in src/im_format_hdr.cpp.
 *
 * RGBE keeps 8 bits of mantissa per channel against a shared exponent, so a
 * channel is accurate to about 1/256 of the brightest channel in its pixel --
 * not of itself. Every tolerance here is stated that way. The fixtures come
 * from ImageMagick and from hand-assembled bytes; see make_float_fixtures.py.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

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

void fill_fixture_pattern(imImage* image)
{
  for (int c = 0; c < image->depth; c++)
    for (int y = 0; y < image->height; y++)
      for (int x = 0; x < image->width; x++)
        at(image, c, x, y) = (float)fixture_value(x, y, c);
}

/* Worst error in units of each pixel's brightest channel: what RGBE's shared
   exponent actually bounds. 1/128 allows the mantissa's half-step rounding
   on both the writer's side and this reader's. */
double worst_relative_error(const imImage* expected, const imImage* actual)
{
  double worst = 0;
  int total = expected->width * expected->height;
  for (int i = 0; i < total; i++)
  {
    double peak = 0;
    for (int c = 0; c < expected->depth; c++)
      if (plane(expected, c)[i] > peak)
        peak = plane(expected, c)[i];
    if (peak <= 0)
      continue;

    for (int c = 0; c < expected->depth; c++)
    {
      double error = fabs(plane(expected, c)[i] - plane(actual, c)[i]) / peak;
      if (error > worst)
        worst = error;
    }
  }
  return worst;
}

imImage* round_trip(imImage* source, const char* file, const char* compression,
                    int* error_out)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  imFile* handle = imFileNew(path.c_str(), "HDR", &error);
  if (!handle)
  {
    *error_out = error;
    return NULL;
  }
  if (compression)
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

void write_bytes(const std::string& path, const std::vector<unsigned char>& bytes)
{
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  fwrite(&bytes[0], 1, bytes.size(), file);
  fclose(file);
}

std::vector<unsigned char> read_bytes(const std::string& path)
{
  std::vector<unsigned char> bytes;
  FILE* file = fopen(path.c_str(), "rb");
  REQUIRE(file != NULL);
  int c;
  while ((c = fgetc(file)) != EOF)
    bytes.push_back((unsigned char)c);
  fclose(file);
  return bytes;
}

std::vector<unsigned char> text(const char* s)
{
  return std::vector<unsigned char>(s, s + strlen(s));
}

} /* namespace */

TEST_CASE("HDR: CanWrite accepts float RGB, XYZ and gray, nothing else")
{
  CHECK(imFormatCanWriteImage("HDR", "RLE", IM_RGB, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("HDR", "NONE", IM_RGB, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("HDR", "", IM_XYZ, IM_FLOAT) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("HDR", "", IM_GRAY, IM_FLOAT) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("HDR", "", IM_RGB, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HDR", "", IM_RGB, IM_DOUBLE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HDR", "", IM_RGB | IM_ALPHA, IM_FLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HDR", "", IM_CMYK, IM_FLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HDR", "LZW", IM_RGB, IM_FLOAT) == IM_ERR_COMPRESS);
}

TEST_CASE("HDR: RGB round-trips within RGBE precision, with and without RLE")
{
  /* Width 40 takes the run-length path; the file is also reopened to check
     the compression it reports is the one it was written with. */
  const char* compressions[2] = { "RLE", "NONE" };
  const char* files[2] = { "rt_rle.hdr", "rt_flat.hdr" };

  for (int k = 0; k < 2; k++)
  {
    CAPTURE(compressions[k]);

    imImage* source = imImageCreate(40, 30, IM_RGB, IM_FLOAT);
    REQUIRE(source != NULL);
    fill_fixture_pattern(source);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, files[k], compressions[k], &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == 40);
    CHECK(loaded->height == 30);
    CHECK(loaded->data_type == IM_FLOAT);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);
    CHECK(worst_relative_error(source, loaded) <= 1.0 / 128);

    imFile* handle = imFileOpen(out_path(files[k]).c_str(), &error);
    REQUIRE(handle != NULL);
    char format[16] = { 0 }, compression[16] = { 0 };
    imFileGetInfo(handle, format, compression, NULL);
    CHECK(strcmp(format, "HDR") == 0);
    CHECK(strcmp(compression, compressions[k]) == 0);
    imFileClose(handle);

    imImageDestroy(loaded);
    imImageDestroy(source);
  }
}

TEST_CASE("HDR: long runs and short widths both survive the encoder")
{
  /* Flat rows exercise the run branch of the RLE encoder, and a width below
     8 forces flat scanlines even when RLE is asked for. 300 crosses the
     127-byte run limit and the 128-byte literal limit. */
  const int widths[3] = { 300, 7, 8 };

  for (int k = 0; k < 3; k++)
  {
    int w = widths[k];
    CAPTURE(w);

    imImage* source = imImageCreate(w, 5, IM_RGB, IM_FLOAT);
    REQUIRE(source != NULL);
    for (int c = 0; c < 3; c++)
      for (int y = 0; y < 5; y++)
        for (int x = 0; x < w; x++)
        {
          /* Row 0 constant, row 1 a run broken every 200 pixels, row 2 all
             distinct, rows 3-4 alternating -- every shape the run finder
             has to decide about. */
          float v;
          switch (y)
          {
          case 0:  v = 1.5f; break;
          case 1:  v = (x / 200) + 0.25f; break;
          case 2:  v = 0.01f * (x + 1) * (c + 1); break;
          default: v = (x % 2)? 3.0f: 0.75f; break;
          }
          plane(source, c)[y * w + x] = v;
        }

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, "rt_runs.hdr", "RLE", &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);
    CHECK(worst_relative_error(source, loaded) <= 1.0 / 128);

    imImageDestroy(loaded);
    imImageDestroy(source);
  }
}

TEST_CASE("HDR: XYZ is written as xyze and comes back as XYZ")
{
  imImage* source = imImageCreate(16, 8, IM_XYZ, IM_FLOAT);
  REQUIRE(source != NULL);
  fill_fixture_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_xyz.hdr", NULL, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  CHECK(imColorModeSpace(loaded->color_space) == IM_XYZ);
  CHECK(worst_relative_error(source, loaded) <= 1.0 / 128);

  std::vector<unsigned char> bytes = read_bytes(out_path("rt_xyz.hdr"));
  std::string header(bytes.begin(), bytes.begin() + 64);
  CHECK(header.find("FORMAT=32-bit_rle_xyze") != std::string::npos);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("HDR: gray is written as three equal channels")
{
  imImage* source = imImageCreate(16, 8, IM_GRAY, IM_FLOAT);
  REQUIRE(source != NULL);
  fill_fixture_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_gray.hdr", NULL, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

  int total = 16 * 8, unequal = 0;
  double worst = 0;
  for (int i = 0; i < total; i++)
  {
    if (plane(loaded, 0)[i] != plane(loaded, 1)[i] ||
        plane(loaded, 0)[i] != plane(loaded, 2)[i])
      unequal++;
    double error_i = fabs(plane(loaded, 0)[i] - plane(source, 0)[i]) / plane(source, 0)[i];
    if (error_i > worst) worst = error_i;
  }
  CHECK(unequal == 0);
  CHECK(worst <= 1.0 / 128);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("HDR: values RGBE cannot hold are stored as their nearest neighbour")
{
  /* Negative and NaN have no RGBE form and become 0. Beyond 2^127 clamps to
     the largest value rather than wrapping the exponent round to a tiny one,
     and below about 2^-128 rounds to 0. */
  imImage* source = imImageCreate(8, 1, IM_RGB, IM_FLOAT);
  REQUIRE(source != NULL);
  const float values[8] = { 0.0f, -1.0f, NAN, 1e38f, INFINITY, 1e-40f, 1.0f, 65504.0f };
  for (int c = 0; c < 3; c++)
    for (int x = 0; x < 8; x++)
      plane(source, c)[x] = values[x];

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_edges.hdr", NULL, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  const float* r = plane(loaded, 0);
  CHECK(r[0] == 0.0f);
  CHECK(r[1] == 0.0f);
  CHECK(r[2] == 0.0f);
  CHECK(r[3] > 1e37f);
  CHECK(r[4] > 1e37f);
  CHECK(!isinf(r[4]));
  CHECK(r[5] == 0.0f);
  CHECK(fabs(r[6] - 1.0f) <= 1.0f / 128);
  CHECK(fabs(r[7] - 65504.0f) <= 65504.0f / 128);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("HDR: files from ImageMagick decode correctly")
{
  /* Written by ImageMagick from a PFM of fixture_value(), so the reader is
     checked against a writer it had no part in -- both its RLE scanlines
     (40 wide) and its flat ones (5 wide). Its header carries GAMMA= and
     PRIMARIES= lines, which must be skipped. */
  struct Case { const char* file; int width, height; const char* compression; };
  const Case cases[2] = {
    { "hdr_rle.hdr",  40, 30, "RLE"  },
    { "hdr_flat.hdr",  5,  4, "NONE" },
  };

  for (int k = 0; k < 2; k++)
  {
    CAPTURE(cases[k].file);

    int error = IM_ERR_NONE;
    imFile* handle = imFileOpen(fixture(cases[k].file).c_str(), &error);
    REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);
    char compression[16] = { 0 };
    imFileGetInfo(handle, NULL, compression, NULL);
    CHECK(strcmp(compression, cases[k].compression) == 0);

    imImage* loaded = imFileLoadImage(handle, 0, &error);
    imFileClose(handle);
    REQUIRE_MESSAGE(loaded != NULL, "load failed, error " << error);

    REQUIRE(loaded->width == cases[k].width);
    REQUIRE(loaded->height == cases[k].height);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

    imImage* expected = imImageCreate(cases[k].width, cases[k].height, IM_RGB, IM_FLOAT);
    fill_fixture_pattern(expected);
    CHECK(worst_relative_error(expected, loaded) <= 1.0 / 128);

    imImageDestroy(expected);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HDR: old-style run-length files are read, and EXPOSURE is reported")
{
  /* No current writer produces old-style runs, so the fixture is hand-built
     bytes: row 0 one pixel then a run of 11 repeating it, row 1 two pixels
     then a run of 10 repeating the second. Exponent 129 means a mantissa m
     decodes to exactly (m + 0.5) / 128. */
  int error = IM_ERR_NONE;
  imFile* handle = imFileOpen(fixture("hdr_old_rle.hdr").c_str(), &error);
  REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);

  imImage* loaded = imFileLoadImage(handle, 0, &error);
  REQUIRE_MESSAGE(loaded != NULL, "load failed, error " << error);
  REQUIRE(loaded->width == 12);
  REQUIRE(loaded->height == 2);

  /* Two EXPOSURE= lines, 2.0 and 1.5: the attribute is their product. */
  int count = 0, type = 0;
  const float* exposure = (const float*)imFileGetAttribute(handle, "Exposure", &type, &count);
  REQUIRE(exposure != NULL);
  CHECK(type == IM_FLOAT);
  CHECK(*exposure == 3.0f);
  imFileClose(handle);

  for (int x = 0; x < 12; x++)
  {
    CAPTURE(x);
    CHECK(at(loaded, 0, x, 0) == 64.5f / 128);
    CHECK(at(loaded, 1, x, 0) == 128.5f / 128);
    CHECK(at(loaded, 2, x, 0) == 192.5f / 128);

    static const float first[3] = { 10.5f, 20.5f, 30.5f };
    static const float rest[3] = { 40.5f, 50.5f, 60.5f };
    const float* expected = (x == 0)? first: rest;
    for (int c = 0; c < 3; c++)
      CHECK(at(loaded, c, x, 1) == expected[c] / 128);
  }

  imImageDestroy(loaded);
}

TEST_CASE("HDR: a bottom-up file is read the right way up")
{
  /* Rewrite the resolution string of a file written top-down. The same bytes
     now describe the image upside down, so the loaded rows must reverse. */
  imImage* source = imImageCreate(12, 6, IM_RGB, IM_FLOAT);
  REQUIRE(source != NULL);
  fill_fixture_pattern(source);

  std::string path = out_path("bottom_up.hdr");
  REQUIRE(imFileImageSave(path.c_str(), "HDR", source) == IM_ERR_NONE);

  std::vector<unsigned char> bytes = read_bytes(path);
  std::string as_text(bytes.begin(), bytes.end());
  size_t at = as_text.find("-Y 6 +X 12\n");
  REQUIRE(at != std::string::npos);
  bytes[at] = '+';
  write_bytes(path, bytes);

  int error = IM_ERR_NONE;
  imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
  REQUIRE_MESSAGE(loaded != NULL, "load failed, error " << error);

  int wrong = 0;
  for (int y = 0; y < 6; y++)
    for (int x = 0; x < 12; x++)
    {
      double expected = plane(source, 0)[(5 - y) * 12 + x];
      if (fabs(plane(loaded, 0)[y * 12 + x] - expected) > expected / 64)
        wrong++;
    }
  CHECK(wrong == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("HDR: a file that is not HDR is declined")
{
  std::string path = out_path("not_hdr.bin");
  std::vector<unsigned char> junk(512);
  for (size_t i = 0; i < junk.size(); i++) junk[i] = (unsigned char)(i * 13 + 5);
  write_bytes(path, junk);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "HDR", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("HDR: damaged scanlines fail cleanly instead of reading past the data")
{
  /* Each of these has a valid header and a scanline that lies about itself.
     Under ASan, any one of them decoded without its bounds check is an
     out-of-bounds read or write; here it has to be an error code. */
  const char* header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n";

  struct Case { const char* name; std::vector<unsigned char> body; };
  std::vector<Case> cases;

  /* A run of 72 into a scanline of 8. */
  cases.push_back({ "run past the end", { 2, 2, 0, 8, 200, 1 } });

  /* Literals claiming more bytes than the file has. */
  cases.push_back({ "literal past the file", { 2, 2, 0, 8, 8, 1, 2, 3 } });

  /* An RLE marker announcing a different width. */
  cases.push_back({ "wrong width", { 2, 2, 0, 9, 136, 1, 136, 1, 136, 1, 136, 1 } });

  /* An old-style run before any pixel to repeat. */
  cases.push_back({ "old run first", { 1, 1, 1, 8 } });

  /* Old-style run past the width. */
  cases.push_back({ "old run too long", { 9, 9, 9, 130, 1, 1, 1, 200 } });

  /* A zero-length literal, which would never advance. */
  cases.push_back({ "zero count", { 2, 2, 0, 8, 0, 0, 0, 0 } });

  for (size_t k = 0; k < cases.size(); k++)
  {
    CAPTURE(cases[k].name);

    std::vector<unsigned char> bytes = text(header);
    bytes.insert(bytes.end(), cases[k].body.begin(), cases[k].body.end());
    std::string path = out_path("damaged.hdr");
    write_bytes(path, bytes);

    int error = IM_ERR_NONE;
    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);

    /* IM hands back the allocated image even when the data read fails; the
       error code is what says not to trust it. */
    CHECK(error != IM_ERR_NONE);
    if (loaded)
      imImageDestroy(loaded);
  }
}

TEST_CASE("HDR: orientations that mirror or transpose are refused")
{
  const char* resolutions[3] = { "-Y 1 -X 8\n", "+X 8 -Y 1\n", "-Y 0 +X 8\n" };
  for (int k = 0; k < 3; k++)
  {
    CAPTURE(resolutions[k]);

    std::vector<unsigned char> bytes = text("#?RADIANCE\n\n");
    std::vector<unsigned char> resolution = text(resolutions[k]);
    bytes.insert(bytes.end(), resolution.begin(), resolution.end());
    bytes.insert(bytes.end(), 32, 0x80);

    std::string path = out_path("orientation.hdr");
    write_bytes(path, bytes);

    int error = IM_ERR_NONE;
    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
    CHECK(loaded == NULL);
    CHECK(error == IM_ERR_DATA);
    if (loaded)
      imImageDestroy(loaded);
  }
}
