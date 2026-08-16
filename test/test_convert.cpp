/* Tests for the colour and data-type conversion layer:
 *
 *   include/im_color.h        the inline colour maths every driver leans on
 *   src/im_colorhsi.cpp       RGB <-> HSI
 *   src/im_convertcolor.cpp   imConvertColorSpace
 *   src/im_converttype.cpp    imConvertDataType
 *   src/im_convertopengl.cpp  imConvertPacking, imConvertMapToRGB
 *   src/im_rgb2map.cpp        imConvertRGB2Map
 *
 * Not covered here: imConvertToBitmap (src/im_convertbitmap.cpp), the OpenGL
 * buffer entry points either side of the two packing helpers, and the slow
 * median-cut path in im_rgb2map.cpp that only runs once a palette overflows.
 *
 * This layer sits underneath every format driver, so a rounding or clamping
 * mistake here corrupts every load path in the library at once -- and none of
 * it had a test.
 *
 * The expected values are taken from the specifications these functions cite
 * (ITU-R 601 for luma and Y'CbCr, sRGB/D65 for XYZ and L*a*b*) rather than
 * from the implementations, so a case failing means the code disagrees with
 * the standard rather than with itself. Where a value is only reproducible by
 * restating the formula, the case asserts a landmark instead: an identity, a
 * round trip, or a point whose answer the standard fixes exactly.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_color.h>
#include <im_colorhsi.h>
#include <im_convert.h>
#include <im_palette.h>

#include <math.h>
#include <string.h>

namespace {

const int W = 4;
const int H = 2;
const int N = W * H;

imImage* create(int color_space, int data_type)
{
  imImage* image = imImageCreate(W, H, color_space, data_type);
  REQUIRE(image != NULL);
  return image;
}

imbyte* bytes(const imImage* image, int plane = 0)
{
  return (imbyte*)image->data[plane];
}

/* The eight pixels used by most of the image-level cases: the primaries and
   secondaries, then black and white. Enough to exercise every branch of a
   colour conversion without needing a fixture. */
const imbyte RGB_R[N] = { 255,   0,   0, 255, 255,   0,   0, 255 };
const imbyte RGB_G[N] = {   0, 255,   0, 255,   0, 255,   0, 255 };
const imbyte RGB_B[N] = {   0,   0, 255,   0, 255, 255,   0, 255 };

void fill_rgb(imImage* image)
{
  memcpy(image->data[0], RGB_R, N);
  memcpy(image->data[1], RGB_G, N);
  memcpy(image->data[2], RGB_B, N);
}

} /* namespace */


/* ================================================================== *
 * The inline colour maths -- include/im_color.h
 * ================================================================== */

TEST_CASE("colour: luma uses the ITU-R 601 weights")
{
  /* Y' = 0.299 R' + 0.587 G' + 0.114 B'. The weights sum to exactly 1, which
     is what makes the two ends exact rather than approximate. */
  CHECK((int)imColorRGB2Luma<imbyte>(255, 255, 255) == 255);
  CHECK((int)imColorRGB2Luma<imbyte>(0, 0, 0) == 0);

  /* Each primary on its own is its own weight, times full scale. The
     implementation works in thousandths and truncates. */
  CHECK((int)imColorRGB2Luma<imbyte>(255, 0, 0) == (299 * 255) / 1000);   /*  76 */
  CHECK((int)imColorRGB2Luma<imbyte>(0, 255, 0) == (587 * 255) / 1000);   /* 149 */
  CHECK((int)imColorRGB2Luma<imbyte>(0, 0, 255) == (114 * 255) / 1000);   /*  29 */

  /* Green is the heaviest and blue the lightest -- the ordering is the whole
     point of a weighted luma over a plain average. */
  CHECK(imColorRGB2Luma<imbyte>(0, 255, 0) > imColorRGB2Luma<imbyte>(255, 0, 0));
  CHECK(imColorRGB2Luma<imbyte>(255, 0, 0) > imColorRGB2Luma<imbyte>(0, 0, 255));
}

TEST_CASE("colour: RGB and Y'CbCr agree with ITU-R 601 at the landmarks")
{
  const imbyte zero = 128;   /* imColorZeroShift for IM_BYTE */
  imbyte Y, Cb, Cr;

  SUBCASE("a neutral colour has no chroma")
  {
    /* The Cb and Cr rows each sum to zero, so any R=G=B lands exactly on the
       zero shift whatever the level. */
    imColorRGB2YCbCr<imbyte>(255, 255, 255, Y, Cb, Cr, zero);
    CHECK((int)Y == 255);
    CHECK((int)Cb == 128);
    CHECK((int)Cr == 128);

    imColorRGB2YCbCr<imbyte>(0, 0, 0, Y, Cb, Cr, zero);
    CHECK((int)Y == 0);
    CHECK((int)Cb == 128);
    CHECK((int)Cr == 128);

    imColorRGB2YCbCr<imbyte>(128, 128, 128, Y, Cb, Cr, zero);
    CHECK((int)Cb == 128);
    CHECK((int)Cr == 128);
  }

  SUBCASE("red is the Cr extreme and blue the Cb extreme")
  {
    imColorRGB2YCbCr<imbyte>(255, 0, 0, Y, Cb, Cr, zero);
    CHECK((int)Cr > 250);          /* +0.5 full scale, on top of the shift */
    CHECK((int)Cb < 90);

    imColorRGB2YCbCr<imbyte>(0, 0, 255, Y, Cb, Cr, zero);
    CHECK((int)Cb > 250);
    CHECK((int)Cr < 110);
  }

  SUBCASE("the inverse returns the original within the byte rounding floor")
  {
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      imbyte R, G, B;
      imColorRGB2YCbCr<imbyte>(RGB_R[i], RGB_G[i], RGB_B[i], Y, Cb, Cr, zero);
      imColorYCbCr2RGB<imbyte>(Y, Cb, Cr, R, G, B, zero, (imbyte)0, (imbyte)255);

      /* Two truncations to 8 bits plus the 3-decimal matrix; measured at 2. */
      CHECK(abs((int)R - (int)RGB_R[i]) <= 3);
      CHECK(abs((int)G - (int)RGB_G[i]) <= 3);
      CHECK(abs((int)B - (int)RGB_B[i]) <= 3);
    }
  }

  SUBCASE("the inverse clamps rather than wrapping")
  {
    /* Cb and Cr at their extremes describe colours outside the RGB cube, so
       R and B run past both ends and have to saturate rather than roll over.
       G stays inside on both, because its two coefficients have the same sign
       and partly cancel -- so it is the control: a clamp that fired on
       everything would show up here. */
    imbyte R, G, B;

    /* R would be 255 + 1.402*127 = 433, B would be 255 + 1.772*127 = 480. */
    imColorYCbCr2RGB<imbyte>(255, 255, 255, R, G, B, zero, (imbyte)0, (imbyte)255);
    CHECK((int)R == 255);
    CHECK((int)B == 255);
    CHECK((int)G == 120);          /* 255 - 0.344*127 - 0.714*127 */

    /* R would be 1.402*-128 = -179, B would be 1.772*-128 = -227. */
    imColorYCbCr2RGB<imbyte>(0, 0, 0, R, G, B, zero, (imbyte)0, (imbyte)255);
    CHECK((int)R == 0);
    CHECK((int)B == 0);
    CHECK((int)G == 135);          /* 0 + 0.344*128 + 0.714*128 */
  }
}

TEST_CASE("colour: CMYK to RGB follows the documented formula")
{
  /* R = (1-K)(1-C), and so on -- the header is explicit that this is a poor
     conversion kept for visualisation, so the test pins the formula rather
     than any colorimetric property. */
  imbyte R, G, B;
  const imbyte max = 255;

  imColorCMYK2RGB<imbyte>(0, 0, 0, 0, R, G, B, max);       /* no ink */
  CHECK((int)R == 255); CHECK((int)G == 255); CHECK((int)B == 255);

  imColorCMYK2RGB<imbyte>(0, 0, 0, 255, R, G, B, max);     /* full black */
  CHECK((int)R == 0); CHECK((int)G == 0); CHECK((int)B == 0);

  imColorCMYK2RGB<imbyte>(255, 0, 0, 0, R, G, B, max);     /* full cyan */
  CHECK((int)R == 0); CHECK((int)G == 255); CHECK((int)B == 255);

  imColorCMYK2RGB<imbyte>(0, 255, 0, 0, R, G, B, max);     /* full magenta */
  CHECK((int)R == 255); CHECK((int)G == 0); CHECK((int)B == 255);
}

TEST_CASE("colour: RGB to XYZ lands on the D65 white point")
{
  double X, Y, Z;

  /* The three rows of the sRGB matrix sum to the D65 white point, which is
     the same constant imColorXYZ2Lab divides by. If the matrix and the white
     point ever drift apart, white stops being neutral. */
  imColorRGB2XYZ<double>(1.0, 1.0, 1.0, X, Y, Z);
  CHECK(X == doctest::Approx(0.9505));
  CHECK(Y == doctest::Approx(1.0));
  CHECK(Z == doctest::Approx(1.0890));

  imColorRGB2XYZ<double>(0.0, 0.0, 0.0, X, Y, Z);
  CHECK(X == doctest::Approx(0.0));
  CHECK(Y == doctest::Approx(0.0));
  CHECK(Z == doctest::Approx(0.0));

  /* Y is luminance, so green dominates it -- the same ordering as luma, but
     with the linear-light weights rather than the 601 ones. */
  double Xr, Yr, Zr, Xg, Yg, Zg, Xb, Yb, Zb;
  imColorRGB2XYZ<double>(1.0, 0.0, 0.0, Xr, Yr, Zr);
  imColorRGB2XYZ<double>(0.0, 1.0, 0.0, Xg, Yg, Zg);
  imColorRGB2XYZ<double>(0.0, 0.0, 1.0, Xb, Yb, Zb);
  CHECK(Yg > Yr);
  CHECK(Yr > Yb);

  SUBCASE("and back again")
  {
    double R, G, B;
    imColorRGB2XYZ<double>(0.25, 0.5, 0.75, X, Y, Z);
    imColorXYZ2RGB<double>(X, Y, Z, R, G, B);
    CHECK(R == doctest::Approx(0.25).epsilon(0.001));
    CHECK(G == doctest::Approx(0.5).epsilon(0.001));
    CHECK(B == doctest::Approx(0.75).epsilon(0.001));
  }
}

TEST_CASE("colour: the D65 white point is L*=1 with no chroma")
{
  double L, a, b;

  /* X/Xn, Y/Yn and Z/Zn are all 1 at the white point, so all three f() come
     out 1 and the a and b differences vanish exactly. */
  imColorXYZ2Lab(0.9505, 1.0, 1.0890, L, a, b);
  CHECK(L == doctest::Approx(1.0));
  CHECK(a == doctest::Approx(0.0));
  CHECK(b == doctest::Approx(0.0));

  /* Black is the other fixed point: f(0) = 16/116, so L = 1.16*(0.16/1.16) -
     0.16 = 0, and a and b are again differences of equal terms. */
  imColorXYZ2Lab(0.0, 0.0, 0.0, L, a, b);
  CHECK(L == doctest::Approx(0.0));
  CHECK(a == doctest::Approx(0.0));
  CHECK(b == doctest::Approx(0.0));

  SUBCASE("and back again")
  {
    double X, Y, Z;
    imColorXYZ2Lab(0.3, 0.4, 0.5, L, a, b);
    imColorLab2XYZ(L, a, b, X, Y, Z);
    CHECK(X == doctest::Approx(0.3).epsilon(0.001));
    CHECK(Y == doctest::Approx(0.4).epsilon(0.001));
    CHECK(Z == doctest::Approx(0.5).epsilon(0.001));
  }

  SUBCASE("lightness is monotonic in luminance")
  {
    double L1, L2, a1, b1, a2, b2;
    imColorXYZ2Lab(0.1, 0.1, 0.1, L1, a1, b1);
    imColorXYZ2Lab(0.6, 0.6, 0.6, L2, a2, b2);
    CHECK(L2 > L1);
  }
}

TEST_CASE("colour: the sRGB transfer function round-trips and fixes its ends")
{
  /* 0 and 1 are fixed points of the sRGB curve by construction. */
  CHECK(imColorTransfer2Linear(0.0) == doctest::Approx(0.0));
  CHECK(imColorTransfer2Linear(1.0) == doctest::Approx(1.0));
  CHECK(imColorTransfer2Nonlinear(0.0) == doctest::Approx(0.0));
  CHECK(imColorTransfer2Nonlinear(1.0) == doctest::Approx(1.0));

  /* The curve lifts the midtones: encoding is above the linear value. */
  CHECK(imColorTransfer2Nonlinear(0.5) > 0.5);
  CHECK(imColorTransfer2Linear(0.5) < 0.5);

  for (int i = 0; i <= 10; i++)
  {
    double v = i / 10.0;
    CAPTURE(v);
    CHECK(imColorTransfer2Linear(imColorTransfer2Nonlinear(v)) ==
          doctest::Approx(v).epsilon(0.0001));
  }
}

TEST_CASE("colour: lightness and luminance are inverses")
{
  CHECK(imColorLuminance2Lightness(0.0) == doctest::Approx(0.0));
  CHECK(imColorLuminance2Lightness(1.0) == doctest::Approx(1.0));

  for (int i = 0; i <= 10; i++)
  {
    double v = i / 10.0;
    CAPTURE(v);
    CHECK(imColorLightness2Luminance(imColorLuminance2Lightness(v)) ==
          doctest::Approx(v).epsilon(0.0001));
  }
}

TEST_CASE("colour: quantize and reconstruct are inverses across the range")
{
  /* These two carry every promotion and demotion between data types, so an
     error here rescales entire images. */
  for (int i = 0; i <= 255; i += 17)
  {
    CAPTURE(i);
    double v = i / 255.0;
    imbyte q = imColorQuantize<imbyte>(v, (imbyte)0, (imbyte)255);
    CHECK((int)q == i);
    CHECK(imColorReconstruct<imbyte>(q, (imbyte)0, (imbyte)255) ==
          doctest::Approx(v).epsilon(0.005));
  }

  /* The ends must saturate exactly rather than landing one short. */
  CHECK((int)imColorQuantize<imbyte>(0.0, (imbyte)0, (imbyte)255) == 0);
  CHECK((int)imColorQuantize<imbyte>(1.0, (imbyte)0, (imbyte)255) == 255);
}


/* ================================================================== *
 * HSI -- src/im_colorhsi.cpp
 * ================================================================== */

TEST_CASE("HSI: the primaries sit at 0, 120 and 240 degrees")
{
  double h, s, i;

  imColorRGB2HSIbyte(255, 0, 0, &h, &s, &i);
  CHECK(h == doctest::Approx(0.0).epsilon(0.01));
  CHECK(s == doctest::Approx(1.0).epsilon(0.01));

  imColorRGB2HSIbyte(0, 255, 0, &h, &s, &i);
  CHECK(h == doctest::Approx(120.0).epsilon(0.01));

  imColorRGB2HSIbyte(0, 0, 255, &h, &s, &i);
  CHECK(h == doctest::Approx(240.0).epsilon(0.01));
}

TEST_CASE("HSI: a neutral colour has no saturation")
{
  double h, s, i;

  /* Hue is undefined on the achromatic axis; saturation is what has to be
     zero, and intensity has to track the level. */
  imColorRGB2HSIbyte(0, 0, 0, &h, &s, &i);
  CHECK(s == doctest::Approx(0.0).epsilon(0.01));
  CHECK(i == doctest::Approx(0.0).epsilon(0.01));

  imColorRGB2HSIbyte(255, 255, 255, &h, &s, &i);
  CHECK(s == doctest::Approx(0.0).epsilon(0.01));
  CHECK(i == doctest::Approx(1.0).epsilon(0.01));

  imColorRGB2HSIbyte(128, 128, 128, &h, &s, &i);
  CHECK(s == doctest::Approx(0.0).epsilon(0.01));
  CHECK(i > 0.4);
  CHECK(i < 0.6);
}

TEST_CASE("HSI: a round trip returns the original colour")
{
  for (int k = 0; k < N; k++)
  {
    CAPTURE(k);
    double h, s, i;
    imbyte r, g, b;

    imColorRGB2HSIbyte(RGB_R[k], RGB_G[k], RGB_B[k], &h, &s, &i);
    imColorHSI2RGBbyte(h, s, i, &r, &g, &b);

    CHECK(abs((int)r - (int)RGB_R[k]) <= 2);
    CHECK(abs((int)g - (int)RGB_G[k]) <= 2);
    CHECK(abs((int)b - (int)RGB_B[k]) <= 2);
  }
}


/* ================================================================== *
 * Image-level colour space conversion -- src/im_convertcolor.cpp
 * ================================================================== */

TEST_CASE("convert: RGB to gray is the luma, plane by plane")
{
  imImage* src = create(IM_RGB, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);
  fill_rgb(src);

  REQUIRE(imConvertColorSpace(src, dst) == IM_ERR_NONE);

  const imbyte* gray = bytes(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(abs((int)gray[i] -
              (int)imColorRGB2Luma<imbyte>(RGB_R[i], RGB_G[i], RGB_B[i])) <= 1);
  }

  /* The two ends have to be exact, whatever the rounding does in between. */
  CHECK((int)gray[6] == 0);      /* black */
  CHECK((int)gray[7] == 255);    /* white */

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: gray to RGB replicates into all three planes")
{
  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_RGB, IM_BYTE);

  imbyte* g = bytes(src);
  for (int i = 0; i < N; i++)
    g[i] = (imbyte)(i * 30);

  REQUIRE(imConvertColorSpace(src, dst) == IM_ERR_NONE);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst, 0)[i] == i * 30);
    CHECK((int)bytes(dst, 1)[i] == i * 30);
    CHECK((int)bytes(dst, 2)[i] == i * 30);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: RGB survives a trip through Y'CbCr")
{
  imImage* src  = create(IM_RGB, IM_BYTE);
  imImage* ycc  = create(IM_YCBCR, IM_BYTE);
  imImage* back = create(IM_RGB, IM_BYTE);
  fill_rgb(src);

  REQUIRE(imConvertColorSpace(src, ycc) == IM_ERR_NONE);
  REQUIRE(imConvertColorSpace(ycc, back) == IM_ERR_NONE);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(abs((int)bytes(back, 0)[i] - (int)RGB_R[i]) <= 3);
    CHECK(abs((int)bytes(back, 1)[i] - (int)RGB_G[i]) <= 3);
    CHECK(abs((int)bytes(back, 2)[i] - (int)RGB_B[i]) <= 3);
  }

  imImageDestroy(src); imImageDestroy(ycc); imImageDestroy(back);
}

TEST_CASE("convert: a MAP image expands through its palette")
{
  imImage* src = create(IM_MAP, IM_BYTE);
  imImage* dst = create(IM_RGB, IM_BYTE);

  /* Three entries, deliberately not gray, so a conversion that ignored the
     palette and treated the indices as intensities could not pass. */
  src->palette[0] = imColorEncode(10, 20, 30);
  src->palette[1] = imColorEncode(200, 100, 50);
  src->palette[2] = imColorEncode(0, 255, 128);
  src->palette_count = 3;

  imbyte* idx = bytes(src);
  for (int i = 0; i < N; i++)
    idx[i] = (imbyte)(i % 3);

  REQUIRE(imConvertColorSpace(src, dst) == IM_ERR_NONE);

  const imbyte expect_r[3] = { 10, 200, 0 };
  const imbyte expect_g[3] = { 20, 100, 255 };
  const imbyte expect_b[3] = { 30, 50, 128 };

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst, 0)[i] == (int)expect_r[i % 3]);
    CHECK((int)bytes(dst, 1)[i] == (int)expect_g[i % 3]);
    CHECK((int)bytes(dst, 2)[i] == (int)expect_b[i % 3]);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: binary becomes the two ends of the gray range")
{
  imImage* src = create(IM_BINARY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);

  imbyte* bits = bytes(src);
  for (int i = 0; i < N; i++)
    bits[i] = (imbyte)(i % 2);

  REQUIRE(imConvertColorSpace(src, dst) == IM_ERR_NONE);

  /* 1 has to become full white, not stay as the literal 1 -- that is the
     whole difference between a binary image and a gray one. */
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (i % 2 ? 255 : 0));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: an alpha channel is carried across a colour space change")
{
  imImage* src = create(IM_RGB | IM_ALPHA, IM_BYTE);
  imImage* dst = create(IM_GRAY | IM_ALPHA, IM_BYTE);
  REQUIRE(src->has_alpha != 0);
  REQUIRE(dst->has_alpha != 0);
  fill_rgb(src);

  imbyte* alpha = bytes(src, src->depth);
  for (int i = 0; i < N; i++)
    alpha[i] = (imbyte)(i * 17);

  REQUIRE(imConvertColorSpace(src, dst) == IM_ERR_NONE);

  const imbyte* out = bytes(dst, dst->depth);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)out[i] == i * 17);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Data type conversion -- src/im_converttype.cpp
 * ================================================================== */

TEST_CASE("convert: promoting to a wider type is exact")
{
  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_INT);

  imbyte* s = bytes(src);
  for (int i = 0; i < N; i++)
    s[i] = (imbyte)(i * 30);

  REQUIRE(imConvertDataType(src, dst, IM_CPX_REAL, 0, 0, IM_CAST_MINMAX) == IM_ERR_NONE);

  const int* d = (const int*)dst->data[0];
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(d[i] == i * 30);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: demoting scales by the range the cast mode selects")
{
  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_BYTE);

  /* 0..2000, deliberately far outside the byte range. */
  int* s = (int*)src->data[0];
  for (int i = 0; i < N; i++)
    s[i] = i * 285;                       /* 0 .. 1995 */

  SUBCASE("IM_CAST_MINMAX stretches the observed range onto 0..255")
  {
    REQUIRE(imConvertDataType(src, dst, IM_CPX_REAL, 0, 0, IM_CAST_MINMAX) == IM_ERR_NONE);

    /* Whatever the rounding, the extremes map to the extremes and the order
       is preserved -- that is what "scan for min and max" means. */
    CHECK((int)bytes(dst)[0] == 0);
    CHECK((int)bytes(dst)[N-1] == 255);
    for (int i = 1; i < N; i++)
    {
      CAPTURE(i);
      CHECK(bytes(dst)[i] >= bytes(dst)[i-1]);
    }
  }

  SUBCASE("IM_CAST_DIRECT casts the value and crops")
  {
    REQUIRE(imConvertDataType(src, dst, IM_CPX_REAL, 0, 0, IM_CAST_DIRECT) == IM_ERR_NONE);

    /* No scaling at all: each value is taken as-is and clamped to the byte
       range, so everything from 285 up saturates. */
    CHECK((int)bytes(dst)[0] == 0);
    CHECK((int)bytes(dst)[1] == 255);
    CHECK((int)bytes(dst)[N-1] == 255);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("convert: the same data type is a no-op, not an error"
          * doctest::should_fail())
{
  /* im_convert.h says of imConvertDataType: "Images must be of the same size
     and color mode. If data type is the same nothing is done." The
     implementation instead returns IM_ERR_DATA the moment the two types
     match, which a caller following the documentation reads as a failure --
     and IM_ERR_DATA is the same code it returns for a genuine mismatch of
     colour space, so the two are indistinguishable.

     One of the two is wrong and it is not obvious which: erroring on a
     conversion that is not one is defensible, but it is not what the header
     promises. Stated here as the documented behaviour. Fix it in whichever
     direction, then delete the should_fail decorator -- and if the code is
     what stays, correct the header. */
  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);

  imbyte* s = bytes(src);
  for (int i = 0; i < N; i++)
    s[i] = (imbyte)(i * 30);

  CHECK(imConvertDataType(src, dst, IM_CPX_REAL, 0, 0, IM_CAST_MINMAX) == IM_ERR_NONE);

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Raw buffer utilities -- src/im_convertbitmap.cpp
 * ================================================================== */

TEST_CASE("convert: packing moves between planar and interleaved")
{
  /* Planar is how imImage stores data; packed is what OpenGL and most codecs
     want. The two directions have to be exact inverses or every driver that
     goes through IM_PACKED loses its channel order. */
  const int depth = 3;
  imbyte planar[N * 3];
  imbyte packed[N * 3];
  imbyte back[N * 3];

  for (int p = 0; p < depth; p++)
    for (int i = 0; i < N; i++)
      planar[p*N + i] = (imbyte)(p*100 + i);

  imConvertPacking(planar, packed, W, H, depth, depth, IM_BYTE, 0);

  /* Interleaved: pixel i holds its three planes consecutively. */
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    for (int p = 0; p < depth; p++)
      CHECK((int)packed[i*depth + p] == p*100 + i);
  }

  imConvertPacking(packed, back, W, H, depth, depth, IM_BYTE, 1);
  CHECK(memcmp(planar, back, sizeof(planar)) == 0);
}

TEST_CASE("convert: a packed MAP buffer expands in place through its palette")
{
  long palette[4];
  palette[0] = imColorEncode(1, 2, 3);
  palette[1] = imColorEncode(40, 50, 60);
  palette[2] = imColorEncode(255, 0, 128);
  palette[3] = imColorEncode(7, 7, 7);

  /* The buffer has to be big enough for the RGB result, per the header. */
  imbyte data[N * 3];
  memset(data, 0, sizeof(data));
  for (int i = 0; i < N; i++)
    data[i] = (imbyte)(i % 4);

  imConvertMapToRGB(data, N, 3, 1, palette, 4);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    imbyte r, g, b;
    imColorDecode(&r, &g, &b, palette[i % 4]);
    CHECK((int)data[i*3 + 0] == (int)r);
    CHECK((int)data[i*3 + 1] == (int)g);
    CHECK((int)data[i*3 + 2] == (int)b);
  }
}


/* ================================================================== *
 * Median cut -- src/im_rgb2map.cpp
 * ================================================================== */

TEST_CASE("convert: an image with few colours quantizes without loss")
{
  /* Median cut only has to approximate when there are more colours than
     palette slots. With four distinct colours and 256 slots available, an
     exact result is the only acceptable one -- which makes this a real
     assertion rather than a tolerance. */
  const int count = 16;
  imbyte red[16], green[16], blue[16], map[16];
  long palette[256];
  int palette_count = 256;

  const imbyte cr[4] = { 255,   0,   0,  17 };
  const imbyte cg[4] = {   0, 255,   0,  99 };
  const imbyte cb[4] = {   0,   0, 255, 200 };

  for (int i = 0; i < count; i++)
  {
    red[i]   = cr[i % 4];
    green[i] = cg[i % 4];
    blue[i]  = cb[i % 4];
  }

  /* IM_ERR_NONE, not 1 -- this returns an IM error code like everything else,
     despite being an internal helper the header keeps out of the docs. */
  REQUIRE(imConvertRGB2Map(count, 1, red, green, blue, map, palette,
                           &palette_count) == IM_ERR_NONE);
  CHECK(palette_count >= 4);
  CHECK(palette_count <= 256);

  for (int i = 0; i < count; i++)
  {
    CAPTURE(i);
    REQUIRE(map[i] < palette_count);

    imbyte r, g, b;
    imColorDecode(&r, &g, &b, palette[map[i]]);
    CHECK((int)r == (int)cr[i % 4]);
    CHECK((int)g == (int)cg[i % 4]);
    CHECK((int)b == (int)cb[i % 4]);
  }

  /* Equal colours must share a slot, or the palette is being wasted. */
  for (int i = 4; i < count; i++)
  {
    CAPTURE(i);
    CHECK(map[i] == map[i % 4]);
  }
}
