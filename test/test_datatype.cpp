/* Tests for the data-type accessors in src/im_datatype.cpp and for the
 * attribute validation in the RAW format reader (src/im_format_raw.cpp).
 *
 * These belong together: RAW has no header, so the application supplies the
 * geometry and data type, and an unvalidated value there was the live route
 * into the table accessors' out-of-bounds reads.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_raw.h>
#include <im_image.h>
#include <im_convert.h>
#include <im_complex.h>

#include <stdio.h>
#include <string.h>
#include <string>

/* ------------------------------------------------------------------ *
 * Data-type accessors
 * ------------------------------------------------------------------ */

TEST_CASE("imDataType accessors report the documented values")
{
  CHECK(imDataTypeSize(IM_BYTE)    == 1);
  CHECK(imDataTypeSize(IM_SHORT)   == 2);
  CHECK(imDataTypeSize(IM_USHORT)  == 2);
  CHECK(imDataTypeSize(IM_INT)     == 4);
  CHECK(imDataTypeSize(IM_FLOAT)   == 4);
  CHECK(imDataTypeSize(IM_DOUBLE)  == 8);
  CHECK(imDataTypeSize(IM_CFLOAT)  == 8);
  CHECK(imDataTypeSize(IM_CDOUBLE) == 16);

  CHECK(strcmp(imDataTypeName(IM_BYTE), "byte") == 0);
  CHECK(strcmp(imDataTypeName(IM_CDOUBLE), "cdouble") == 0);

  CHECK(imDataTypeIntMax(IM_BYTE) == 255);
  CHECK(imDataTypeIntMin(IM_SHORT) == -32768);
}

TEST_CASE("imDataType accessors reject out-of-range types instead of indexing")
{
  /* Regression: each accessor asserted and then indexed iTypeInfoTable
   * regardless. Every shipped build defines NDEBUG, so an out-of-range type
   * was an out-of-bounds read returning whatever sat next to the table --
   * which callers then used to size allocations or as a divisor. */
  const int bad_types[] = { IM_BYTE - 1, IM_CDOUBLE + 1, -1, -100000, 999999 };

  for (size_t i = 0; i < sizeof(bad_types)/sizeof(bad_types[0]); i++)
  {
    const int bad = bad_types[i];
    CAPTURE(bad);

    /* 0 means "no usable size" -- never a plausible-looking width that would
       under-allocate a buffer. */
    CHECK(imDataTypeSize(bad) == 0);
    CHECK(imDataTypeIntMax(bad) == 0);
    CHECK(imDataTypeIntMin(bad) == 0);

    /* Must never be NULL: callers pass this to strcat and lua_pushstring. */
    const char* name = imDataTypeName(bad);
    REQUIRE(name != NULL);
    CHECK(strlen(name) > 0);
  }
}

/* ------------------------------------------------------------------ *
 * RAW attribute validation
 * ------------------------------------------------------------------ */

namespace {

/* RAW has no header, so a file of arbitrary bytes is a valid subject; the
   attributes decide how it is interpreted. */
std::string make_raw_file(const char* name)
{
  std::string path = std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
  FILE* f = fopen(path.c_str(), "wb");
  if (f)
  {
    unsigned char bytes[4096];
    for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(i & 0xFF);
    fwrite(bytes, 1, sizeof(bytes), f);
    fclose(f);
  }
  return path;
}

void set_int(imFile* file, const char* name, int value)
{
  imFileSetAttribute(file, name, IM_INT, 1, &value);
}

} /* namespace */

TEST_CASE("RAW rejects missing geometry attributes instead of dereferencing NULL")
{
  /* Regression: the four required attributes were read as
   *   this->width = *(int*)attrib_table->Get("Width");
   * with "The following attributes MUST exist" as the only enforcement.
   * imAttribTableGet returns NULL for a name never set, so omitting any one
   * of them was an immediate NULL dereference. */
  std::string path = make_raw_file("params.raw");

  struct Case { const char* omitted; };
  const Case cases[] = { {"Width"}, {"Height"}, {"ColorMode"}, {"DataType"} };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    const char* omitted = cases[c].omitted;
    CAPTURE(omitted);

    int error = IM_ERR_NONE;
    imFile* file = imFileOpenRaw(path.c_str(), &error);
    REQUIRE(file != NULL);

    if (strcmp(omitted, "Width")     != 0) set_int(file, "Width", 16);
    if (strcmp(omitted, "Height")    != 0) set_int(file, "Height", 16);
    if (strcmp(omitted, "ColorMode") != 0) set_int(file, "ColorMode", IM_GRAY);
    if (strcmp(omitted, "DataType")  != 0) set_int(file, "DataType", IM_BYTE);

    int w = 0, h = 0, cm = 0, dt = 0;
    error = imFileReadImageInfo(file, 0, &w, &h, &cm, &dt);

    CHECK(error == IM_ERR_DATA);      /* an error, not a crash */

    imFileClose(file);
  }
}

TEST_CASE("RAW rejects an out-of-range DataType")
{
  /* This was the live route to the out-of-bounds table read: an unvalidated
   * DataType reached imDataTypeSize, whose result then sized a read and, if
   * it landed on zero, became the divisor in imBinFileBase::Read. */
  std::string path = make_raw_file("badtype.raw");

  const int bad_types[] = { IM_BYTE - 1, IM_CDOUBLE + 1, 12345, -999 };

  for (size_t i = 0; i < sizeof(bad_types)/sizeof(bad_types[0]); i++)
  {
    CAPTURE(bad_types[i]);

    int error = IM_ERR_NONE;
    imFile* file = imFileOpenRaw(path.c_str(), &error);
    REQUIRE(file != NULL);

    set_int(file, "Width", 16);
    set_int(file, "Height", 16);
    set_int(file, "ColorMode", IM_GRAY);
    set_int(file, "DataType", bad_types[i]);

    int w = 0, h = 0, cm = 0, dt = 0;
    error = imFileReadImageInfo(file, 0, &w, &h, &cm, &dt);

    CHECK(error == IM_ERR_DATA);

    imFileClose(file);
  }
}

TEST_CASE("RAW rejects an out-of-range ColorMode and nonsense dimensions")
{
  std::string path = make_raw_file("badmode.raw");

  struct Case { const char* label; int width; int height; int color_mode; };
  const Case cases[] = {
    { "color space above the enum", 16, 16, IM_XYZ + 1 },
    { "negative color space",       16, 16, -1          },
    { "zero width",                  0, 16, IM_GRAY     },
    { "negative width",             -8, 16, IM_GRAY     },
    { "zero height",                16,  0, IM_GRAY     },
    { "negative height",            16, -8, IM_GRAY     },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].label);

    int error = IM_ERR_NONE;
    imFile* file = imFileOpenRaw(path.c_str(), &error);
    REQUIRE(file != NULL);

    set_int(file, "Width", cases[c].width);
    set_int(file, "Height", cases[c].height);
    set_int(file, "ColorMode", cases[c].color_mode);
    set_int(file, "DataType", IM_BYTE);

    int w = 0, h = 0, cm = 0, dt = 0;
    error = imFileReadImageInfo(file, 0, &w, &h, &cm, &dt);

    CHECK(error == IM_ERR_DATA);

    imFileClose(file);
  }
}

TEST_CASE("RAW still reads a correctly described file")
{
  /* The validation must not have made the format unusable: a fully and
   * correctly specified RAW read is the case that has to keep working. */
  std::string path = make_raw_file("valid.raw");

  int error = IM_ERR_NONE;
  imFile* file = imFileOpenRaw(path.c_str(), &error);
  REQUIRE(file != NULL);

  set_int(file, "Width", 32);
  set_int(file, "Height", 16);
  set_int(file, "ColorMode", IM_GRAY);
  set_int(file, "DataType", IM_BYTE);

  int w = 0, h = 0, cm = 0, dt = 0;
  error = imFileReadImageInfo(file, 0, &w, &h, &cm, &dt);

  CHECK(error == IM_ERR_NONE);
  CHECK(w == 32);
  CHECK(h == 16);
  CHECK(imColorModeSpace(cm) == IM_GRAY);
  CHECK(dt == IM_BYTE);

  imFileClose(file);
}

/* ------------------------------------------------------------------ *
 * Complex-to-real conversion
 * ------------------------------------------------------------------ */

TEST_CASE("imConvertDataType: the complex-to-real conversions are unscaled")
{
  /* Demoting complex to real is documented as using the complex2real
     conversion and nothing else -- no min/max scan, no gamma -- so every
     value here is exact rather than a tolerance on a rescaled range.
     *
     * IM_CPX_PHASE is the reason this case exists: it is the only caller of
     * cpxphase outside imProcessSplitComplex, and cpxphase used to pass atan2
     * its arguments in the wrong order, so the angle came out measured from
     * the imaginary axis. Nothing here covered it. */
  const int W = 5;
  const float re[5] = { 3.0f, 1.0f, 0.0f, -1.0f,  0.0f };
  const float im[5] = { 4.0f, 0.0f, 1.0f,  0.0f, -1.0f };

  imImage* src = imImageCreate(W, 1, IM_GRAY, IM_CFLOAT);
  imImage* dst = imImageCreate(W, 1, IM_GRAY, IM_FLOAT);
  REQUIRE(src != NULL);
  REQUIRE(dst != NULL);

  imcfloat* data = (imcfloat*)src->data[0];
  for (int i = 0; i < W; i++)
  {
    data[i].real = re[i];
    data[i].imag = im[i];
  }

  const float* out = (const float*)dst->data[0];

  SUBCASE("IM_CPX_REAL and IM_CPX_IMAG take a component each")
  {
    REQUIRE(imConvertDataType(src, dst, IM_CPX_REAL, 0, 0, IM_CAST_DIRECT) == IM_ERR_NONE);
    for (int i = 0; i < W; i++)
    {
      CAPTURE(i);
      CHECK(out[i] == re[i]);
    }

    REQUIRE(imConvertDataType(src, dst, IM_CPX_IMAG, 0, 0, IM_CAST_DIRECT) == IM_ERR_NONE);
    for (int i = 0; i < W; i++)
    {
      CAPTURE(i);
      CHECK(out[i] == im[i]);
    }
  }

  SUBCASE("IM_CPX_MAG is the modulus")
  {
    REQUIRE(imConvertDataType(src, dst, IM_CPX_MAG, 0, 0, IM_CAST_DIRECT) == IM_ERR_NONE);
    CHECK(out[0] == doctest::Approx(5.0f));   /* the 3-4-5 triangle */
    CHECK(out[1] == doctest::Approx(1.0f));
    CHECK(out[2] == doctest::Approx(1.0f));
    CHECK(out[3] == doctest::Approx(1.0f));
    CHECK(out[4] == doctest::Approx(1.0f));
  }

  SUBCASE("IM_CPX_PHASE is the argument, in radians, from the real axis")
  {
    REQUIRE(imConvertDataType(src, dst, IM_CPX_PHASE, 0, 0, IM_CAST_DIRECT) == IM_ERR_NONE);

    /* The axes are what pin the argument order down: with atan2's arguments
       swapped, (1,0) and (0,1) trade places. */
    CHECK(out[1] == doctest::Approx(0.0f));                  /* (1,0)  -> 0    */
    CHECK(out[2] == doctest::Approx(1.5707963f));            /* (0,1)  -> pi/2 */
    CHECK(out[3] == doctest::Approx(3.1415927f));            /* (-1,0) -> pi   */
    CHECK(out[4] == doctest::Approx(-1.5707963f));           /* (0,-1) -> -pi/2 */
    CHECK(out[0] == doctest::Approx(0.9272952f));            /* (3,4)          */
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}
