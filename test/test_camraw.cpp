/* Tests for the camera raw driver in src/im_format_camraw.cpp.
 *
 * There is no small, freely licensed real camera file to commit, so the
 * fixture is a DNG built to the specification by tifffile (see
 * make_qoi_fits_dng_fixtures.py): a 64x48 RGGB Bayer mosaic of three
 * vertical bands -- red, green, blue. The colour matrix makes the "camera"
 * sRGB itself, so a correct decode gives those three colours. A wrong CFA
 * phase or a swapped channel order shows up as the wrong colours, and that,
 * not exact values, is what LibRaw's demosaicing lets a test pin down.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_camraw.h>

#include <stdio.h>
#include <string.h>
#include <string>

namespace {

std::string fixture(const char* name)
{
  return std::string(IM_TEST_FIXTURE_DIR) + "/" + name;
}

std::string out_path(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

/* Channel c at (x, y) with y counted from the top, as the file is. */
int at(const imImage* image, int c, int x, int y)
{
  return ((imushort**)image->data)[c][(image->height - 1 - y) * image->width + x];
}

/* Loads the fixture, optionally with RawHalfSize, as imFileImageLoad would
   but with the attribute set in between -- the only point it can be. */
imImage* load_bands(int half_size, imFile** handle_out, int* error)
{
  imFile* handle = imFileOpen(fixture("camraw_bands.dng").c_str(), error);
  if (!handle)
    return NULL;

  if (half_size)
  {
    int one = 1;
    imFileSetAttribute(handle, "RawHalfSize", IM_INT, 1, &one);
  }

  imImage* image = imFileLoadImage(handle, 0, error);
  if (handle_out)
    *handle_out = handle;
  else
    imFileClose(handle);
  return image;
}

struct RegisterOnce
{
  RegisterOnce() { imFormatRegisterCAMRAW(); }
};

/* imFormatRegisterCAMRAW allocates and registers a driver on every call, so
   register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("CAMRAW: the driver is read-only")
{
  CHECK(imFormatCanWriteImage("CAMRAW", "", IM_RGB, IM_USHORT) != IM_ERR_NONE);

  int error = IM_ERR_NONE;
  imFile* handle = imFileNew(out_path("never.dng").c_str(), "CAMRAW", &error);
  CHECK(handle == NULL);
  if (handle)
    imFileClose(handle);
}

TEST_CASE("CAMRAW: a DNG decodes to 16 bit RGB with its bands the right colours")
{
  int error = IM_ERR_NONE;
  imFile* handle = NULL;
  imImage* image = load_bands(0, &handle, &error);
  REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);

  CHECK(image->width == 64);
  CHECK(image->height == 48);
  CHECK(image->data_type == IM_USHORT);
  CHECK(imColorModeSpace(image->color_space) == IM_RGB);

  /* The centre of each band, clear of the demosaic's edge effects. Each must
     be dominated by its own channel, by a wide margin. */
  const int centres[3] = { 10, 32, 53 };
  const char* names[3] = { "red", "green", "blue" };
  for (int band = 0; band < 3; band++)
  {
    CAPTURE(names[band]);
    int own = at(image, band, centres[band], 24);
    for (int c = 0; c < 3; c++)
    {
      if (c == band)
        continue;
      int other = at(image, c, centres[band], 24);
      CAPTURE(own);
      CAPTURE(other);
      CHECK(own > 4 * other);
    }
    CHECK(own > 20000);   /* bright: the band is near the white level */
  }

  /* The camera identification the file carries. */
  int type = 0, count = 0;
  const char* make = (const char*)imFileGetAttribute(handle, "Make", &type, &count);
  const char* model = (const char*)imFileGetAttribute(handle, "Model", &type, &count);
  REQUIRE(make != NULL);
  REQUIRE(model != NULL);
  CHECK(strlen(make) > 0);
  CHECK(std::string(model).find("Synthetic") != std::string::npos);

  imImageDestroy(image);
  imFileClose(handle);
}

TEST_CASE("CAMRAW: RawHalfSize halves the image and keeps the colours")
{
  int error = IM_ERR_NONE;
  imImage* image = load_bands(1, NULL, &error);
  REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);

  CHECK(image->width == 32);
  CHECK(image->height == 24);

  const int centres[3] = { 5, 16, 26 };
  for (int band = 0; band < 3; band++)
  {
    CAPTURE(band);
    int own = at(image, band, centres[band], 12);
    for (int c = 0; c < 3; c++)
      if (c != band)
        CHECK(own > 4 * at(image, c, centres[band], 12));
  }

  imImageDestroy(image);
}

TEST_CASE("CAMRAW: an ordinary image file is not claimed")
{
  /* LibRaw identifies by content and knows several hundred formats, so the
     risk is it accepting something that is not a camera file. A PNG offered
     to this driver alone must be declined. */
  std::string png = std::string(IM_TEST_FIXTURE_DIR) + "/qoi_rgba_source.png";

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(png.c_str(), "CAMRAW", &error);
  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);
  if (handle)
    imFileClose(handle);
}
