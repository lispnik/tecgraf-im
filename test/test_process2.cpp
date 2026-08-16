/* More of libim_process: the operations whose results can be written out by
 * hand, which is most of what was still dark after test_process.cpp covered
 * the point and arithmetic ones.
 *
 *   src/process/im_geometric.cpp   mirror, flip and the two rotations
 *   src/process/im_logic.cpp       the bitwise operations
 *   src/process/im_threshold.cpp   manual and difference thresholds
 *   src/process/im_histogram.cpp   the three histogram entry points
 *   src/process/im_statistics.cpp  imStats, colour counting, RMS error
 *   src/process/im_tonegamut.cpp   negative
 *
 * Geometric transforms are pure permutations, so the assertions are exact
 * index arithmetic rather than tolerances -- a transposed axis or an
 * off-by-one at an edge fails them outright. The same goes for the bitwise
 * and threshold operations.
 *
 * Convolution, morphology, resize, FFT, Hough, Canny, quantization and the
 * analysis operations are still uncovered; they need golden images or a
 * reference implementation rather than a table of expected values, which is
 * a different and larger job.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_process.h>

#include <math.h>
#include <string.h>

namespace {

/* Deliberately not square, so a rotation that transposed its indices without
   swapping the dimensions cannot pass, and odd in both axes so a centre
   pixel exists. */
const int W = 5;
const int H = 3;
const int N = W * H;

imImage* create(int w, int h, int color_space, int data_type)
{
  imImage* image = imImageCreate(w, h, color_space, data_type);
  REQUIRE(image != NULL);
  return image;
}

imbyte* bytes(const imImage* image, int plane = 0)
{
  return (imbyte*)image->data[plane];
}

/* Each sample carries its own position, so an assertion can name where a
   value should have come from rather than just what it should be. */
void fill_index(imImage* image)
{
  imbyte* data = bytes(image);
  for (int i = 0; i < image->count; i++)
    data[i] = (imbyte)(i + 1);          /* 1-based: 0 stays a sentinel */
}

void set_bytes(imImage* image, const imbyte* values, int count)
{
  memcpy(image->data[0], values, count);
}

} /* namespace */


/* ================================================================== *
 * Geometric transforms -- exact permutations
 * ================================================================== */

TEST_CASE("geometry: mirror swaps columns and leaves rows alone")
{
  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  imImage* dst = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(src);

  CHECK(imProcessMirror(src, dst) != 0);

  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      CAPTURE(x); CAPTURE(y);
      CHECK((int)bytes(dst)[y*W + x] == (int)bytes(src)[y*W + (W-1-x)]);
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("geometry: flip swaps rows and leaves columns alone")
{
  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  imImage* dst = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(src);

  CHECK(imProcessFlip(src, dst) != 0);

  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      CAPTURE(x); CAPTURE(y);
      CHECK((int)bytes(dst)[y*W + x] == (int)bytes(src)[(H-1-y)*W + x]);
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("geometry: mirror and flip are their own inverses, in place")
{
  /* Both are documented as safe in place, which is the case a caller reaches
     for and the one most likely to alias its way into corruption. */
  imImage* image = create(W, H, IM_GRAY, IM_BYTE);
  imImage* original = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(image);
  fill_index(original);

  CHECK(imProcessMirror(image, image) != 0);
  CHECK(memcmp(image->data[0], original->data[0], N) != 0);   /* it did move */
  CHECK(imProcessMirror(image, image) != 0);
  CHECK(memcmp(image->data[0], original->data[0], N) == 0);

  CHECK(imProcessFlip(image, image) != 0);
  CHECK(imProcessFlip(image, image) != 0);
  CHECK(memcmp(image->data[0], original->data[0], N) == 0);

  imImageDestroy(image);
  imImageDestroy(original);
}

TEST_CASE("geometry: rotating 180 degrees reverses both axes")
{
  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  imImage* dst = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(src);

  CHECK(imProcessRotate180(src, dst) != 0);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (int)bytes(src)[N-1-i]);
  }

  /* And it is exactly mirror composed with flip, which is what the header
     says it is ("swap columns and swap lines"). */
  imImage* composed = create(W, H, IM_GRAY, IM_BYTE);
  imProcessMirror(src, composed);
  imProcessFlip(composed, composed);
  CHECK(memcmp(dst->data[0], composed->data[0], N) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(composed);
}

TEST_CASE("geometry: rotating 90 degrees swaps the dimensions")
{
  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(src);

  /* Target is H by W, per the header. Getting this backwards is the classic
     way to write past the end of a rotation buffer. */
  imImage* cw  = create(H, W, IM_GRAY, IM_BYTE);
  imImage* ccw = create(H, W, IM_GRAY, IM_BYTE);

  CHECK(imProcessRotate90(src, cw, 1) != 0);
  CHECK(imProcessRotate90(src, ccw, -1) != 0);

  CHECK(cw->width == H);
  CHECK(cw->height == W);

  /* Stated as index arithmetic rather than as a direction in screen terms:
     IM stores row 0 at the bottom, so "clockwise" on the buffer and
     clockwise as displayed are not the same turn, and naming one would be
     asserting a convention the header does not fix. What matters is that
     each direction is the permutation the driver claims and that the two
     are mutual inverses, which the subcases below check. */
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      CAPTURE(x); CAPTURE(y);
      CHECK((int)bytes(cw)[(W-1-x)*H + y] == (int)bytes(src)[y*W + x]);
      CHECK((int)bytes(ccw)[x*H + (H-1-y)] == (int)bytes(src)[y*W + x]);
    }
  }

  SUBCASE("and the two directions undo each other")
  {
    imImage* back = create(W, H, IM_GRAY, IM_BYTE);
    CHECK(imProcessRotate90(cw, back, -1) != 0);
    CHECK(memcmp(back->data[0], src->data[0], N) == 0);
    imImageDestroy(back);
  }

  SUBCASE("and two turns the same way make a half turn")
  {
    imImage* twice = create(W, H, IM_GRAY, IM_BYTE);
    imImage* half  = create(W, H, IM_GRAY, IM_BYTE);
    CHECK(imProcessRotate90(cw, twice, 1) != 0);
    CHECK(imProcessRotate180(src, half) != 0);
    CHECK(memcmp(twice->data[0], half->data[0], N) == 0);
    imImageDestroy(twice);
    imImageDestroy(half);
  }

  imImageDestroy(src);
  imImageDestroy(cw);
  imImageDestroy(ccw);
}

TEST_CASE("geometry: every plane of a colour image moves together")
{
  /* A transform that only handled the first plane would pass every case
     above, all of which are gray. */
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_BYTE);

  for (int p = 0; p < 3; p++)
    for (int i = 0; i < N; i++)
      bytes(src, p)[i] = (imbyte)(i + 1 + p * 40);

  CHECK(imProcessMirror(src, dst) != 0);

  for (int p = 0; p < 3; p++)
  {
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        CAPTURE(p); CAPTURE(x); CAPTURE(y);
        CHECK((int)bytes(dst, p)[y*W + x] == (int)bytes(src, p)[y*W + (W-1-x)]);
      }
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Bitwise operations
 * ================================================================== */

TEST_CASE("logic: the bitwise operations are exact")
{
  const int COUNT = 6;
  const imbyte a[6] = { 0x00, 0xFF, 0xF0, 0x0F, 0xAA, 0x55 };
  const imbyte b[6] = { 0xFF, 0xFF, 0xCC, 0x33, 0x0F, 0xF0 };

  imImage* src1 = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* src2 = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst  = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(src1, a, COUNT);
  set_bytes(src2, b, COUNT);

  SUBCASE("IM_BIT_AND")
  {
    imProcessBitwiseOp(src1, src2, dst, IM_BIT_AND);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (a[i] & b[i]));
    }
  }
  SUBCASE("IM_BIT_OR")
  {
    imProcessBitwiseOp(src1, src2, dst, IM_BIT_OR);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (a[i] | b[i]));
    }
  }
  SUBCASE("IM_BIT_XOR is a NOR, whatever the constant is called")
  {
    /* The header spells the formula out as "xor = ~(a | b)" and the code
       agrees with the header, so this is not a defect in the implementation
       -- but the constant is named for an operation it does not perform, and
       ~(a|b) is NOR. Anyone reading the enum rather than the comment gets the
       wrong function.

       Pinned as it behaves. Changing it would be an API break either way
       round: fixing the operation changes results for existing callers,
       renaming the constant breaks their source. Worth a decision, not a
       silent correction. */
    imProcessBitwiseOp(src1, src2, dst, IM_BIT_XOR);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (imbyte)~(a[i] | b[i]));
      /* And demonstrably not the operation it is named for. */
      if ((a[i] ^ b[i]) != (imbyte)~(a[i] | b[i]))
        CHECK((int)bytes(dst)[i] != (a[i] ^ b[i]));
    }
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("logic: not inverts every bit and is its own inverse")
{
  const int COUNT = 6;
  const imbyte a[6] = { 0x00, 0xFF, 0xF0, 0x0F, 0xAA, 0x55 };

  imImage* src = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(src, a, COUNT);

  imProcessBitwiseNot(src, dst);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (imbyte)~a[i]);
  }

  imProcessBitwiseNot(dst, dst);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (int)a[i]);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("logic: a bit mask is the same operation against a constant")
{
  const int COUNT = 6;
  const imbyte a[6] = { 0x00, 0xFF, 0xF0, 0x0F, 0xAA, 0x55 };
  const imbyte mask = 0x3C;

  imImage* src = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(src, a, COUNT);

  imProcessBitMask(src, dst, mask, IM_BIT_AND);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (a[i] & mask));
  }

  imProcessBitMask(src, dst, mask, IM_BIT_OR);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (a[i] | mask));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("logic: a bit plane is extracted or cleared")
{
  const int COUNT = 4;
  const imbyte a[4] = { 0x00, 0xFF, 0x08, 0xF7 };   /* bit 3 set, clear */

  imImage* src = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(src, a, COUNT);

  SUBCASE("extract reports the bit as 0 or 1")
  {
    /* Not the bit left in place, despite the "000X0000" illustration in the
       header -- the implementation is an explicit "? 1 : 0", which is what
       makes the result usable as a binary image. The comment is what is
       misleading here, not the code. */
    imProcessBitPlane(src, dst, 3, 0);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == ((a[i] & 0x08) ? 1 : 0));
    }
  }
  SUBCASE("reset clears only that bit")
  {
    imProcessBitPlane(src, dst, 3, 1);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (a[i] & ~0x08));
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Thresholds
 * ================================================================== */

TEST_CASE("threshold: a <= level becomes 0, everything else the given value")
{
  const int COUNT = 6;
  const imbyte a[6] = { 0, 50, 100, 101, 200, 255 };

  imImage* src = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst = create(COUNT, 1, IM_BINARY, IM_BYTE);
  set_bytes(src, a, COUNT);

  SUBCASE("with the usual value of 1")
  {
    const int expected[6] = { 0, 0, 0, 1, 1, 1 };
    imProcessThreshold(src, dst, 100, 1);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == expected[i]);
    }
  }
  SUBCASE("the boundary is inclusive below")
  {
    /* 100 <= 100, so the sample equal to the level is background. Off by one
       here inverts the edge of every mask a caller builds. */
    imProcessThreshold(src, dst, 100, 255);
    CHECK((int)bytes(dst)[2] == 0);
    CHECK((int)bytes(dst)[3] == 255);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("threshold: by difference compares the two sources")
{
  const int COUNT = 5;
  const imbyte a[5] = { 10, 20, 30, 40, 50 };
  const imbyte b[5] = { 20, 20, 20, 20, 20 };
  const int expected[5] = { 0, 0, 1, 1, 1 };     /* a1 <= a2 ? 0 : 1 */

  imImage* src1 = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* src2 = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst  = create(COUNT, 1, IM_BINARY, IM_BYTE);
  set_bytes(src1, a, COUNT);
  set_bytes(src2, b, COUNT);

  imProcessThresholdByDiff(src1, src2, dst);

  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == expected[i]);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Histograms and statistics
 * ================================================================== */

TEST_CASE("histogram: a byte histogram counts, and cumulates on request")
{
  const int COUNT = 8;
  const unsigned char data[8] = { 0, 0, 0, 5, 5, 200, 255, 255 };
  unsigned long histo[256];

  imCalcByteHistogram(data, COUNT, histo, 0);
  CHECK(histo[0] == 3);
  CHECK(histo[5] == 2);
  CHECK(histo[200] == 1);
  CHECK(histo[255] == 2);
  CHECK(histo[1] == 0);
  CHECK(histo[128] == 0);

  unsigned long total = 0;
  for (int i = 0; i < 256; i++)
    total += histo[i];
  CHECK(total == (unsigned long)COUNT);

  SUBCASE("cumulative is the running sum, ending at the pixel count")
  {
    imCalcByteHistogram(data, COUNT, histo, 1);
    CHECK(histo[0] == 3);
    CHECK(histo[4] == 3);
    CHECK(histo[5] == 5);
    CHECK(histo[199] == 5);
    CHECK(histo[200] == 6);
    CHECK(histo[254] == 6);
    CHECK(histo[255] == (unsigned long)COUNT);

    /* Monotonic by construction; a break means the accumulator was reset. */
    for (int i = 1; i < 256; i++)
    {
      CAPTURE(i);
      CHECK(histo[i] >= histo[i-1]);
    }
  }
}

TEST_CASE("histogram: the image entry points agree with the raw one")
{
  imImage* image = create(W, H, IM_GRAY, IM_BYTE);
  imbyte* data = bytes(image);
  for (int i = 0; i < N; i++)
    data[i] = (imbyte)((i * 17) % 7);

  unsigned long from_image[256], from_raw[256], from_plane[256];

  CHECK(imCalcGrayHistogram(image, from_image, 0) != 0);
  imCalcByteHistogram(data, N, from_raw, 0);
  CHECK(imCalcHistogram(image, from_plane, 0, 0) != 0);

  for (int i = 0; i < 256; i++)
  {
    CAPTURE(i);
    CHECK(from_image[i] == from_raw[i]);
    CHECK(from_plane[i] == from_raw[i]);
  }

  imImageDestroy(image);
}

TEST_CASE("statistics: the summary matches a hand count")
{
  /* Chosen so mean and standard deviation are exact in binary: the values
     are symmetric about 100 and the variance works out to 200. */
  const int COUNT = 5;
  const imbyte a[5] = { 80, 90, 100, 110, 120 };

  imImage* image = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(image, a, COUNT);

  imStats stats;
  CHECK(imCalcImageStatistics(image, &stats) != 0);

  CHECK(stats.max == doctest::Approx(120.0));
  CHECK(stats.min == doctest::Approx(80.0));
  CHECK(stats.mean == doctest::Approx(100.0));
  CHECK(stats.positive == 5);
  CHECK(stats.negative == 0);
  CHECK(stats.zeros == 0);

  /* Population standard deviation of {80,90,100,110,120} about 100 is
     sqrt(1000/5) = sqrt(200); the sample form would be sqrt(250). Both are
     defensible, so accept either rather than pinning a convention the header
     does not state. */
  CHECK((stats.stddev == doctest::Approx(sqrt(200.0)).epsilon(0.01) ||
         stats.stddev == doctest::Approx(sqrt(250.0)).epsilon(0.01)));

  imImageDestroy(image);
}

TEST_CASE("statistics: zeros are counted separately from positives")
{
  const int COUNT = 6;
  const imbyte a[6] = { 0, 0, 0, 1, 2, 3 };

  imImage* image = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(image, a, COUNT);

  imStats stats;
  CHECK(imCalcImageStatistics(image, &stats) != 0);

  CHECK(stats.zeros == 3);
  CHECK(stats.positive == 3);
  CHECK(stats.negative == 0);
  CHECK(stats.min == doctest::Approx(0.0));

  imImageDestroy(image);
}

TEST_CASE("statistics: counting colours ignores how often each appears")
{
  imImage* image = create(4, 1, IM_RGB, IM_BYTE);

  /* Three distinct colours, one of them repeated. */
  const imbyte r[4] = { 255,   0, 255,   7 };
  const imbyte g[4] = {   0, 255,   0,   8 };
  const imbyte b[4] = {   0,   0,   0,   9 };
  memcpy(image->data[0], r, 4);
  memcpy(image->data[1], g, 4);
  memcpy(image->data[2], b, 4);

  unsigned long count = 0;
  CHECK(imCalcCountColors(image, &count) != 0);
  CHECK(count == 3);

  imImageDestroy(image);
}

TEST_CASE("statistics: RMS error is zero only for identical images")
{
  imImage* a = create(W, H, IM_GRAY, IM_BYTE);
  imImage* b = create(W, H, IM_GRAY, IM_BYTE);
  fill_index(a);
  fill_index(b);

  double error = -1;
  CHECK(imCalcRMSError(a, b, &error) != 0);
  CHECK(error == doctest::Approx(0.0));

  /* One sample apart by 10 over N samples: sqrt(100/N). */
  bytes(b)[0] = (imbyte)(bytes(a)[0] + 10);
  CHECK(imCalcRMSError(a, b, &error) != 0);
  CHECK(error == doctest::Approx(sqrt(100.0 / N)).epsilon(0.01));

  imImageDestroy(a);
  imImageDestroy(b);
}


/* ================================================================== *
 * Tone gamut
 * ================================================================== */

TEST_CASE("tone: a negative is the complement, and its own inverse"
          * doctest::should_fail())
{
  /* imProcessNegative routes through imProcessToneGamut with
     IM_GAMUT_INVERT, which normalises to a double and back:

         invert_op(v) = 1.0 - (v - min)/range        then  * range + min

     For a byte image spanning 0..255 that should be 255 - v exactly, but the
     round trip does not land on an integer: (1.0 - 254.0/255.0) * 255.0 is
     0.9999999999999964, and the cast to imbyte truncates, so 254 inverts to
     0 rather than 1. The operation is therefore not an involution either --
     inverting twice does not return the original.

     Rounding rather than truncating on an integer target would fix it, the
     way imColorQuantize already does, but the same template carries every
     other gamut operation (normalize, pow, log, exp, solarize) for every
     data type including float, so the change is broader than it looks and
     wants a deliberate decision rather than a quiet edit.

     Stated here as the arithmetic a caller would expect. */
  const int COUNT = 6;
  const imbyte a[6] = { 0, 1, 100, 128, 254, 255 };

  imImage* src = create(COUNT, 1, IM_GRAY, IM_BYTE);
  imImage* dst = create(COUNT, 1, IM_GRAY, IM_BYTE);
  set_bytes(src, a, COUNT);

  imProcessNegative(src, dst);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == 255 - (int)a[i]);
  }

  imProcessNegative(dst, dst);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (int)a[i]);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}
