/* Tests for the QOI driver in src/im_format_qoi.cpp.
 *
 * QOI is lossless, so every comparison here is exact. The external fixtures
 * come from ImageMagick (see make_qoi_fits_dng_fixtures.py); between them
 * they use all six chunk types, so the decoder's every branch is checked
 * against an encoder it shares no code with.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>

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

std::string example(const char* name)
{
  return std::string(IM_TEST_FIXTURE_DIR) + "/../../html/examples/" + name;
}

int plane_count(const imImage* image)
{
  return image->depth + (image->has_alpha? 1: 0);
}

imbyte* plane(const imImage* image, int c)
{
  return ((imbyte**)image->data)[c];
}

int count_differences(const imImage* a, const imImage* b, int planes)
{
  int total = a->width * a->height, different = 0;
  for (int c = 0; c < planes; c++)
    for (int i = 0; i < total; i++)
      if (plane(a, c)[i] != plane(b, c)[i])
        different++;
  return different;
}

/* Content chosen to make the encoder use every chunk type: a flat band for
   runs (long enough to cross the 62 pixel run limit and a row boundary), a
   repeating pattern for index hits, gentle and steeper gradients for DIFF and
   LUMA, and jumps for full RGB -- and, with alpha, alpha changes for RGBA. */
void fill_pattern(imImage* image)
{
  int w = image->width, h = image->height;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
    {
      int i = y * w + x;
      for (int c = 0; c < plane_count(image); c++)
      {
        int v;
        if (image->has_alpha && c == image->depth)
          v = (y < 3)? 255: (x * 9 + y * 5) & 0xFF;
        else if (y < 3)
          v = 77;                                   /* runs */
        else if (y < 6)
          v = ((x % 4) * 60 + c * 20) & 0xFF;       /* index hits */
        else if (y < 9)
          v = (x + c) & 0xFF;                       /* DIFF */
        else if (y < 12)
          v = (x * 7 + c * 3) & 0xFF;               /* LUMA */
        else
          v = (x * 53 + y * 29 + c * 101) & 0xFF;   /* RGB */
        plane(image, c)[i] = (imbyte)v;
      }
    }
}

imImage* round_trip(imImage* source, const char* file, int* error_out)
{
  std::string path = out_path(file);
  int error = imFileImageSave(path.c_str(), "QOI", source);
  if (error != IM_ERR_NONE)
  {
    *error_out = error;
    return NULL;
  }

  imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
  *error_out = loaded? IM_ERR_NONE: error;
  return loaded;
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

void write_bytes(const std::string& path, const std::vector<unsigned char>& bytes)
{
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  if (!bytes.empty())
    fwrite(&bytes[0], 1, bytes.size(), file);
  fclose(file);
}

} /* namespace */

TEST_CASE("QOI: CanWrite accepts 8 bit RGB, gray and binary")
{
  CHECK(imFormatCanWriteImage("QOI", "QOI", IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("QOI", "", IM_RGB | IM_ALPHA, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("QOI", "", IM_GRAY, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("QOI", "", IM_BINARY, IM_BYTE) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("QOI", "", IM_RGB, IM_USHORT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("QOI", "", IM_MAP, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("QOI", "", IM_CMYK, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("QOI", "RLE", IM_RGB, IM_BYTE) == IM_ERR_COMPRESS);
}

TEST_CASE("QOI: RGB and RGBA round-trip exactly")
{
  /* 70 wide, so the flat rows hold a run longer than the 62 pixel limit
     that also continues across a row boundary. */
  const int modes[2] = { IM_RGB, IM_RGB | IM_ALPHA };
  const char* files[2] = { "rt_rgb.qoi", "rt_rgba.qoi" };

  for (int m = 0; m < 2; m++)
  {
    CAPTURE(files[m]);

    imImage* source = imImageCreate(70, 23, modes[m], IM_BYTE);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, files[m], &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == 70);
    CHECK(loaded->height == 23);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);
    CHECK((loaded->has_alpha != 0) == (m == 1));
    CHECK(count_differences(source, loaded, plane_count(source)) == 0);

    imImageDestroy(loaded);
    imImageDestroy(source);
  }
}

TEST_CASE("QOI: the written file has the specification's header and end marker")
{
  /* The header is fixed by the spec, and the stream must end with the
     seven zeros and a one. A malformed ending is what another decoder
     would reject first. */
  imImage* source = imImageCreate(70, 23, IM_RGB | IM_ALPHA, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  std::string path = out_path("layout.qoi");
  REQUIRE(imFileImageSave(path.c_str(), "QOI", source) == IM_ERR_NONE);
  imImageDestroy(source);

  std::vector<unsigned char> bytes = read_bytes(path);
  REQUIRE(bytes.size() > 22);
  CHECK(memcmp(&bytes[0], "qoif", 4) == 0);
  CHECK(bytes[4] == 0);  CHECK(bytes[7] == 70);   /* width, big endian */
  CHECK(bytes[8] == 0);  CHECK(bytes[11] == 23);  /* height */
  CHECK(bytes[12] == 4);                          /* channels */
  CHECK(bytes[13] == 0);                          /* sRGB */

  static const unsigned char end[8] = { 0, 0, 0, 0, 0, 0, 0, 1 };
  CHECK(memcmp(&bytes[bytes.size() - 8], end, 8) == 0);
}

TEST_CASE("QOI: gray and binary are stored as RGB")
{
  imImage* gray = imImageCreate(37, 11, IM_GRAY, IM_BYTE);
  REQUIRE(gray != NULL);
  fill_pattern(gray);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(gray, "rt_gray.qoi", &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);
  CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

  int total = 37 * 11, wrong = 0;
  for (int i = 0; i < total; i++)
    for (int c = 0; c < 3; c++)
      if (plane(loaded, c)[i] != plane(gray, 0)[i])
        wrong++;
  CHECK(wrong == 0);
  imImageDestroy(loaded);
  imImageDestroy(gray);

  imImage* binary = imImageCreate(37, 11, IM_BINARY, IM_BYTE);
  REQUIRE(binary != NULL);
  for (int i = 0; i < total; i++)
    plane(binary, 0)[i] = (imbyte)((i / 3) & 1);

  loaded = round_trip(binary, "rt_binary.qoi", &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);
  wrong = 0;
  for (int i = 0; i < total; i++)
    for (int c = 0; c < 3; c++)
      if (plane(loaded, c)[i] != (plane(binary, 0)[i]? 255: 0))
        wrong++;
  CHECK(wrong == 0);
  imImageDestroy(loaded);
  imImageDestroy(binary);
}

TEST_CASE("QOI: files from ImageMagick decode exactly")
{
  int error = IM_ERR_NONE;

  SUBCASE("3 channels, from a gray PNG")
  {
    imImage* png = imFileImageLoad(example("rice.png").c_str(), 0, &error);
    REQUIRE_MESSAGE(png != NULL, "could not load rice.png, error " << error);
    imImage* qoi = imFileImageLoad(fixture("qoi_rgb.qoi").c_str(), 0, &error);
    REQUIRE_MESSAGE(qoi != NULL, "load failed, error " << error);

    REQUIRE(qoi->width == png->width);
    REQUIRE(qoi->height == png->height);
    CHECK(imColorModeSpace(qoi->color_space) == IM_RGB);
    CHECK(qoi->has_alpha == 0);

    int total = png->width * png->height, wrong = 0;
    for (int i = 0; i < total; i++)
      for (int c = 0; c < 3; c++)
        if (plane(qoi, c)[i] != plane(png, 0)[i])
          wrong++;
    CHECK(wrong == 0);

    imImageDestroy(png);
    imImageDestroy(qoi);
  }

  SUBCASE("4 channels, against the PNG it was made from")
  {
    imImage* png = imFileImageLoad(fixture("qoi_rgba_source.png").c_str(), 0, &error);
    REQUIRE_MESSAGE(png != NULL, "could not load the source PNG, error " << error);
    imImage* qoi = imFileImageLoad(fixture("qoi_rgba.qoi").c_str(), 0, &error);
    REQUIRE_MESSAGE(qoi != NULL, "load failed, error " << error);

    REQUIRE(qoi->width == 33);
    REQUIRE(qoi->height == 21);
    REQUIRE(png->has_alpha != 0);
    REQUIRE(qoi->has_alpha != 0);
    CHECK(count_differences(png, qoi, 4) == 0);

    imImageDestroy(png);
    imImageDestroy(qoi);
  }
}

TEST_CASE("QOI: a file that is not QOI is declined")
{
  std::vector<unsigned char> junk(512);
  for (size_t i = 0; i < junk.size(); i++) junk[i] = (unsigned char)(i * 17 + 3);
  std::string path = out_path("not_qoi.bin");
  write_bytes(path, junk);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "QOI", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("QOI: impossible headers are refused before any pixel is read")
{
  /* A zero dimension, one past the spec's 400 megapixel limit, and a channel
     count that is neither 3 nor 4. */
  struct Case { const char* name; unsigned w, h; unsigned char channels; };
  const Case cases[3] = {
    { "zero width",   0,     10,    3 },
    { "too large",    40000, 10001, 3 },
    { "two channels", 10,    10,    2 },
  };

  for (int k = 0; k < 3; k++)
  {
    CAPTURE(cases[k].name);

    std::vector<unsigned char> bytes(14 + 8 + 16, 0);
    memcpy(&bytes[0], "qoif", 4);
    for (int b = 0; b < 4; b++)
    {
      bytes[4 + b] = (unsigned char)(cases[k].w >> (24 - 8 * b));
      bytes[8 + b] = (unsigned char)(cases[k].h >> (24 - 8 * b));
    }
    bytes[12] = cases[k].channels;

    std::string path = out_path("bad_header.qoi");
    write_bytes(path, bytes);

    int error = IM_ERR_NONE;
    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
    CHECK(loaded == NULL);
    CHECK(error == IM_ERR_DATA);
    if (loaded)
      imImageDestroy(loaded);
  }
}

TEST_CASE("QOI: a truncated stream fails cleanly")
{
  /* Cut mid-stream, and cut to the header alone. Under ASan, a decoder
     without its bounds checks reads past the buffer in both. */
  std::vector<unsigned char> whole = read_bytes(fixture("qoi_rgb.qoi"));
  const size_t cuts[2] = { whole.size() / 2, 15 };

  for (int k = 0; k < 2; k++)
  {
    CAPTURE(cuts[k]);
    std::vector<unsigned char> cut(whole.begin(), whole.begin() + (long)cuts[k]);
    std::string path = out_path("truncated.qoi");
    write_bytes(path, cut);

    int error = IM_ERR_NONE;
    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);

    /* IM hands back the allocated image even when the data read fails; the
       error code is what says not to trust it. */
    CHECK(error != IM_ERR_NONE);
    if (loaded)
      imImageDestroy(loaded);
  }
}
