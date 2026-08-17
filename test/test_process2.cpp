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
 *   src/process/im_kernel.cpp      the generated kernels and their orientation
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

#include <im_kernel.h>

#include <math.h>
#include <string.h>
#include <string>

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
  SUBCASE("IM_BIT_XOR is an exclusive or")
  {
    /* It used to compute ~(a | b) -- a NOR -- and the header documented that
       formula beside a name saying otherwise, so a caller reading the name
       got the wrong operation and there was no way to reach a real exclusive
       or at all. The old behaviour now lives under IM_BIT_NOR. */
    imProcessBitwiseOp(src1, src2, dst, IM_BIT_XOR);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (a[i] ^ b[i]));
    }

    /* The two are genuinely different operations, not two spellings of one. */
    CHECK((imbyte)(a[2] ^ b[2]) != (imbyte)~(a[2] | b[2]));
  }
  SUBCASE("IM_BIT_NOR carries the operation the old name performed")
  {
    imProcessBitwiseOp(src1, src2, dst, IM_BIT_NOR);
    for (int i = 0; i < COUNT; i++)
    {
      CAPTURE(i);
      CHECK((int)bytes(dst)[i] == (imbyte)~(a[i] | b[i]));
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

  imProcessBitMask(src, dst, mask, IM_BIT_XOR);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (a[i] ^ mask));
  }

  imProcessBitMask(src, dst, mask, IM_BIT_NOR);
  for (int i = 0; i < COUNT; i++)
  {
    CAPTURE(i);
    CHECK((int)bytes(dst)[i] == (imbyte)~(a[i] | mask));
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

TEST_CASE("tone: a negative is the complement, and its own inverse")
{
  /* imProcessNegative goes through imProcessToneGamut with IM_GAMUT_INVERT,
     which normalises to a double and scales back. The cast to an integer
     target used to truncate: (1.0 - 254.0/255.0) * 255.0 is
     0.9999999999999964, so 254 inverted to 0 rather than 1 and inverting
     twice did not return the original. It rounds now, for every gamut
     operation rather than just this one -- truncation was biasing all of them
     downward by up to a unit. */
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


/* ================================================================== *
 * Unchecked preconditions
 *
 * Several operations write the destination through a pointer of the source's
 * type without ever consulting dst_image->data_type, so a destination
 * narrower than the source ran off the end of its buffer -- four or eight
 * bytes per sample into a one byte slot. Each header stated the rule and none
 * of them enforced it, which made a documented misuse a heap overflow rather
 * than an error.
 *
 * Every case below would abort under AddressSanitizer before the guards went
 * in. They now assert in a debug build and return without writing in a
 * shipped one, which is the pattern the rest of the library uses for a
 * precondition a void function cannot report -- and what imProcessCompose has
 * always done when handed an image with no alpha channel.
 *
 * The destination is filled with a sentinel first, so "did nothing" is
 * distinguishable from "wrote zeroes".
 *
 * All three cases are NDEBUG only, per the convention in CLAUDE.md: they
 * violate a documented precondition on purpose to prove the release guard,
 * so they trip the assert that sits in front of it, and doctest cannot catch
 * an abort. The "asserts" job in ci-linux.yml is what covers the other half.
 * ================================================================== */
#ifdef NDEBUG

namespace {

const int GW = 8;
const int GH = 4;
const int GN = GW * GH;

bool all_sentinel(const imImage* image, imbyte sentinel)
{
  for (int i = 0; i < image->count * image->depth; i++)
    if (((imbyte*)image->data[0])[i] != sentinel)
      return false;
  return true;
}

imImage* sentinel_byte_image(int color_space, imbyte sentinel)
{
  imImage* image = imImageCreate(GW, GH, color_space, IM_BYTE);
  REQUIRE(image != NULL);
  memset(image->data[0], sentinel, (size_t)image->count * image->depth);
  return image;
}

} /* namespace */

TEST_CASE("guards: a destination narrower than the source is refused")
{
  imImage* wide = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(wide != NULL);
  for (int i = 0; i < GN; i++)
    ((int*)wide->data[0])[i] = i * 1000;

  SUBCASE("imProcessArithmeticOp with an int source and a byte target")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xAB);
    imProcessArithmeticOp(wide, wide, narrow, IM_BIN_ADD);
    CHECK(all_sentinel(narrow, 0xAB));
    imImageDestroy(narrow);
  }

  SUBCASE("and with a short target, which is still too narrow")
  {
    imImage* narrow = imImageCreate(GW, GH, IM_GRAY, IM_SHORT);
    REQUIRE(narrow != NULL);
    short* data = (short*)narrow->data[0];
    for (int i = 0; i < GN; i++) data[i] = 999;

    imProcessArithmeticOp(wide, wide, narrow, IM_BIN_ADD);
    for (int i = 0; i < GN; i++)
    {
      CAPTURE(i);
      CHECK(data[i] == 999);
    }
    imImageDestroy(narrow);
  }

  SUBCASE("but a wider or equal target still works")
  {
    /* The guard has to let through everything the dispatch handles, or it
       would be a regression dressed as a fix. */
    imImage* same = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(same != NULL);
    imProcessArithmeticOp(wide, wide, same, IM_BIN_ADD);
    for (int i = 0; i < GN; i++)
    {
      CAPTURE(i);
      CHECK(((int*)same->data[0])[i] == i * 2000);
    }
    imImageDestroy(same);

    imImage* wider = imImageCreate(GW, GH, IM_GRAY, IM_DOUBLE);
    REQUIRE(wider != NULL);
    imProcessArithmeticOp(wide, wide, wider, IM_BIN_ADD);
    CHECK(((double*)wider->data[0])[3] == doctest::Approx(6000.0));
    imImageDestroy(wider);
  }

  SUBCASE("and a double source into a float target, which is named and handled")
  {
    /* The reason the guard mirrors the dispatch instead of comparing widths:
       a width comparison would reject this, and it works. */
    imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_DOUBLE);
    imImage* dst = imImageCreate(GW, GH, IM_GRAY, IM_FLOAT);
    REQUIRE(src != NULL);
    REQUIRE(dst != NULL);
    for (int i = 0; i < GN; i++)
      ((double*)src->data[0])[i] = i * 1.5;

    imProcessArithmeticOp(src, src, dst, IM_BIN_ADD);
    for (int i = 0; i < GN; i++)
    {
      CAPTURE(i);
      CHECK(((float*)dst->data[0])[i] == doctest::Approx(i * 3.0));
    }
    imImageDestroy(src);
    imImageDestroy(dst);
  }

  imImageDestroy(wide);
}

TEST_CASE("guards: the same-type operations refuse a mismatched destination")
{
  imImage* wide1 = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  imImage* wide2 = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(wide1 != NULL);
  REQUIRE(wide2 != NULL);
  for (int i = 0; i < GN; i++)
  {
    ((int*)wide1->data[0])[i] = i * 1000;
    ((int*)wide2->data[0])[i] = i * 500;
  }

  SUBCASE("imProcessBlendConst")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xCD);
    imProcessBlendConst(wide1, wide2, narrow, 0.5);
    CHECK(all_sentinel(narrow, 0xCD));
    imImageDestroy(narrow);
  }

  SUBCASE("imProcessBackSub")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xCD);
    imProcessBackSub(wide1, wide2, narrow, 2.0, 0);
    CHECK(all_sentinel(narrow, 0xCD));
    imImageDestroy(narrow);
  }

  SUBCASE("imProcessMultiplyConj, which assumes complex throughout")
  {
    /* This one hardcodes imcfloat for all three images, so a byte target
       would receive eight bytes per sample. */
    imImage* cpx = imImageCreate(GW, GH, IM_GRAY, IM_CFLOAT);
    REQUIRE(cpx != NULL);
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xCD);
    imProcessMultiplyConj(cpx, cpx, narrow);
    CHECK(all_sentinel(narrow, 0xCD));
    imImageDestroy(cpx);
    imImageDestroy(narrow);
  }

  SUBCASE("imProcessCompose")
  {
    imImage* a = imImageCreate(GW, GH, IM_RGB | IM_ALPHA, IM_INT);
    imImage* b = imImageCreate(GW, GH, IM_RGB | IM_ALPHA, IM_INT);
    REQUIRE(a != NULL);
    REQUIRE(b != NULL);
    imImage* narrow = sentinel_byte_image(IM_RGB | IM_ALPHA, 0xCD);

    imProcessCompose(a, b, narrow);
    CHECK(all_sentinel(narrow, 0xCD));

    imImageDestroy(a); imImageDestroy(b); imImageDestroy(narrow);
  }

  SUBCASE("but matching types still work")
  {
    imImage* same = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(same != NULL);
    imProcessBlendConst(wide1, wide2, same, 1.0);
    for (int i = 0; i < GN; i++)
    {
      CAPTURE(i);
      CHECK(((int*)same->data[0])[i] == i * 1000);   /* alpha 1 is source one */
    }
    imImageDestroy(same);
  }

  imImageDestroy(wide1);
  imImageDestroy(wide2);
}

TEST_CASE("guards: normalizing components requires a real destination")
{
  imImage* src = imImageCreate(GW, GH, IM_RGB, IM_BYTE);
  REQUIRE(src != NULL);
  for (int p = 0; p < 3; p++)
    for (int i = 0; i < GN; i++)
      ((imbyte**)src->data)[p][i] = (imbyte)(i + p * 10 + 1);

  SUBCASE("a byte destination would have taken eight bytes a sample")
  {
    imImage* narrow = sentinel_byte_image(IM_RGB, 0xEF);
    imProcessNormalizeComponents(src, narrow);
    CHECK(all_sentinel(narrow, 0xEF));
    imImageDestroy(narrow);
  }

  SUBCASE("float and double destinations both work")
  {
    imImage* as_float = imImageCreate(GW, GH, IM_RGB, IM_FLOAT);
    REQUIRE(as_float != NULL);
    imProcessNormalizeComponents(src, as_float);

    /* Each pixel's components sum to one by construction. */
    for (int i = 0; i < GN; i++)
    {
      CAPTURE(i);
      double sum = 0;
      for (int p = 0; p < 3; p++)
        sum += ((float**)as_float->data)[p][i];
      CHECK(sum == doctest::Approx(1.0));
    }
    imImageDestroy(as_float);

    imImage* as_double = imImageCreate(GW, GH, IM_RGB, IM_DOUBLE);
    REQUIRE(as_double != NULL);
    imProcessNormalizeComponents(src, as_double);
    double sum = 0;
    for (int p = 0; p < 3; p++)
      sum += ((double**)as_double->data)[p][0];
    CHECK(sum == doctest::Approx(1.0));
    imImageDestroy(as_double);
  }

  imImageDestroy(src);
}
TEST_CASE("guards: the geometric operations refuse a mismatched destination")
{
  /* Every one of these loops over the SOURCE's dimensions and writes the
     destination through a pointer of the SOURCE's type, so a smaller or
     narrower target overruns its buffer. Each header says "images must be of
     the same type and size". */
  imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(src != NULL);
  for (int i = 0; i < GN; i++)
    ((int*)src->data[0])[i] = i + 1;

  SUBCASE("a narrower type")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xA5);
    CHECK(imProcessMirror(src, narrow) == 0);
    CHECK(all_sentinel(narrow, 0xA5));
    CHECK(imProcessFlip(src, narrow) == 0);
    CHECK(all_sentinel(narrow, 0xA5));
    CHECK(imProcessRotate180(src, narrow) == 0);
    CHECK(all_sentinel(narrow, 0xA5));
    imImageDestroy(narrow);
  }

  SUBCASE("a smaller destination of the right type")
  {
    imImage* small = imImageCreate(GW / 2, GH / 2, IM_GRAY, IM_INT);
    REQUIRE(small != NULL);
    int* data = (int*)small->data[0];
    for (int i = 0; i < small->count; i++) data[i] = -1;

    CHECK(imProcessMirror(src, small) == 0);
    for (int i = 0; i < small->count; i++)
    {
      CAPTURE(i);
      CHECK(data[i] == -1);
    }
    imImageDestroy(small);
  }

  SUBCASE("fewer planes than the source")
  {
    /* A three plane source into a one plane target indexes past the end of
       the destination's array of plane pointers, which is a different overrun
       from the ones above and needs the depth comparison to catch it. */
    imImage* rgb = imImageCreate(GW, GH, IM_RGB, IM_INT);
    REQUIRE(rgb != NULL);
    imImage* gray = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(gray != NULL);
    ((int*)gray->data[0])[0] = -1;

    CHECK(imProcessMirror(rgb, gray) == 0);
    CHECK(((int*)gray->data[0])[0] == -1);

    imImageDestroy(rgb);
    imImageDestroy(gray);
  }

  SUBCASE("Rotate90 wants the dimensions swapped, not equal")
  {
    imImage* equal = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(equal != NULL);
    ((int*)equal->data[0])[0] = -1;
    CHECK(imProcessRotate90(src, equal, 1) == 0);
    CHECK(((int*)equal->data[0])[0] == -1);
    imImageDestroy(equal);

    /* And the transposed one still works, or the guard would be a
       regression rather than a fix. */
    imImage* swapped = imImageCreate(GH, GW, IM_GRAY, IM_INT);
    REQUIRE(swapped != NULL);
    CHECK(imProcessRotate90(src, swapped, 1) != 0);
    imImageDestroy(swapped);
  }

  SUBCASE("but a matching destination still works")
  {
    imImage* same = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(same != NULL);
    CHECK(imProcessMirror(src, same) != 0);
    for (int y = 0; y < GH; y++)
      for (int x = 0; x < GW; x++)
      {
        CAPTURE(x); CAPTURE(y);
        CHECK(((int*)same->data[0])[y*GW + x] ==
              ((int*)src->data[0])[y*GW + (GW-1-x)]);
      }
    imImageDestroy(same);
  }

  imImageDestroy(src);
}

TEST_CASE("guards: the rank convolutions and binary morphology check too")
{
  imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(src != NULL);
  for (int i = 0; i < GN; i++)
    ((int*)src->data[0])[i] = i + 1;

  SUBCASE("a narrower destination is refused")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0xB4);
    CHECK(imProcessMedianConvolve(src, narrow, 3) == 0);
    CHECK(all_sentinel(narrow, 0xB4));
    CHECK(imProcessRankMaxConvolve(src, narrow, 3) == 0);
    CHECK(all_sentinel(narrow, 0xB4));
    imImageDestroy(narrow);
  }

  SUBCASE("binary morphology, through all five of its entry points")
  {
    imImage* binary = imImageCreate(GW, GH, IM_BINARY, IM_BYTE);
    REQUIRE(binary != NULL);
    for (int i = 0; i < GN; i++)
      ((imbyte*)binary->data[0])[i] = (imbyte)((i / 3) % 2);

    imImage* small = imImageCreate(GW / 2, GH / 2, IM_BINARY, IM_BYTE);
    REQUIRE(small != NULL);
    memset(small->data[0], 0x7E, (size_t)small->count);

    CHECK(imProcessBinMorphErode(binary, small, 3, 1) == 0);
    CHECK(imProcessBinMorphDilate(binary, small, 3, 1) == 0);
    CHECK(imProcessBinMorphOpen(binary, small, 3, 1) == 0);
    CHECK(imProcessBinMorphClose(binary, small, 3, 1) == 0);
    CHECK(imProcessBinMorphOutline(binary, small, 3, 1) == 0);
    CHECK(all_sentinel(small, 0x7E));

    imImageDestroy(binary);
    imImageDestroy(small);
  }

  SUBCASE("but the byte destination the threshold operations want is fine")
  {
    /* LocalMaxThreshold writes bytes whatever the source is, so its target
       type deliberately differs and the guard must not reject it. */
    imImage* binary = imImageCreate(GW, GH, IM_BINARY, IM_BYTE);
    REQUIRE(binary != NULL);
    CHECK(imProcessLocalMaxThreshold(src, binary, 3, 1) != 0);
    imImageDestroy(binary);
  }

  imImageDestroy(src);
}
TEST_CASE("guards: the convolutions and gray morphology check their destination")
{
  imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(src != NULL);
  for (int i = 0; i < GN; i++)
    ((int*)src->data[0])[i] = i + 1;

  SUBCASE("a narrower destination is refused")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0x5A);

    /* Convolve, ConvolveSep, ConvolveDual, ConvolveRep, CompassConvolve,
       ZeroCrossing and MeanConvolve write directly; the other eleven in that
       file build a kernel and delegate to one of them, so guarding the seven
       covers all eighteen. */
    CHECK(imProcessMeanConvolve(src, narrow, 3) == 0);
    CHECK(all_sentinel(narrow, 0x5A));
    CHECK(imProcessGaussianConvolve(src, narrow, 1.5) == 0);   /* delegates */
    CHECK(all_sentinel(narrow, 0x5A));
    CHECK(imProcessSobelConvolve(src, narrow) == 0);           /* delegates */
    CHECK(all_sentinel(narrow, 0x5A));
    CHECK(imProcessGrayMorphErode(src, narrow, 3) == 0);       /* delegates */
    CHECK(all_sentinel(narrow, 0x5A));

    imImageDestroy(narrow);
  }

  SUBCASE("a smaller destination is refused")
  {
    imImage* small = imImageCreate(GW / 2, GH / 2, IM_GRAY, IM_INT);
    REQUIRE(small != NULL);
    int* data = (int*)small->data[0];
    for (int i = 0; i < small->count; i++) data[i] = -1;

    CHECK(imProcessMeanConvolve(src, small, 3) == 0);
    for (int i = 0; i < small->count; i++)
    {
      CAPTURE(i);
      CHECK(data[i] == -1);
    }
    imImageDestroy(small);
  }

  SUBCASE("but a matching destination still works")
  {
    imImage* same = imImageCreate(GW, GH, IM_GRAY, IM_INT);
    REQUIRE(same != NULL);
    CHECK(imProcessMeanConvolve(src, same, 3) != 0);
    CHECK(imProcessGrayMorphDilate(src, same, 3) != 0);
    imImageDestroy(same);
  }

  imImageDestroy(src);
}

TEST_CASE("guards: the threshold and logic families check their destination")
{
  imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_INT);
  REQUIRE(src != NULL);
  for (int i = 0; i < GN; i++)
    ((int*)src->data[0])[i] = i * 10;

  SUBCASE("a threshold wants a byte target of the same size")
  {
    /* The result is binary, so the type deliberately differs from an int
       source -- what must match is the geometry. */
    imImage* wrong_size = imImageCreate(GW / 2, GH, IM_BINARY, IM_BYTE);
    REQUIRE(wrong_size != NULL);
    memset(wrong_size->data[0], 0x33, (size_t)wrong_size->count);

    imProcessThreshold(src, wrong_size, 50, 1);
    CHECK(all_sentinel(wrong_size, 0x33));
    imImageDestroy(wrong_size);

    imImage* right = imImageCreate(GW, GH, IM_BINARY, IM_BYTE);
    REQUIRE(right != NULL);
    imProcessThreshold(src, right, 50, 1);
    CHECK(((imbyte*)right->data[0])[0] == 0);
    CHECK(((imbyte*)right->data[0])[GN - 1] == 1);
    imImageDestroy(right);
  }

  SUBCASE("the bitwise operations want a matching destination")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0x44);
    imProcessBitwiseOp(src, src, narrow, IM_BIT_AND);
    CHECK(all_sentinel(narrow, 0x44));
    imProcessBitwiseNot(src, narrow);
    CHECK(all_sentinel(narrow, 0x44));
    imImageDestroy(narrow);
  }

  SUBCASE("tone gamut wants one too")
  {
    imImage* narrow = sentinel_byte_image(IM_GRAY, 0x44);
    imProcessToneGamut(src, narrow, IM_GAMUT_NORMALIZE, NULL);
    CHECK(all_sentinel(narrow, 0x44));
    imImageDestroy(narrow);
  }

  imImageDestroy(src);
}

TEST_CASE("guards: the point operations check the geometry they were given")
{
  /* These dispatch on the destination type already, so the type is safe --
     the sample count comes from the source, which is what a smaller
     destination cannot absorb. */
  imImage* src = imImageCreate(GW, GH, IM_GRAY, IM_BYTE);
  REQUIRE(src != NULL);

  imImage* small = imImageCreate(GW / 2, GH, IM_GRAY, IM_BYTE);
  REQUIRE(small != NULL);
  memset(small->data[0], 0x66, (size_t)small->count);

  imProcessUnArithmeticOp(src, small, IM_UN_EQL);
  CHECK(all_sentinel(small, 0x66));

  imImageDestroy(src);
  imImageDestroy(small);
}
#endif /* NDEBUG */


/* ================================================================== *
 * src/process/im_kernel.cpp -- the generated kernels, and which way
 * up they are.
 *
 * The matrices in im_kernel.h are pictures of the kernel, first row
 * topmost, while the arrays in im_kernel.cpp are in memory order,
 * which is bottom-up. So each picture is the vertical mirror of the
 * literal beside it, and the two agree precisely because of that.
 *
 * Sixteen of the twenty-one are symmetric about the horizontal axis,
 * so nothing distinguishes the two readings for them. Five are not,
 * and for those the reading decides the sign of the gradient. That is
 * an easy thing to get backwards -- one of the five was documented in
 * memory order rather than as a picture, which read as pictured
 * inverted its gradient, and anyone reconciling the header against
 * the source by flipping the array instead would silently invert the
 * other four.
 *
 * These cases assert the direction as behaviour, from the response to
 * a known ramp, so the header's claim about it cannot drift again
 * without a failure. They restate none of the weights: a case that
 * compared the array against the same numbers written a second time
 * would have passed throughout.
 * ================================================================== */

namespace {

/* A gray ramp. `axis` picks which way it climbs, in the terms a reader of the
   header would use: brighter upward means brighter toward the top of the
   picture, which is the HIGH end of the memory index. */
enum RampAxis { BRIGHTER_UPWARD, BRIGHTER_RIGHTWARD };

imImage* ramp(RampAxis axis)
{
  const int RW = 16, RH = 12;
  imImage* image = imImageCreate(RW, RH, IM_GRAY, IM_FLOAT);
  REQUIRE(image != NULL);
  for (int y = 0; y < RH; y++)
    for (int x = 0; x < RW; x++)
      ((float*)image->data[0])[y*RW + x] =
        (float)(10 * (axis == BRIGHTER_UPWARD? y: x));
  return image;
}

/* The response well away from the border, where the mirrored edge cannot
   reach. Float throughout: a byte destination would clip every negative
   response to zero and the sign is the whole point. */
double response(imImage* kernel, RampAxis axis)
{
  imImage* src = ramp(axis);
  imImage* dst = imImageCreateBased(src, -1, -1, -1, -1);
  REQUIRE(dst != NULL);
  REQUIRE(imProcessConvolve(src, dst, kernel) != 0);
  double value = ((float*)dst->data[0])[6*src->width + 8];
  imImageDestroy(src);
  imImageDestroy(dst);
  return value;
}

int kernel_is_y_symmetric(const imImage* kernel)
{
  const int* data = (const int*)kernel->data[0];
  for (int y = 0; y < kernel->height/2; y++)
    for (int x = 0; x < kernel->width; x++)
      if (data[y*kernel->width + x] !=
          data[(kernel->height - 1 - y)*kernel->width + x])
        return 0;
  return 1;
}

} /* namespace */

TEST_CASE("kernel: the asymmetric generators point the way the header says")
{
  SUBCASE("Sobel, Prewitt and Kirsh are upward gradients")
  {
    /* All three are pictured with their positive row on top and their negative
       row at the bottom, so each answers "how much brighter is it above than
       below" and none of them responds to a horizontal ramp at all. */
    struct { const char* name; imImage* (*make)(void); } upward[] = {
      { "Sobel",   imKernelSobel   },
      { "Prewitt", imKernelPrewitt },
      { "Kirsh",   imKernelKirsh   },
    };

    for (int i = 0; i < 3; i++)
    {
      CAPTURE(std::string(upward[i].name));
      imImage* kernel = upward[i].make();
      REQUIRE(kernel != NULL);
      CHECK(kernel_is_y_symmetric(kernel) == 0);   /* or the case proves nothing */
      CHECK(response(kernel, BRIGHTER_UPWARD) > 0.0);
      CHECK(response(kernel, BRIGHTER_RIGHTWARD) == 0.0);
      imImageDestroy(kernel);
    }
  }

  SUBCASE("Gradian3x3 is a pixel minus the one below it")
  {
    /* The one whose picture was upside down. Its weights are +1 at the centre
       and -1 one step away, so on a ramp of 10 per row the response is exactly
       the step -- which also pins the sign, since the inverted reading gives
       -10 rather than a different magnitude. */
    imImage* kernel = imKernelGradian3x3();
    REQUIRE(kernel != NULL);
    CHECK(kernel_is_y_symmetric(kernel) == 0);
    CHECK(response(kernel, BRIGHTER_UPWARD) == doctest::Approx(10.0));
    CHECK(response(kernel, BRIGHTER_RIGHTWARD) == 0.0);
    imImageDestroy(kernel);
  }

  SUBCASE("Gradian7x7 measures the other axis")
  {
    /* Not a bug, but not guessable from the name either: the 7x7 of the pair
       is a horizontal gradient where the 3x3 is a vertical one. Asserted so
       that the note saying so in the header is held to something. */
    imImage* kernel = imKernelGradian7x7();
    REQUIRE(kernel != NULL);
    CHECK(response(kernel, BRIGHTER_UPWARD) == 0.0);
    CHECK(response(kernel, BRIGHTER_RIGHTWARD) > 0.0);
    imImageDestroy(kernel);
  }

  SUBCASE("Sculpt is the bottom-right neighbour minus the top-left")
  {
    /* Diagonal, so it responds to both ramps -- and with opposite signs, which
       is what says it is that diagonal and not the other one. */
    imImage* kernel = imKernelSculpt();
    REQUIRE(kernel != NULL);
    CHECK(kernel_is_y_symmetric(kernel) == 0);
    CHECK(response(kernel, BRIGHTER_UPWARD) < 0.0);
    CHECK(response(kernel, BRIGHTER_RIGHTWARD) > 0.0);
    imImageDestroy(kernel);
  }
}

TEST_CASE("kernel: the rest are symmetric, so their orientation cannot matter")
{
  /* The claim the header makes about the other sixteen. If a future edit made
     one of them asymmetric without saying which way up it is meant to be read,
     this is what notices. */
  struct { const char* name; imImage* (*make)(void); } symmetric[] = {
    { "Mean3x3",          imKernelMean3x3          },
    { "Mean5x5",          imKernelMean5x5          },
    { "Mean7x7",          imKernelMean7x7          },
    { "CircularMean5x5",  imKernelCircularMean5x5  },
    { "CircularMean7x7",  imKernelCircularMean7x7  },
    { "Gaussian3x3",      imKernelGaussian3x3      },
    { "Gaussian5x5",      imKernelGaussian5x5      },
    { "Barlett5x5",       imKernelBarlett5x5       },
    { "TopHat5x5",        imKernelTopHat5x5        },
    { "TopHat7x7",        imKernelTopHat7x7        },
    { "Enhance",          imKernelEnhance          },
    { "Laplacian4",       imKernelLaplacian4       },
    { "Laplacian8",       imKernelLaplacian8       },
    { "Laplacian5x5",     imKernelLaplacian5x5     },
    { "Laplacian7x7",     imKernelLaplacian7x7     },
    { "Gradian7x7",       imKernelGradian7x7       },
  };
  const int count = (int)(sizeof(symmetric)/sizeof(symmetric[0]));
  CHECK(count == 16);

  for (int i = 0; i < count; i++)
  {
    CAPTURE(std::string(symmetric[i].name));
    imImage* kernel = symmetric[i].make();
    REQUIRE(kernel != NULL);
    CHECK(kernel->data_type == IM_INT);
    CHECK(imColorModeSpace(kernel->color_space) == IM_GRAY);
    CHECK(kernel_is_y_symmetric(kernel) == 1);
    imImageDestroy(kernel);
  }
}
