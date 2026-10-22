/* Tests for the WebP driver in src/im_format_webp.cpp.
 *
 * Lossless WebP (VP8L) is exact, so every lossless case here demands zero
 * difference. Lossy (VP8) is checked for geometry and for an error bound
 * loose enough to survive a libwebp upgrade but far tighter than what a
 * broken pixel path produces.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_webp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

const int TEST_WIDTH  = 37;      /* odd, so a stride or chroma-block mistake */
const int TEST_HEIGHT = 23;      /* cannot hide behind an even size          */

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

imbyte sample(const imImage* image, int plane, int i)
{
  return ((imbyte**)image->data)[plane][i];
}

/* A ramp offset per plane, with a block of fully transparent pixels in the
   alpha plane whose colour is still distinctive -- the part libwebp throws
   away unless the driver asks it not to. */
void fill_pattern(imImage* image)
{
  int total = image->width * image->height;
  for (int p = 0; p < plane_count(image); p++)
  {
    for (int i = 0; i < total; i++)
    {
      int x = i % image->width, y = i / image->width;
      imbyte value = (imbyte)((x * 6 + y * 3 + p * 50) & 0xFF);

      if (image->has_alpha && p == image->depth)
        value = (x < 8)? 0: (imbyte)(128 + y);

      ((imbyte**)image->data)[p][i] = value;
    }
  }
}

int max_difference(const imImage* a, const imImage* b, int planes)
{
  int total = a->width * a->height, worst = 0;
  for (int p = 0; p < planes; p++)
  {
    for (int i = 0; i < total; i++)
    {
      int diff = (int)sample(a, p, i) - (int)sample(b, p, i);
      if (diff < 0) diff = -diff;
      if (diff > worst) worst = diff;
    }
  }
  return worst;
}

double mean_difference(const imImage* a, const imImage* b, int planes)
{
  int total = a->width * a->height;
  double sum = 0;
  for (int p = 0; p < planes; p++)
    for (int i = 0; i < total; i++)
      sum += abs((int)sample(a, p, i) - (int)sample(b, p, i));
  return sum / ((double)total * planes);
}

/* Saves through an explicit compression and attributes, then reloads. */
imImage* round_trip(imImage* source, const char* file, const char* compression,
                    int lossless, int quality, int* error_out)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  imFile* handle = imFileNew(path.c_str(), "WEBP", &error);
  if (!handle)
  {
    *error_out = error;
    return NULL;
  }

  if (compression)
    imFileSetInfo(handle, compression);
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
  RegisterOnce() { imFormatRegisterWebP(); }
};

/* imFormatRegisterWebP allocates and registers a driver on every call, so
   register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("WebP: CanWrite accepts what WebP carries and nothing else")
{
  CHECK(imFormatCanWriteImage("WEBP", "VP8", IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("WEBP", "VP8L", IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_RGB | IM_ALPHA, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_GRAY, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_BINARY, IM_BYTE) == IM_ERR_NONE);

  CHECK(imFormatCanWriteImage("WEBP", "", IM_RGB, IM_USHORT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_RGB, IM_FLOAT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_CMYK, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("WEBP", "", IM_MAP, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("WEBP", "JPEG", IM_RGB, IM_BYTE) == IM_ERR_COMPRESS);
}

TEST_CASE("WebP: lossless RGB round-trips exactly and reports VP8L")
{
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_RGB, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_rgb.webp", "VP8L", 0, 75, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  CHECK(loaded->width == TEST_WIDTH);
  CHECK(loaded->height == TEST_HEIGHT);
  CHECK(loaded->data_type == IM_BYTE);
  CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);
  CHECK(loaded->has_alpha == 0);
  CHECK(max_difference(source, loaded, 3) == 0);

  imImageDestroy(loaded);

  /* The file says what it is, rather than the driver assuming lossy. */
  imFile* handle = imFileOpen(out_path("rt_rgb.webp").c_str(), &error);
  REQUIRE(handle != NULL);
  char format[16] = { 0 }, compression[16] = { 0 };
  int image_count = 0;
  imFileGetInfo(handle, format, compression, &image_count);
  CHECK(strcmp(format, "WEBP") == 0);
  CHECK(strcmp(compression, "VP8L") == 0);
  CHECK(image_count == 1);
  imFileClose(handle);

  imImageDestroy(source);
}

TEST_CASE("WebP: the Lossless attribute is the same as asking for VP8L")
{
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_RGB, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_lossless_attr.webp", NULL, 1, 75, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);
  CHECK(max_difference(source, loaded, 3) == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("WebP: lossless alpha keeps the colour of transparent pixels")
{
  /* libwebp's default is to rewrite the RGB under alpha 0 to whatever
     compresses best. The driver sets "exact", so the whole image survives --
     a caller who later edits the alpha should not find the colour gone. */
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_RGB | IM_ALPHA, IM_BYTE);
  REQUIRE(source != NULL);
  REQUIRE(source->has_alpha != 0);
  fill_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_alpha.webp", "VP8L", 0, 75, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  CHECK(loaded->has_alpha != 0);
  CHECK(max_difference(source, loaded, 4) == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("WebP: gray is stored as RGB with three equal channels")
{
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_GRAY, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_gray.webp", "VP8L", 0, 75, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  /* WebP has no gray mode, so it cannot come back as gray. */
  CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

  int total = TEST_WIDTH * TEST_HEIGHT, wrong = 0;
  for (int i = 0; i < total; i++)
    for (int p = 0; p < 3; p++)
      if (sample(loaded, p, i) != sample(source, 0, i))
        wrong++;
  CHECK(wrong == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("WebP: binary is widened to black and white, not 0 and 1")
{
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_BINARY, IM_BYTE);
  REQUIRE(source != NULL);
  int total = TEST_WIDTH * TEST_HEIGHT;
  for (int i = 0; i < total; i++)
    ((imbyte**)source->data)[0][i] = (imbyte)(((i % TEST_WIDTH) / 4 + i / TEST_WIDTH) & 1);

  int error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "rt_binary.webp", "VP8L", 0, 75, &error);
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

  int wrong = 0;
  for (int i = 0; i < total; i++)
  {
    imbyte expected = sample(source, 0, i)? 255: 0;
    for (int p = 0; p < 3; p++)
      if (sample(loaded, p, i) != expected)
        wrong++;
  }
  CHECK(wrong == 0);

  imImageDestroy(loaded);
  imImageDestroy(source);
}

TEST_CASE("WebP: a lossy write keeps geometry, and Quality trades size for error")
{
  imImage* source = imImageCreate(64, 48, IM_RGB, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  const int qualities[2] = { 10, 95 };
  const char* files[2] = { "rt_lossy_low.webp", "rt_lossy_high.webp" };
  double error_at[2] = { 0, 0 };
  long size_at[2] = { 0, 0 };

  for (int q = 0; q < 2; q++)
  {
    CAPTURE(qualities[q]);

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, files[q], "VP8", 0, qualities[q], &error);
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << error);

    CHECK(loaded->width == 64);
    CHECK(loaded->height == 48);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

    error_at[q] = mean_difference(source, loaded, 3);
    size_at[q] = file_size(out_path(files[q]));

    imImageDestroy(loaded);
  }

  /* Both directions, so an attribute that was read and then ignored fails. */
  CHECK(error_at[1] < error_at[0]);
  CHECK(size_at[1] > size_at[0]);

  /* Measured at 7.42, and cwebp -q 95 on the same pixels gives the same
     7.42 (ImageMagick's MAE of its dwebp output) -- 4:2:0 chroma on a pattern
     with hard wraparound edges, not the driver. A broken interleave would be
     tens of levels out. */
  CHECK(error_at[1] < 9.0);

  imImageDestroy(source);
}

TEST_CASE("WebP: files from cwebp decode correctly")
{
  /* Round-trips pass just as happily when the read and write paths are wrong
     in matching ways. These were written by libwebp's own cwebp, from the
     example images, so they check the read path against an encoder this
     driver had no part in. */
  int error = IM_ERR_NONE;

  SUBCASE("lossless gray is exactly the source PNG")
  {
    imImage* png = imFileImageLoad(example("rice.png").c_str(), 0, &error);
    REQUIRE_MESSAGE(png != NULL, "could not load rice.png, error " << error);

    imImage* webp = imFileImageLoad(fixture("external_gray_lossless.webp").c_str(), 0, &error);
    REQUIRE_MESSAGE(webp != NULL, "load failed, error " << error);

    REQUIRE(webp->width == png->width);
    REQUIRE(webp->height == png->height);
    CHECK(imColorModeSpace(webp->color_space) == IM_RGB);

    int total = png->width * png->height, wrong = 0;
    for (int i = 0; i < total; i++)
      for (int p = 0; p < 3; p++)
        if (sample(webp, p, i) != sample(png, 0, i))
          wrong++;
    CHECK(wrong == 0);

    imImageDestroy(png);
    imImageDestroy(webp);
  }

  SUBCASE("lossy RGB is close to the source JPEG")
  {
    imImage* jpeg = imFileImageLoad(example("flower.jpg").c_str(), 0, &error);
    REQUIRE_MESSAGE(jpeg != NULL, "could not load flower.jpg, error " << error);

    imImage* webp = imFileImageLoad(fixture("external_rgb_lossy.webp").c_str(), 0, &error);
    REQUIRE_MESSAGE(webp != NULL, "load failed, error " << error);

    REQUIRE(webp->width == 184);
    REQUIRE(webp->height == 148);
    CHECK(imColorModeSpace(webp->color_space) == IM_RGB);

    /* Encoded at -q 80. Measured at 6.665, which is exactly what ImageMagick
       reports between flower.jpg and dwebp's decode of this file, so the
       read path adds nothing. Swapped channels or a row offset would be far
       outside this; a flat decode would be too. */
    double mean = mean_difference(jpeg, webp, 3);
    CAPTURE(mean);
    CHECK(mean < 8.0);

    imImageDestroy(jpeg);
    imImageDestroy(webp);
  }
}

TEST_CASE("WebP: an animation reads frame by frame, in either order")
{
  /* img2webp, lossless: a red 32x24 frame followed by a blue one. Reading
     frame 1 before frame 0 makes the decoder start over, which is the path
     most likely to hand back the wrong frame. */
  int error = IM_ERR_NONE;
  imFile* handle = imFileOpen(fixture("external_anim.webp").c_str(), &error);
  REQUIRE_MESSAGE(handle != NULL, "open failed, error " << error);

  char compression[16] = { 0 };
  int image_count = 0;
  imFileGetInfo(handle, NULL, compression, &image_count);
  CHECK(image_count == 2);

  const int order[3] = { 1, 0, 1 };
  for (int k = 0; k < 3; k++)
  {
    int index = order[k];
    CAPTURE(index);

    imImage* frame = imFileLoadImage(handle, index, &error);
    REQUIRE_MESSAGE(frame != NULL, "frame load failed, error " << error);
    CHECK(frame->width == 32);
    CHECK(frame->height == 24);

    int center = 12 * 32 + 16;
    if (index == 0)
    {
      CHECK(sample(frame, 0, center) == 255);
      CHECK(sample(frame, 2, center) == 0);
    }
    else
    {
      CHECK(sample(frame, 0, center) == 0);
      CHECK(sample(frame, 2, center) == 255);
    }

    imImageDestroy(frame);
  }

  imFileClose(handle);
}

TEST_CASE("WebP: a file that is not WebP is declined")
{
  std::string path = out_path("not_webp.bin");
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  unsigned char junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i * 7);
  fwrite(junk, 1, sizeof(junk), file);
  fclose(file);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "WEBP", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("WebP: a truncated file fails cleanly")
{
  /* Half of a real file, so the RIFF header is valid and libwebp gets as far
     as the bitstream before running out. Under ASan this is the case that
     would show a read past the buffer. */
  std::string whole = fixture("external_rgb_lossy.webp");
  FILE* in = fopen(whole.c_str(), "rb");
  REQUIRE(in != NULL);
  unsigned char buffer[4096];
  size_t got = fread(buffer, 1, sizeof(buffer), in);
  fclose(in);
  REQUIRE(got == sizeof(buffer));

  std::string cut = out_path("truncated.webp");
  FILE* out = fopen(cut.c_str(), "wb");
  REQUIRE(out != NULL);
  fwrite(buffer, 1, got, out);
  fclose(out);

  int error = IM_ERR_NONE;
  imImage* loaded = imFileImageLoad(cut.c_str(), 0, &error);

  /* The geometry is in the header, so ReadImageInfo succeeds and the decode
     fails. imFileLoadImage hands back the allocated image even then -- that is
     IM's convention, not this driver's -- so the error code is what says the
     pixels are not to be trusted. */
  CHECK(error != IM_ERR_NONE);
  if (loaded)
  {
    CHECK(loaded->width == 184);
    CHECK(loaded->height == 148);
    imImageDestroy(loaded);
  }
}
