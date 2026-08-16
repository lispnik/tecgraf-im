/* Tests for the point and arithmetic operations:
 *
 *   src/process/im_point.cpp          the four generic callback drivers
 *   src/process/im_arithmetic_un.cpp  imProcessUnArithmeticOp
 *   src/process/im_arithmetic_bin.cpp the binary and multi-image operations
 *
 * These are the cheapest part of libim_process to pin down, because every
 * expected value can be written out by hand rather than compared against a
 * golden image. Integer data types are used wherever possible for exactly
 * that reason -- IM_INT and IM_BYTE results are exact, so a mismatch is a
 * defect rather than a rounding argument.
 *
 * Two behaviours are worth stating up front, because most of the cases below
 * turn on one of them:
 *
 *   - The arithmetic operations walk data[0] as one flat buffer of
 *     count*depth samples and deliberately stop short of the alpha plane.
 *     The point operations walk depth+1 planes and DO include alpha.
 *   - A byte destination crops to 0..255 (crop_byte in im_math_op.h); a wider
 *     destination keeps the full result. The same call therefore gives
 *     different answers depending only on the destination type.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_complex.h>
#include <im_process.h>

#include <math.h>
#include <string.h>

namespace {

/* Small and not square, so a transposed x/y cannot pass by coincidence. */
const int W = 4;
const int H = 3;
const int N = W * H;

imImage* create(int color_space, int data_type)
{
  imImage* image = imImageCreate(W, H, color_space, data_type);
  REQUIRE(image != NULL);
  return image;
}

int* ints(const imImage* image, int plane = 0)
{
  return (int*)image->data[plane];
}

imbyte* bytes(const imImage* image, int plane = 0)
{
  return (imbyte*)image->data[plane];
}

float* floats(const imImage* image, int plane = 0)
{
  return (float*)image->data[plane];
}

void set_ints(imImage* image, const int* values, int plane = 0)
{
  memcpy(image->data[plane], values, N * sizeof(int));
}

void set_bytes(imImage* image, const imbyte* values, int plane = 0)
{
  memcpy(image->data[plane], values, N * sizeof(imbyte));
}

void fill_ints(imImage* image, int value)
{
  int* data = ints(image);
  int total = image->count * image->depth;
  for (int i = 0; i < total; i++)
    data[i] = value;
}

void fill_bytes(imImage* image, imbyte value)
{
  memset(image->data[0], value, (size_t)image->count * image->depth);
}

/* Every sample carries its own flat index, so a callback can identify the
   sample it was handed without being told. */
void fill_flat_index(imImage* image)
{
  int* data = ints(image);
  int planes = image->has_alpha? image->depth + 1: image->depth;
  for (int i = 0; i < image->count * planes; i++)
    data[i] = i;
}

void check_ints(const imImage* image, const int* expected, int plane = 0)
{
  const int* actual = ints(image, plane);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(actual[i] == expected[i]);
  }
}

void check_bytes(const imImage* image, const imbyte* expected, int plane = 0)
{
  const imbyte* actual = bytes(image, plane);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)actual[i] == (int)expected[i]);
  }
}

/* ------------------------------------------------------------------ *
 * Callbacks. doctest runs each case in its own process under CTest, but the
 * whole binary shares one process when run directly, so every case resets
 * these before use.
 * ------------------------------------------------------------------ */

int g_calls;

struct Visit { int x, y, d; };
Visit g_visits[N * IM_MAXDEPTH];

double g_seen_params[2];
void*  g_seen_userdata;

void reset_callback_state()
{
  g_calls = 0;
  memset(g_visits, -1, sizeof(g_visits));
  g_seen_params[0] = g_seen_params[1] = 0;
  g_seen_userdata = NULL;
}

/* dst = 2*src + 1: monotonic, and distinct from src for every input, so a
   destination that was never written is obvious. */
int double_plus_one(double src_value, double* dst_value, double* params,
                    void* userdata, int x, int y, int d)
{
  (void)params; (void)userdata; (void)x; (void)y; (void)d;
  g_calls++;
  *dst_value = 2*src_value + 1;
  return 1;
}

/* Writes only even samples, to show that a zero return leaves the
   destination alone rather than writing a default. */
int only_even(double src_value, double* dst_value, double* params,
              void* userdata, int x, int y, int d)
{
  (void)params; (void)userdata; (void)x; (void)y; (void)d;
  g_calls++;
  if (((int)src_value) % 2 != 0)
    return 0;
  *dst_value = 1000;
  return 1;
}

int apply_params(double src_value, double* dst_value, double* params,
                 void* userdata, int x, int y, int d)
{
  (void)x; (void)y; (void)d;
  g_calls++;
  g_seen_params[0] = params[0];
  g_seen_params[1] = params[1];
  g_seen_userdata = userdata;
  *dst_value = src_value*params[0] + params[1];
  return 1;
}

int record_coords(double src_value, double* dst_value, double* params,
                  void* userdata, int x, int y, int d)
{
  (void)params; (void)userdata;
  int i = (int)src_value;
  if (i >= 0 && i < (int)(sizeof(g_visits)/sizeof(g_visits[0])))
  {
    g_visits[i].x = x;
    g_visits[i].y = y;
    g_visits[i].d = d;
  }
  g_calls++;
  *dst_value = src_value;
  return 1;
}

int sum_planes(const double* src_value, double* dst_value, double* params,
               void* userdata, int x, int y)
{
  (void)params; (void)userdata; (void)x; (void)y;
  g_calls++;
  dst_value[0] = src_value[0] + src_value[1] + src_value[2];
  return 1;
}

int record_coords_color(const double* src_value, double* dst_value,
                        double* params, void* userdata, int x, int y)
{
  (void)params; (void)userdata;
  int i = (int)src_value[0];
  if (i >= 0 && i < (int)(sizeof(g_visits)/sizeof(g_visits[0])))
  {
    g_visits[i].x = x;
    g_visits[i].y = y;
    g_visits[i].d = 0;
  }
  g_calls++;
  dst_value[0] = src_value[0];
  return 1;
}

int record_coords_multi(const double* src_value, double* dst_value,
                        double* params, void* userdata, int x, int y, int d,
                        int src_image_count)
{
  (void)params; (void)userdata; (void)src_image_count;
  int i = (int)src_value[0];
  if (i >= 0 && i < (int)(sizeof(g_visits)/sizeof(g_visits[0])))
  {
    g_visits[i].x = x;
    g_visits[i].y = y;
    g_visits[i].d = d;
  }
  g_calls++;
  *dst_value = src_value[0];
  return 1;
}

int record_coords_multi_color(double* src_value, double* dst_value,
                              double* params, void* userdata, int x, int y,
                              int src_image_count, int src_depth, int dst_depth)
{
  (void)params; (void)userdata; (void)src_image_count; (void)src_depth;
  int i = (int)src_value[0];
  if (i >= 0 && i < (int)(sizeof(g_visits)/sizeof(g_visits[0])))
  {
    g_visits[i].x = x;
    g_visits[i].y = y;
    g_visits[i].d = 0;
  }
  g_calls++;
  for (int d = 0; d < dst_depth; d++)
    dst_value[d] = src_value[0];
  return 1;
}

int sum_sources(const double* src_value, double* dst_value, double* params,
                void* userdata, int x, int y, int d, int src_image_count)
{
  (void)params; (void)userdata; (void)x; (void)y; (void)d;
  g_calls++;
  double total = 0;
  for (int i = 0; i < src_image_count; i++)
    total += src_value[i];
  *dst_value = total;
  return 1;
}

/* Source values are documented as copies, so scribbling on them must not
   reach the source image. imProcessMultipleMedian relies on this -- it
   qsorts the array in place. */
int clobber_sources(const double* src_value, double* dst_value, double* params,
                    void* userdata, int x, int y, int d, int src_image_count)
{
  (void)params; (void)userdata; (void)x; (void)y; (void)d;
  g_calls++;
  double* writable = (double*)src_value;
  for (int i = 0; i < src_image_count; i++)
    writable[i] = -1;
  *dst_value = 0;
  return 1;
}

int sum_sources_color(double* src_value, double* dst_value, double* params,
                      void* userdata, int x, int y, int src_image_count,
                      int src_depth, int dst_depth)
{
  (void)params; (void)userdata; (void)x; (void)y;
  g_calls++;
  for (int d = 0; d < dst_depth; d++)
  {
    double total = 0;
    for (int s = 0; s < src_image_count; s++)
      total += src_value[s*src_depth + d];
    dst_value[d] = total;
  }
  return 1;
}

} /* namespace */


/* ================================================================== *
 * Generic point operations -- src/process/im_point.cpp
 * ================================================================== */

TEST_CASE("point op: the function is called once per sample and its result is stored")
{
  reset_callback_state();

  imImage* src = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);
  fill_flat_index(src);

  CHECK(imProcessUnaryPointOp(src, dst, double_plus_one, NULL, NULL, NULL) != 0);

  /* Once per sample, not once per pixel: RGB is three planes. */
  CHECK(g_calls == N * 3);

  const int* data = ints(dst);
  for (int i = 0; i < N * 3; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == 2*i + 1);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("point op: a zero return leaves that sample untouched")
{
  reset_callback_state();

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  fill_flat_index(src);
  fill_ints(dst, -7);

  CHECK(imProcessUnaryPointOp(src, dst, only_even, NULL, NULL, NULL) != 0);
  CHECK(g_calls == N);

  /* The callback is still called for every sample -- it just declines to
     write the odd ones, which must keep the sentinel. */
  const int* data = ints(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == (i % 2 == 0? 1000: -7));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("point op: params and userdata are handed through unchanged")
{
  reset_callback_state();

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  fill_flat_index(src);

  double params[2] = { 3.0, 5.0 };
  int marker = 0;

  CHECK(imProcessUnaryPointOp(src, dst, apply_params, params, &marker, "test") != 0);

  CHECK(g_seen_params[0] == 3.0);
  CHECK(g_seen_params[1] == 5.0);
  CHECK(g_seen_userdata == &marker);

  const int* data = ints(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == 3*i + 5);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("point op: source and destination may be the same image")
{
  reset_callback_state();

  imImage* image = create(IM_GRAY, IM_INT);
  fill_flat_index(image);

  CHECK(imProcessUnaryPointOp(image, image, double_plus_one, NULL, NULL, NULL) != 0);

  const int* data = ints(image);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == 2*i + 1);
  }

  imImageDestroy(image);
}

TEST_CASE("point op: the alpha plane is processed, unlike in arithmetic ops")
{
  reset_callback_state();

  imImage* src = create(IM_RGB | IM_ALPHA, IM_INT);
  imImage* dst = create(IM_RGB | IM_ALPHA, IM_INT);
  REQUIRE(src->has_alpha != 0);
  fill_flat_index(src);

  CHECK(imProcessUnaryPointOp(src, dst, double_plus_one, NULL, NULL, NULL) != 0);

  /* Four planes, not three: imProcessUnaryPointOp adds one for alpha. */
  CHECK(g_calls == N * 4);

  const int* alpha = ints(dst, dst->depth);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(alpha[i] == 2*(3*N + i) + 1);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("point op: the function is told which pixel and plane it is on")
{
  /* This used to read the plane index out of i%count where i is the flat
     sample index, rather than i/count, and x and y inherited the inversion --
     from i=1 onwards the callback was handed coordinates that identified no
     pixel in the image, with y going negative almost immediately.

     The sample VALUES were never affected, because src_map[i] and dst_map[i]
     are indexed directly; only a callback that uses its coordinates was, so
     nothing else in this file noticed. Keep this case adjacent to the value
     assertions: the two failure modes are independent. */
  reset_callback_state();

  imImage* src = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);
  fill_flat_index(src);

  CHECK(imProcessUnaryPointOp(src, dst, record_coords, NULL, NULL, NULL) != 0);

  for (int i = 0; i < N * 3; i++)
  {
    CAPTURE(i);
    CHECK(g_visits[i].d == i / N);
    CHECK(g_visits[i].y == (i % N) / W);
    CHECK(g_visits[i].x == i % W);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("colour point op: the whole pixel arrives at once and the depth may change")
{
  reset_callback_state();

  imImage* src = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  fill_flat_index(src);

  CHECK(imProcessUnaryPointColorOp(src, dst, sum_planes, NULL, NULL, NULL) != 0);

  /* Once per pixel this time, not once per sample. */
  CHECK(g_calls == N);

  /* Pixel i holds i, N+i and 2N+i in its three planes. */
  const int* data = ints(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == i + (N + i) + (2*N + i));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("colour point op: the function is told which pixel it is on")
{
  /* The colour variants carried the same inversion in a different shape,
     y = i%width instead of i/width, which agrees for the first row and then
     diverges -- i=6 on a 4-wide image used to report x=-2. Worth its own
     case rather than folding into the one above: the two drivers compute
     their coordinates separately, so one can regress without the other. */
  reset_callback_state();

  imImage* src = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);
  fill_flat_index(src);

  CHECK(imProcessUnaryPointColorOp(src, dst, record_coords_color, NULL, NULL, NULL) != 0);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(g_visits[i].y == i / W);
    CHECK(g_visits[i].x == i % W);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("multi point op: one sample from each source reaches the function")
{
  reset_callback_state();

  imImage* a = create(IM_GRAY, IM_INT);
  imImage* b = create(IM_GRAY, IM_INT);
  imImage* c = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);

  fill_ints(a, 1);
  fill_ints(b, 20);
  fill_ints(c, 300);

  const imImage* sources[3] = { a, b, c };
  CHECK(imProcessMultiPointOp(sources, 3, dst, sum_sources, NULL, NULL, NULL) != 0);

  CHECK(g_calls == N);

  const int* data = ints(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(data[i] == 321);
  }

  imImageDestroy(a); imImageDestroy(b); imImageDestroy(c); imImageDestroy(dst);
}

TEST_CASE("multi point op: the source values are copies, safe to modify")
{
  reset_callback_state();

  imImage* a = create(IM_GRAY, IM_INT);
  imImage* b = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);

  fill_ints(a, 11);
  fill_ints(b, 22);

  const imImage* sources[2] = { a, b };
  CHECK(imProcessMultiPointOp(sources, 2, dst, clobber_sources, NULL, NULL, NULL) != 0);

  /* The callback overwrote every value it was given; the images must not
     have moved. imProcessMultipleMedian depends on this. */
  const int* da = ints(a);
  const int* db = ints(b);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(da[i] == 11);
    CHECK(db[i] == 22);
  }

  imImageDestroy(a); imImageDestroy(b); imImageDestroy(dst);
}

TEST_CASE("multi point op: the function is told which pixel and plane it is on")
{
  /* The fourth driver carrying the coordinate decomposition. Source 0 holds
     each sample's own flat index, so the callback can name the sample it was
     handed without being told. */
  reset_callback_state();

  imImage* a = create(IM_RGB, IM_INT);
  imImage* b = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);
  fill_flat_index(a);
  fill_ints(b, 0);

  const imImage* sources[2] = { a, b };
  CHECK(imProcessMultiPointOp(sources, 2, dst, record_coords_multi,
                              NULL, NULL, NULL) != 0);

  CHECK(g_calls == N * 3);

  for (int i = 0; i < N * 3; i++)
  {
    CAPTURE(i);
    CHECK(g_visits[i].d == i / N);
    CHECK(g_visits[i].y == (i % N) / W);
    CHECK(g_visits[i].x == i % W);
  }

  imImageDestroy(a); imImageDestroy(b); imImageDestroy(dst);
}

TEST_CASE("multi point colour op: the function is told which pixel it is on")
{
  reset_callback_state();

  imImage* a = create(IM_RGB, IM_INT);
  imImage* b = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);
  fill_flat_index(a);
  fill_ints(b, 0);

  const imImage* sources[2] = { a, b };
  CHECK(imProcessMultiPointColorOp(sources, 2, dst, record_coords_multi_color,
                                   NULL, NULL, NULL) != 0);

  CHECK(g_calls == N);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(g_visits[i].y == i / W);
    CHECK(g_visits[i].x == i % W);
  }

  imImageDestroy(a); imImageDestroy(b); imImageDestroy(dst);
}

TEST_CASE("multi point colour op: every plane of every source reaches the function")
{
  reset_callback_state();

  imImage* a = create(IM_RGB, IM_INT);
  imImage* b = create(IM_RGB, IM_INT);
  imImage* dst = create(IM_RGB, IM_INT);

  fill_flat_index(a);
  fill_ints(b, 100);

  const imImage* sources[2] = { a, b };
  CHECK(imProcessMultiPointColorOp(sources, 2, dst, sum_sources_color,
                                   NULL, NULL, NULL) != 0);

  CHECK(g_calls == N);

  for (int p = 0; p < 3; p++)
  {
    const int* data = ints(dst, p);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(p); CAPTURE(i);
      CHECK(data[i] == (p*N + i) + 100);
    }
  }

  imImageDestroy(a); imImageDestroy(b); imImageDestroy(dst);
}


/* ================================================================== *
 * Unary arithmetic -- src/process/im_arithmetic_un.cpp
 * ================================================================== */

TEST_CASE("unary arithmetic: the integer operations are exact")
{
  const int src_values[N] = { -4, -1, 0, 1, 2, 3, 9, 16, 25, 100, -100, 7 };

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(src, src_values);

  SUBCASE("IM_UN_EQL copies")
  {
    imProcessUnArithmeticOp(src, dst, IM_UN_EQL);
    check_ints(dst, src_values);
  }
  SUBCASE("IM_UN_ABS")
  {
    const int expected[N] = { 4, 1, 0, 1, 2, 3, 9, 16, 25, 100, 100, 7 };
    imProcessUnArithmeticOp(src, dst, IM_UN_ABS);
    check_ints(dst, expected);
  }
  SUBCASE("IM_UN_LESS negates")
  {
    const int expected[N] = { 4, 1, 0, -1, -2, -3, -9, -16, -25, -100, 100, -7 };
    imProcessUnArithmeticOp(src, dst, IM_UN_LESS);
    check_ints(dst, expected);
  }
  SUBCASE("IM_UN_SQR")
  {
    const int expected[N] = { 16, 1, 0, 1, 4, 9, 81, 256, 625, 10000, 10000, 49 };
    imProcessUnArithmeticOp(src, dst, IM_UN_SQR);
    check_ints(dst, expected);
  }
  SUBCASE("IM_UN_SQRT truncates, and floors a negative input at zero")
  {
    /* Integer input takes the integer sqrt overload in im_math_op.h, which
       returns 0 rather than a NaN for a negative value. */
    const int expected[N] = { 0, 0, 0, 1, 1, 1, 3, 4, 5, 10, 0, 2 };
    imProcessUnArithmeticOp(src, dst, IM_UN_SQRT);
    check_ints(dst, expected);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: a byte destination crops instead of wrapping")
{
  const imbyte src_values[N] = { 0, 1, 15, 16, 100, 200, 255, 2, 3, 4, 5, 6 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);
  set_bytes(src, src_values);

  SUBCASE("squares above 255 are cropped, not truncated to 8 bits")
  {
    /* 16*16 is 256: a wrap would give 0, the crop gives 255. */
    const imbyte expected[N] = { 0, 1, 225, 255, 255, 255, 255, 4, 9, 16, 25, 36 };
    imProcessUnArithmeticOp(src, dst, IM_UN_SQR);
    check_bytes(dst, expected);
  }
  SUBCASE("negation of unsigned data collapses to zero")
  {
    const imbyte expected[N] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    imProcessUnArithmeticOp(src, dst, IM_UN_LESS);
    check_bytes(dst, expected);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: a wider destination keeps what a byte one would crop")
{
  const imbyte src_values[N] = { 0, 1, 15, 16, 100, 200, 255, 2, 3, 4, 5, 6 };
  const int expected[N] = { 0, 1, 225, 256, 10000, 40000, 65025, 4, 9, 16, 25, 36 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_bytes(src, src_values);

  imProcessUnArithmeticOp(src, dst, IM_UN_SQR);
  check_ints(dst, expected);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: the alpha plane is left alone")
{
  imImage* src = create(IM_RGB | IM_ALPHA, IM_BYTE);
  imImage* dst = create(IM_RGB | IM_ALPHA, IM_BYTE);
  REQUIRE(src->has_alpha != 0);

  fill_bytes(src, 3);
  memset(src->data[src->depth], 77, N);
  memset(dst->data[dst->depth], 77, N);

  imProcessUnArithmeticOp(src, dst, IM_UN_SQR);

  const imbyte* colour = bytes(dst);
  for (int i = 0; i < N * 3; i++)
  {
    CAPTURE(i);
    CHECK((int)colour[i] == 9);
  }

  /* count*depth stops before the alpha plane, by design. */
  const imbyte* alpha = bytes(dst, dst->depth);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)alpha[i] == 77);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Binary arithmetic -- src/process/im_arithmetic_bin.cpp
 * ================================================================== */

TEST_CASE("binary arithmetic: the integer operations are exact")
{
  const int a[N] = { 10, 3, 7, 0, -5, 8, 2, 9, 100, 1, 6, 4 };
  const int b[N] = {  3, 10, 7, 5,  2, 1, 3, 3,   7, 1, 2, 2 };

  imImage* src1 = create(IM_GRAY, IM_INT);
  imImage* src2 = create(IM_GRAY, IM_INT);
  imImage* dst  = create(IM_GRAY, IM_INT);
  set_ints(src1, a);
  set_ints(src2, b);

  SUBCASE("IM_BIN_ADD")
  {
    const int expected[N] = { 13, 13, 14, 5, -3, 9, 5, 12, 107, 2, 8, 6 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_ADD);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_SUB is src1 - src2, in that order")
  {
    const int expected[N] = { 7, -7, 0, -5, -7, 7, -1, 6, 93, 0, 4, 2 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_SUB);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MUL")
  {
    const int expected[N] = { 30, 30, 49, 0, -10, 8, 6, 27, 700, 1, 12, 8 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_MUL);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_DIV truncates toward zero")
  {
    const int expected[N] = { 3, 0, 1, 0, -2, 8, 0, 3, 14, 1, 3, 2 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_DIV);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_DIFF is unsigned, unlike SUB")
  {
    const int expected[N] = { 7, 7, 0, 5, 7, 7, 1, 6, 93, 0, 4, 2 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_DIFF);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MIN")
  {
    const int expected[N] = { 3, 3, 7, 0, -5, 1, 2, 3, 7, 1, 2, 2 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_MIN);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MAX")
  {
    const int expected[N] = { 10, 10, 7, 5, 2, 8, 3, 9, 100, 1, 6, 4 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_MAX);
    check_ints(dst, expected);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("binary arithmetic: dividing by a zero sample is defined, not a trap")
{
  /* The same hazard as IM_UN_INV, reached from the other side: a zero in the
     second image is an ordinary black pixel, and integer division by zero is
     undefined -- SIGFPE on x86, a quiet 0 on ARM. div_op's integer overloads
     define it as 0.
     *
     * As with the IM_UN_INV case, running under -DIM_ENABLE_SANITIZERS=ON is
     * what makes this bite on every platform rather than only on x86. */
  const int a[N] = { 10, 3, 7, 0, -5, 8, 2, 9, 100, 1, 6, 4 };
  const int b[N] = {  0, 1, 0, 0,  2, 0, 3, 0,   7, 0, 2, 0 };
  const int expected[N] = { 0, 3, 0, 0, -2, 0, 0, 0, 14, 0, 3, 0 };

  imImage* src1 = create(IM_GRAY, IM_INT);
  imImage* src2 = create(IM_GRAY, IM_INT);
  imImage* dst  = create(IM_GRAY, IM_INT);
  set_ints(src1, a);
  set_ints(src2, b);

  SUBCASE("a zero divisor in the second image")
  {
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_DIV);
    check_ints(dst, expected);
  }
  SUBCASE("a zero constant divisor")
  {
    const int all_zero[N] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    imProcessArithmeticConstOp(src1, 0.0, dst, IM_BIN_DIV);
    check_ints(dst, all_zero);
  }
  SUBCASE("the byte path is guarded too")
  {
    /* Byte sources, not the int ones above: the header requires an integer
       target to have equal or more precision than the source, and nothing
       enforces it -- an IM_INT source with an IM_BYTE target writes ints
       into a byte-sized buffer and walks off the end of the allocation. */
    const imbyte ba[N] = { 10, 3, 7, 0, 200, 8, 2, 9, 100, 1, 6, 4 };
    const imbyte bb[N] = {  0, 1, 0, 0,   2, 0, 3, 0,   7, 0, 2, 0 };
    const imbyte expected_byte[N] = { 0, 3, 0, 0, 100, 0, 0, 0, 14, 0, 3, 0 };

    imImage* byte1 = create(IM_GRAY, IM_BYTE);
    imImage* byte2 = create(IM_GRAY, IM_BYTE);
    imImage* byte_dst = create(IM_GRAY, IM_BYTE);
    set_bytes(byte1, ba);
    set_bytes(byte2, bb);

    imProcessArithmeticOp(byte1, byte2, byte_dst, IM_BIN_DIV);
    check_bytes(byte_dst, expected_byte);

    imImageDestroy(byte1); imImageDestroy(byte2); imImageDestroy(byte_dst);
  }
  SUBCASE("a real destination still divides to an infinity")
  {
    /* Only the integer overloads were guarded; IEEE division is untouched. */
    imImage* float_dst = create(IM_GRAY, IM_FLOAT);
    imProcessArithmeticOp(src1, src2, float_dst, IM_BIN_DIV);

    const float* result = floats(float_dst);
    CHECK(isinf(result[0]));                              /*  10 / 0 */
    CHECK(result[0] > 0);
    CHECK(isnan(result[3]));                              /*   0 / 0 */
    CHECK(result[1] == doctest::Approx(3.0f));            /*   3 / 1 */
    CHECK(result[4] == doctest::Approx(-2.5f));           /*  -5 / 2 */
    CHECK(result[8] == doctest::Approx(100.0f/7.0f));
    imImageDestroy(float_dst);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("binary arithmetic: integer power uses repeated multiplication")
{
  /* Integer operands take the ipow overload rather than pow(), so the result
     is exact for large values instead of carrying a double's rounding. */
  const int a[N] = { 2, 3, 5, 10, 2, 2, 1, 0, 4, 3, 2, 7 };
  const int b[N] = { 3, 2, 2,  2, 10, 0, 5, 3, 2, 1, 1, 1 };
  const int expected[N] = { 8, 9, 25, 100, 1024, 1, 1, 0, 16, 3, 2, 7 };

  imImage* src1 = create(IM_GRAY, IM_INT);
  imImage* src2 = create(IM_GRAY, IM_INT);
  imImage* dst  = create(IM_GRAY, IM_INT);
  set_ints(src1, a);
  set_ints(src2, b);

  imProcessArithmeticOp(src1, src2, dst, IM_BIN_POW);
  check_ints(dst, expected);

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("binary arithmetic: a byte destination crops at both ends")
{
  const imbyte a[N] = { 200, 10, 255, 0, 128, 1, 250, 3, 4, 5, 6, 7 };
  const imbyte b[N] = { 100, 20,   2, 5, 128, 1,  10, 3, 4, 5, 6, 7 };

  imImage* src1 = create(IM_GRAY, IM_BYTE);
  imImage* src2 = create(IM_GRAY, IM_BYTE);
  imImage* dst  = create(IM_GRAY, IM_BYTE);
  set_bytes(src1, a);
  set_bytes(src2, b);

  SUBCASE("a sum over 255 stops at 255")
  {
    const imbyte expected[N] = { 255, 30, 255, 5, 255, 2, 255, 6, 8, 10, 12, 14 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_ADD);
    check_bytes(dst, expected);
  }
  SUBCASE("a negative difference stops at 0")
  {
    const imbyte expected[N] = { 100, 0, 253, 0, 0, 0, 240, 0, 0, 0, 0, 0 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_SUB);
    check_bytes(dst, expected);
  }
  SUBCASE("a product over 255 stops at 255")
  {
    const imbyte expected[N] = { 255, 200, 255, 0, 255, 1, 255, 9, 16, 25, 36, 49 };
    imProcessArithmeticOp(src1, src2, dst, IM_BIN_MUL);
    check_bytes(dst, expected);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("binary arithmetic: byte sources with a wider destination do not crop")
{
  /* Same inputs and same operation as the case above; only the destination
     type differs, and the cropping disappears. */
  const imbyte a[N] = { 200, 10, 255, 0, 128, 1, 250, 3, 4, 5, 6, 7 };
  const imbyte b[N] = { 100, 20,   2, 5, 128, 1,  10, 3, 4, 5, 6, 7 };
  const int expected[N] = { 300, 30, 257, 5, 256, 2, 260, 6, 8, 10, 12, 14 };

  imImage* src1 = create(IM_GRAY, IM_BYTE);
  imImage* src2 = create(IM_GRAY, IM_BYTE);
  imImage* dst  = create(IM_GRAY, IM_INT);
  set_bytes(src1, a);
  set_bytes(src2, b);

  imProcessArithmeticOp(src1, src2, dst, IM_BIN_ADD);
  check_ints(dst, expected);

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("binary arithmetic: every plane is processed, and alpha is not")
{
  imImage* src1 = create(IM_RGB | IM_ALPHA, IM_INT);
  imImage* src2 = create(IM_RGB | IM_ALPHA, IM_INT);
  imImage* dst  = create(IM_RGB | IM_ALPHA, IM_INT);
  REQUIRE(src1->has_alpha != 0);

  fill_flat_index(src1);
  fill_ints(src2, 1000);
  memset(src2->data[src2->depth], 0, N * sizeof(int));

  int* dst_alpha = ints(dst, dst->depth);
  for (int i = 0; i < N; i++)
    dst_alpha[i] = -1;

  imProcessArithmeticOp(src1, src2, dst, IM_BIN_ADD);

  for (int p = 0; p < 3; p++)
  {
    const int* data = ints(dst, p);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(p); CAPTURE(i);
      CHECK(data[i] == (p*N + i) + 1000);
    }
  }

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(dst_alpha[i] == -1);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("const arithmetic: the constant is always the right-hand operand")
{
  const int a[N] = { 10, 3, 7, 0, -5, 8, 2, 9, 100, 1, 6, 4 };

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(src, a);

  SUBCASE("IM_BIN_ADD")
  {
    const int expected[N] = { 13, 6, 10, 3, -2, 11, 5, 12, 103, 4, 9, 7 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_ADD);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_SUB is src - value, not value - src")
  {
    const int expected[N] = { 7, 0, 4, -3, -8, 5, -1, 6, 97, -2, 3, 1 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_SUB);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MUL")
  {
    const int expected[N] = { 30, 9, 21, 0, -15, 24, 6, 27, 300, 3, 18, 12 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_MUL);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_DIV is src / value, not value / src")
  {
    const int expected[N] = { 3, 1, 2, 0, -1, 2, 0, 3, 33, 0, 2, 1 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_DIV);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_DIFF")
  {
    const int expected[N] = { 7, 0, 4, 3, 8, 5, 1, 6, 97, 2, 3, 1 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_DIFF);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MIN clamps from above")
  {
    const int expected[N] = { 3, 3, 3, 0, -5, 3, 2, 3, 3, 1, 3, 3 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_MIN);
    check_ints(dst, expected);
  }
  SUBCASE("IM_BIN_MAX clamps from below")
  {
    const int expected[N] = { 10, 3, 7, 3, 3, 8, 3, 9, 100, 3, 6, 4 };
    imProcessArithmeticConstOp(src, 3.0, dst, IM_BIN_MAX);
    check_ints(dst, expected);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("const arithmetic: a byte destination crops as it does elsewhere")
{
  const imbyte a[N] = { 0, 1, 15, 16, 100, 200, 255, 2, 3, 4, 5, 6 };
  const imbyte expected[N] = { 0, 20, 255, 255, 255, 255, 255, 40, 60, 80, 100, 120 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);
  set_bytes(src, a);

  imProcessArithmeticConstOp(src, 20.0, dst, IM_BIN_MUL);
  check_bytes(dst, expected);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("blend: a constant alpha interpolates between the two sources")
{
  const imbyte a[N] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
  const imbyte b[N] = { 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240 };

  imImage* src1 = create(IM_GRAY, IM_BYTE);
  imImage* src2 = create(IM_GRAY, IM_BYTE);
  imImage* dst  = create(IM_GRAY, IM_BYTE);
  set_bytes(src1, a);
  set_bytes(src2, b);

  SUBCASE("alpha 1 is the first source")
  {
    imProcessBlendConst(src1, src2, dst, 1.0);
    check_bytes(dst, a);
  }
  SUBCASE("alpha 0 is the second source")
  {
    imProcessBlendConst(src1, src2, dst, 0.0);
    check_bytes(dst, b);
  }
  SUBCASE("alpha 0.5 is the midpoint")
  {
    const imbyte expected[N] = { 15, 30, 45, 60, 75, 90, 105, 120, 135, 150, 165, 180 };
    imProcessBlendConst(src1, src2, dst, 0.5);
    check_bytes(dst, expected);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("blend: an alpha image is reused across the colour planes")
{
  /* DoBlend indexes the alpha with i % alpha_count, so one gray plane covers
     all three planes of an RGB source rather than only the first. */
  const imbyte alpha_values[N] = { 0, 51, 102, 153, 204, 255, 0, 51, 102, 153, 204, 255 };
  const int expected[N] = { 200, 180, 160, 140, 120, 100, 200, 180, 160, 140, 120, 100 };

  imImage* src1  = create(IM_RGB, IM_BYTE);
  imImage* src2  = create(IM_RGB, IM_BYTE);
  imImage* alpha = create(IM_GRAY, IM_BYTE);
  imImage* dst   = create(IM_RGB, IM_BYTE);

  fill_bytes(src1, 100);
  fill_bytes(src2, 200);
  set_bytes(alpha, alpha_values);

  imProcessBlend(src1, src2, alpha, dst);

  for (int p = 0; p < 3; p++)
  {
    const imbyte* data = bytes(dst, p);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(p); CAPTURE(i);
      /* The endpoints are exact; the intermediate ratios are a/255, which is
         not a binary fraction, so allow the truncation to land either side. */
      if (alpha_values[i] == 0 || alpha_values[i] == 255)
        CHECK((int)data[i] == expected[i]);
      else
      {
        int off = (int)data[i] - expected[i];
        CHECK((off < 0? -off: off) <= 1);
      }
    }
  }

  imImageDestroy(src1); imImageDestroy(src2);
  imImageDestroy(alpha); imImageDestroy(dst);
}

TEST_CASE("background subtraction: what matches within tolerance becomes zero")
{
  const int a[N] = { 10, 10, 10, 10, 0, 100, 5, 5, 5, 5, 5, 5 };
  const int b[N] = { 10,  8, 12,  5, 3,  90, 5, 6, 4, 1, 9, 5 };

  imImage* src1 = create(IM_GRAY, IM_INT);
  imImage* src2 = create(IM_GRAY, IM_INT);
  imImage* dst  = create(IM_GRAY, IM_INT);
  set_ints(src1, a);
  set_ints(src2, b);

  SUBCASE("without show_diff the original sample survives")
  {
    const int expected[N] = { 0, 0, 0, 10, 0, 100, 0, 0, 0, 5, 5, 0 };
    imProcessBackSub(src1, src2, dst, 2.0, 0);
    check_ints(dst, expected);
  }
  SUBCASE("with show_diff the magnitude of the difference survives")
  {
    const int expected[N] = { 0, 0, 0, 5, 3, 10, 0, 0, 0, 4, 4, 0 };
    imProcessBackSub(src1, src2, dst, 2.0, 1);
    check_ints(dst, expected);
  }

  imImageDestroy(src1);
  imImageDestroy(src2);
  imImageDestroy(dst);
}

TEST_CASE("multiple mean: byte inputs are summed in a wider accumulator")
{
  /* The point of the case: three bright byte images sum past 255 before the
     division, so an accumulator of the input type would crop and give a
     visibly darker mean. */
  const imbyte a[N] = { 0, 10, 100, 200, 255, 255, 3, 6, 9, 12, 15, 18 };
  const imbyte b[N] = { 0, 20, 100, 200, 255, 250, 4, 7, 10, 13, 16, 19 };
  const imbyte c[N] = { 0, 30, 100, 200, 255, 245, 5, 8, 11, 14, 17, 20 };
  const imbyte expected[N] = { 0, 20, 100, 200, 255, 250, 4, 7, 10, 13, 16, 19 };

  imImage* i1 = create(IM_GRAY, IM_BYTE);
  imImage* i2 = create(IM_GRAY, IM_BYTE);
  imImage* i3 = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);
  set_bytes(i1, a);
  set_bytes(i2, b);
  set_bytes(i3, c);

  const imImage* sources[3] = { i1, i2, i3 };
  imProcessMultipleMean(sources, 3, dst);
  check_bytes(dst, expected);

  imImageDestroy(i1); imImageDestroy(i2); imImageDestroy(i3); imImageDestroy(dst);
}

TEST_CASE("multiple median: the middle sample wins, whatever the input order")
{
  const int a[N] = { 5, 1, 9, 0, 7, 2, 100, -5, 3, 3, 3, 8 };
  const int b[N] = { 1, 9, 5, 0, 2, 7,  50, -1, 3, 4, 5, 8 };
  const int c[N] = { 9, 5, 1, 0, 4, 4,  75, -3, 3, 5, 4, 8 };
  const int expected[N] = { 5, 5, 5, 0, 4, 4, 75, -3, 3, 4, 4, 8 };

  imImage* i1 = create(IM_GRAY, IM_INT);
  imImage* i2 = create(IM_GRAY, IM_INT);
  imImage* i3 = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(i1, a);
  set_ints(i2, b);
  set_ints(i3, c);

  const imImage* sources[3] = { i1, i2, i3 };
  CHECK(imProcessMultipleMedian(sources, 3, dst) != 0);
  check_ints(dst, expected);

  /* The first three columns are the same three values in three different
     orders and all give 5, which is what distinguishes a median from a
     "pick the second image". */
  imImageDestroy(i1); imImageDestroy(i2); imImageDestroy(i3); imImageDestroy(dst);
}

TEST_CASE("multiple standard deviation: the spread around a supplied mean")
{
  /* Two samples at 0 and 10 about a mean of 5: sqrt(((0-5)^2 + (10-5)^2)/2)
     is exactly 5, so no tolerance is needed.

     Note that the destination is accumulated into rather than assigned, so
     it must arrive zeroed -- imImageCreate does that. */
  imImage* i1 = create(IM_GRAY, IM_FLOAT);
  imImage* i2 = create(IM_GRAY, IM_FLOAT);
  imImage* mean = create(IM_GRAY, IM_FLOAT);
  imImage* dst = create(IM_GRAY, IM_FLOAT);

  float* d1 = floats(i1);
  float* d2 = floats(i2);
  float* dm = floats(mean);
  for (int i = 0; i < N; i++)
  {
    d1[i] = 0.0f;
    d2[i] = 10.0f;
    dm[i] = 5.0f;
  }

  const imImage* sources[2] = { i1, i2 };
  imProcessMultipleStdDev(sources, 2, mean, dst);

  const float* result = floats(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i] == doctest::Approx(5.0f));
  }

  imImageDestroy(i1); imImageDestroy(i2);
  imImageDestroy(mean); imImageDestroy(dst);
}


/* ================================================================== *
 * The rest of the unary operations
 * ================================================================== */

TEST_CASE("unary arithmetic: POSITIVES and NEGATIVES split a signed image by sign")
{
  const int src_values[N] = { -4, -1, 0, 1, 2, 3, 9, 16, 25, 100, -100, 7 };

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(src, src_values);

  SUBCASE("IM_UN_POSITIVES zeroes everything at or below zero")
  {
    const int expected[N] = { 0, 0, 0, 1, 2, 3, 9, 16, 25, 100, 0, 7 };
    imProcessUnArithmeticOp(src, dst, IM_UN_POSITIVES);
    check_ints(dst, expected);
  }
  SUBCASE("IM_UN_NEGATIVES zeroes everything above zero")
  {
    const int expected[N] = { -4, -1, 0, 0, 0, 0, 0, 0, 0, 0, -100, 0 };
    imProcessUnArithmeticOp(src, dst, IM_UN_NEGATIVES);
    check_ints(dst, expected);
  }

  /* Zero belongs to NEGATIVES: positives_op is "v > 0? v: 0" and
     negatives_op is "v > 0? 0: v", so the two are not complements. */
  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: inverting a zero sample is defined, not a trap")
{
  /* IM_UN_INV evaluates 1/v. For an integer destination that is integer
     division, so a zero sample used to be undefined behaviour -- SIGFPE on
     x86, a quiet 0 on ARM -- and a zero sample is just a black pixel. The
     overloads added to inv_op in im_math_op.h define it as 0.
     *
     * Running this under -DIM_ENABLE_SANITIZERS=ON is what makes the case
     * bite on every platform rather than only on x86: UBSan's
     * integer-divide-by-zero check fires on the unguarded expression whatever
     * the hardware does with it. */
  const imbyte src_values[N] = { 0, 1, 2, 1, 0, 255, 1, 0, 3, 1, 0, 100 };

  /* 1/1 is 1, 1/0 is now 0, and everything from 2 up truncates to 0. */
  const imbyte expected_byte[N] = { 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0 };
  const int expected_int[N] = { 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  set_bytes(src, src_values);

  SUBCASE("byte destination")
  {
    imImage* dst = create(IM_GRAY, IM_BYTE);
    imProcessUnArithmeticOp(src, dst, IM_UN_INV);
    check_bytes(dst, expected_byte);
    imImageDestroy(dst);
  }
  SUBCASE("int destination")
  {
    imImage* dst = create(IM_GRAY, IM_INT);
    imProcessUnArithmeticOp(src, dst, IM_UN_INV);
    check_ints(dst, expected_int);
    imImageDestroy(dst);
  }
  SUBCASE("ushort destination")
  {
    imImage* dst = create(IM_GRAY, IM_USHORT);
    imProcessUnArithmeticOp(src, dst, IM_UN_INV);

    const imushort* result = (const imushort*)dst->data[0];
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK((int)result[i] == expected_int[i]);
    }
    imImageDestroy(dst);
  }
  SUBCASE("short destination")
  {
    imImage* dst = create(IM_GRAY, IM_SHORT);
    imProcessUnArithmeticOp(src, dst, IM_UN_INV);

    const short* result = (const short*)dst->data[0];
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK((int)result[i] == expected_int[i]);
    }
    imImageDestroy(dst);
  }

  imImageDestroy(src);
}

TEST_CASE("unary arithmetic: inverting a signed image keeps the sign")
{
  /* The int overload has to leave negative values alone: 1/-1 is -1, and
     everything past -1 truncates toward zero like any C division. */
  const int src_values[N] = { 0, 1, -1, 2, -2, 0, 1, -1, 100, -100, 0, 1 };
  const int expected[N]   = { 0, 1, -1, 0,  0, 0, 1, -1,   0,    0, 0, 1 };

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(src, src_values);

  imProcessUnArithmeticOp(src, dst, IM_UN_INV);
  check_ints(dst, expected);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: a real destination still inverts to an infinity")
{
  /* Only the integer overloads were guarded. Float division by zero is
     defined by IEEE 754, so 1.0/0.0 must keep producing an infinity rather
     than quietly becoming the integer path's zero -- a caller scaling by the
     result needs to be able to tell the two apart. */
  const imbyte src_values[N] = { 0, 1, 2, 4, 5, 8, 10, 16, 20, 25, 40, 50 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_FLOAT);
  set_bytes(src, src_values);

  imProcessUnArithmeticOp(src, dst, IM_UN_INV);

  const float* result = floats(dst);
  CHECK(isinf(result[0]));
  CHECK(result[0] > 0);

  for (int i = 1; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i] == doctest::Approx(1.0f/src_values[i]));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: CONJ and CPXNORM are the identity on real data")
{
  /* im_arithmetic_un.cpp defines real-typed overloads of conj_op and
     cpxnorm_op that return their argument, so the two complex operations are
     safe to apply to a real image rather than being rejected. */
  const int src_values[N] = { -4, -1, 0, 1, 2, 3, 9, 16, 25, 100, -100, 7 };

  imImage* src = create(IM_GRAY, IM_INT);
  imImage* dst = create(IM_GRAY, IM_INT);
  set_ints(src, src_values);

  SUBCASE("IM_UN_CONJ")
  {
    imProcessUnArithmeticOp(src, dst, IM_UN_CONJ);
    check_ints(dst, src_values);
  }
  SUBCASE("IM_UN_CPXNORM")
  {
    imProcessUnArithmeticOp(src, dst, IM_UN_CPXNORM);
    check_ints(dst, src_values);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: a byte destination supports the sign and complex operations")
{
  /* DoUnaryOpByte used to have cases for ABS, INV, EQL, LESS, SQR, SQRT, LOG,
     SIN, COS and EXP and nothing else, so CONJ, CPXNORM, POSITIVES and
     NEGATIVES fell through its switch: a byte source with a byte destination
     wrote no samples at all and the caller kept whatever was there.

     The sentinel below is what exposes it. A freshly created destination does
     not, because imImageCreate zeroes it and zero is the right answer for
     NEGATIVES on unsigned data -- so three of these four subcases would have
     passed against a no-op. */
  const imbyte src_values[N] = { 0, 1, 15, 16, 100, 200, 255, 2, 3, 4, 5, 6 };
  const imbyte zeroes[N] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

  imImage* src = create(IM_GRAY, IM_BYTE);
  imImage* dst = create(IM_GRAY, IM_BYTE);
  set_bytes(src, src_values);

  SUBCASE("IM_UN_POSITIVES is the identity when nothing can be negative")
  {
    fill_bytes(dst, 99);
    imProcessUnArithmeticOp(src, dst, IM_UN_POSITIVES);
    check_bytes(dst, src_values);
  }
  SUBCASE("IM_UN_NEGATIVES clears everything when nothing can be negative")
  {
    fill_bytes(dst, 99);
    imProcessUnArithmeticOp(src, dst, IM_UN_NEGATIVES);
    check_bytes(dst, zeroes);
  }
  SUBCASE("IM_UN_CONJ is the identity")
  {
    fill_bytes(dst, 99);
    imProcessUnArithmeticOp(src, dst, IM_UN_CONJ);
    check_bytes(dst, src_values);
  }
  SUBCASE("IM_UN_CPXNORM is the identity")
  {
    fill_bytes(dst, 99);
    imProcessUnArithmeticOp(src, dst, IM_UN_CPXNORM);
    check_bytes(dst, src_values);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("unary arithmetic: the transcendental operations are the C library's")
{
  /* Unlike the integer cases above, the oracle here is <math.h> rather than a
     hand-written table -- the contract of IM_UN_LOG is "apply log", so
     restating log's values would test nothing. The landmarks that do pin the
     operation down (log 1 is 0, exp 0 is 1, and so on) are asserted exactly.

     IM_FLOAT throughout on purpose. An integer source routes through the
     int overloads in im_math_op.h, where log(0) is -inf and exp(100) is far
     outside int, and casting either back to int is undefined. */
  const float safe[N] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f,
                          -2.0f, 3.0f, 0.25f, -0.25f, 1.5f, -1.5f };
  const float positive[N] = { 1.0f, 2.0f, 0.5f, 10.0f, 100.0f, 0.25f,
                              3.0f, 4.0f, 5.0f, 7.0f, 20.0f, 0.125f };

  imImage* src = create(IM_GRAY, IM_FLOAT);
  imImage* dst = create(IM_GRAY, IM_FLOAT);

  SUBCASE("IM_UN_LOG is the natural logarithm, not log10")
  {
    memcpy(src->data[0], positive, sizeof(positive));
    imProcessUnArithmeticOp(src, dst, IM_UN_LOG);

    const float* result = floats(dst);
    CHECK(result[0] == 0.0f);                                /* log 1 */
    CHECK(result[3] == doctest::Approx(2.302585f));          /* log 10, not 1 */
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(result[i] == doctest::Approx(logf(positive[i])));
    }
  }
  SUBCASE("IM_UN_EXP")
  {
    memcpy(src->data[0], safe, sizeof(safe));
    imProcessUnArithmeticOp(src, dst, IM_UN_EXP);

    const float* result = floats(dst);
    CHECK(result[0] == 1.0f);                                /* exp 0 */
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(result[i] == doctest::Approx(expf(safe[i])));
    }
  }
  SUBCASE("IM_UN_SIN takes radians, not degrees")
  {
    memcpy(src->data[0], safe, sizeof(safe));
    imProcessUnArithmeticOp(src, dst, IM_UN_SIN);

    const float* result = floats(dst);
    CHECK(result[0] == 0.0f);                                /* sin 0 */
    CHECK(result[1] == doctest::Approx(0.841471f));          /* sin 1 rad */
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(result[i] == doctest::Approx(sinf(safe[i])));
    }
  }
  SUBCASE("IM_UN_COS takes radians, not degrees")
  {
    memcpy(src->data[0], safe, sizeof(safe));
    imProcessUnArithmeticOp(src, dst, IM_UN_COS);

    const float* result = floats(dst);
    CHECK(result[0] == 1.0f);                                /* cos 0 */
    CHECK(result[1] == doctest::Approx(0.540302f));          /* cos 1 rad */
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(result[i] == doctest::Approx(cosf(safe[i])));
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Complex images
 * ================================================================== */

namespace {

/* A spread of samples that exercises every quadrant plus both axes and the
   origin, so a sign slip or a swapped component cannot hide. */
const float CPX_RE[N] = { 3, 0, 1, 0, -1,  0, -3, 2, 5,  0, -4,  1 };
const float CPX_IM[N] = { 4, 0, 0, 1,  0, -1,  4, 2, 0, -3,  3, -1 };

imcfloat* cpx(const imImage* image, int plane = 0)
{
  return (imcfloat*)image->data[plane];
}

void set_complex(imImage* image, const float* re, const float* im)
{
  imcfloat* data = cpx(image);
  for (int i = 0; i < N; i++)
  {
    data[i].real = re[i];
    data[i].imag = im[i];
  }
}

} /* namespace */

TEST_CASE("complex: CONJ negates the imaginary part and leaves the real part")
{
  imImage* src = create(IM_GRAY, IM_CFLOAT);
  imImage* dst = create(IM_GRAY, IM_CFLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessUnArithmeticOp(src, dst, IM_UN_CONJ);

  const imcfloat* result = cpx(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i].real == CPX_RE[i]);
    CHECK(result[i].imag == -CPX_IM[i]);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("complex: CPXNORM projects onto the unit circle and leaves the origin alone")
{
  imImage* src = create(IM_GRAY, IM_CFLOAT);
  imImage* dst = create(IM_GRAY, IM_CFLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessUnArithmeticOp(src, dst, IM_UN_CPXNORM);

  const imcfloat* result = cpx(dst);

  /* (3,4) has magnitude 5, so it lands on (0.6, 0.8) exactly. */
  CHECK(result[0].real == doctest::Approx(0.6f));
  CHECK(result[0].imag == doctest::Approx(0.8f));

  /* The origin has no direction; cpxnorm_op returns (0,0) rather than
     dividing by zero. */
  CHECK(result[1].real == 0.0f);
  CHECK(result[1].imag == 0.0f);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    float mag = sqrtf(result[i].real*result[i].real +
                      result[i].imag*result[i].imag);
    float expected = (CPX_RE[i] == 0 && CPX_IM[i] == 0)? 0.0f: 1.0f;
    CHECK(mag == doctest::Approx(expected));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("complex: a rectangular split separates the two components")
{
  imImage* src  = create(IM_GRAY, IM_CFLOAT);
  imImage* real = create(IM_GRAY, IM_FLOAT);
  imImage* imag = create(IM_GRAY, IM_FLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessSplitComplex(src, real, imag, 0);

  const float* re = floats(real);
  const float* im = floats(imag);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(re[i] == CPX_RE[i]);
    CHECK(im[i] == CPX_IM[i]);
  }

  imImageDestroy(src); imImageDestroy(real); imImageDestroy(imag);
}

TEST_CASE("complex: a rectangular merge is the exact inverse of the split")
{
  imImage* src  = create(IM_GRAY, IM_CFLOAT);
  imImage* real = create(IM_GRAY, IM_FLOAT);
  imImage* imag = create(IM_GRAY, IM_FLOAT);
  imImage* back = create(IM_GRAY, IM_CFLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessSplitComplex(src, real, imag, 0);
  imProcessMergeComplex(real, imag, back, 0);

  const imcfloat* result = cpx(back);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i].real == CPX_RE[i]);
    CHECK(result[i].imag == CPX_IM[i]);
  }

  imImageDestroy(src); imImageDestroy(real);
  imImageDestroy(imag); imImageDestroy(back);
}

TEST_CASE("complex: a polar split reports the magnitude")
{
  imImage* src   = create(IM_GRAY, IM_CFLOAT);
  imImage* mag   = create(IM_GRAY, IM_FLOAT);
  imImage* phase = create(IM_GRAY, IM_FLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessSplitComplex(src, mag, phase, 1);

  const float* m = floats(mag);
  CHECK(m[0] == doctest::Approx(5.0f));   /* the 3-4-5 triangle */
  CHECK(m[1] == 0.0f);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(m[i] == doctest::Approx(sqrtf(CPX_RE[i]*CPX_RE[i] +
                                        CPX_IM[i]*CPX_IM[i])));
  }

  imImageDestroy(src); imImageDestroy(mag); imImageDestroy(phase);
}

TEST_CASE("complex: a polar split reports the phase measured from the real axis")
{
  /* cpxphase used to pass atan2 its arguments the wrong way round, measuring
     the angle from the imaginary axis: (1,0) came back as pi/2 rather than 0.
     Nothing compensated for it -- cpxphase has only two callers, this one and
     the IM_CPX_PHASE conversion in im_converttype.cpp, and im_fft.cpp does no
     phase arithmetic of its own -- so both were simply wrong. */
  imImage* src   = create(IM_GRAY, IM_CFLOAT);
  imImage* mag   = create(IM_GRAY, IM_FLOAT);
  imImage* phase = create(IM_GRAY, IM_FLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessSplitComplex(src, mag, phase, 1);

  const float* p = floats(phase);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(p[i] == doctest::Approx(atan2f(CPX_IM[i], CPX_RE[i])));
  }

  imImageDestroy(src); imImageDestroy(mag); imImageDestroy(phase);
}

TEST_CASE("complex: a polar split and merge returns the original image")
{
  /* The two halves used to disagree about the unit. doSplitComplex stores
     cpxphase, an atan2 result in radians; doMergeComplex read it as degrees,
     wrapping above 180 and dividing by 180/pi, so it divided an angle that
     was already in radians and collapsed every sample towards the positive
     real axis. Radians won, because cpxpolar and every other angle in
     im_complex.h are radians and merge was the sole outlier. */
  imImage* src   = create(IM_GRAY, IM_CFLOAT);
  imImage* mag   = create(IM_GRAY, IM_FLOAT);
  imImage* phase = create(IM_GRAY, IM_FLOAT);
  imImage* back  = create(IM_GRAY, IM_CFLOAT);
  set_complex(src, CPX_RE, CPX_IM);

  imProcessSplitComplex(src, mag, phase, 1);
  imProcessMergeComplex(mag, phase, back, 1);

  const imcfloat* result = cpx(back);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i].real == doctest::Approx(CPX_RE[i]).epsilon(0.001));
    CHECK(result[i].imag == doctest::Approx(CPX_IM[i]).epsilon(0.001));
  }

  imImageDestroy(src); imImageDestroy(mag);
  imImageDestroy(phase); imImageDestroy(back);
}

TEST_CASE("complex: MultiplyConj is Conj(src1) * src2, in that order")
{
  /* The header documents "Conj(img1) * img2", which expands to
       (a.re - i*a.im)(b.re + i*b.im)
         = (a.re*b.re + a.im*b.im) + i*(a.re*b.im - a.im*b.re)
     Note the sign of the imaginary part: src1 * Conj(src2) would give its
     negation, and the real part alone cannot tell the two apart. */
  const float b_re[N] = { 1, 2, -1, 0, 2, 3,  1, -2, 4, 1,  0, 2 };
  const float b_im[N] = { 2, 1,  3, 5, 0, 1, -1,  2, 0, 1, -2, 3 };

  imImage* src1 = create(IM_GRAY, IM_CFLOAT);
  imImage* src2 = create(IM_GRAY, IM_CFLOAT);
  imImage* dst  = create(IM_GRAY, IM_CFLOAT);
  set_complex(src1, CPX_RE, CPX_IM);
  set_complex(src2, b_re, b_im);

  imProcessMultiplyConj(src1, src2, dst);

  const imcfloat* result = cpx(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i].real == doctest::Approx(CPX_RE[i]*b_re[i] + CPX_IM[i]*b_im[i]));
    CHECK(result[i].imag == doctest::Approx(CPX_RE[i]*b_im[i] - CPX_IM[i]*b_re[i]));
  }

  /* Sample 0 written out, so the case does not rest entirely on a formula
     restated from the source: Conj(3+4i) * (1+2i) = (3-4i)(1+2i) = 11 + 2i. */
  CHECK(result[0].real == doctest::Approx(11.0f));
  CHECK(result[0].imag == doctest::Approx(2.0f));

  imImageDestroy(src1); imImageDestroy(src2); imImageDestroy(dst);
}

TEST_CASE("complex: MultiplyConj can write into one of its own sources")
{
  /* The loop stages each result in a local before storing it, specifically so
     that dst may alias a source. */
  imImage* src1 = create(IM_GRAY, IM_CFLOAT);
  imImage* src2 = create(IM_GRAY, IM_CFLOAT);
  imImage* dst  = create(IM_GRAY, IM_CFLOAT);
  const float b_re[N] = { 1, 2, -1, 0, 2, 3,  1, -2, 4, 1,  0, 2 };
  const float b_im[N] = { 2, 1,  3, 5, 0, 1, -1,  2, 0, 1, -2, 3 };

  set_complex(src1, CPX_RE, CPX_IM);
  set_complex(src2, b_re, b_im);
  imProcessMultiplyConj(src1, src2, dst);

  imImage* in_place = create(IM_GRAY, IM_CFLOAT);
  set_complex(in_place, CPX_RE, CPX_IM);
  imProcessMultiplyConj(in_place, src2, in_place);

  const imcfloat* expected = cpx(dst);
  const imcfloat* actual = cpx(in_place);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(actual[i].real == expected[i].real);
    CHECK(actual[i].imag == expected[i].imag);
  }

  imImageDestroy(src1); imImageDestroy(src2);
  imImageDestroy(dst); imImageDestroy(in_place);
}


/* ================================================================== *
 * Alpha compositing -- imProcessCompose
 * ================================================================== */

namespace {

/* One pixel per branch of compose_op, so a single run covers the whole
   decision tree rather than needing a case each. Order matters only in that
   the expected tables below are written in the same order. */
const imbyte COMPOSE_A1[N] = { 255, 255,   0,  0, 128, 128,  64, 128,  64, 200, 255,  0 };
const imbyte COMPOSE_A2[N] = {   0, 128, 255, 77,   0, 255, 255, 128, 192,  50, 255,  0 };

/* Constant per plane, so the expected values stay legible. */
const imbyte COMPOSE_V1[3] = { 200, 100,  50 };
const imbyte COMPOSE_V2[3] = {  40,  80, 240 };

imImage* compose_source(const imbyte* colour, const imbyte* alpha)
{
  imImage* image = imImageCreate(W, H, IM_RGB | IM_ALPHA, IM_BYTE);
  REQUIRE(image != NULL);
  for (int p = 0; p < 3; p++)
    memset(image->data[p], colour[p], N);
  memcpy(image->data[image->depth], alpha, N);
  return image;
}

} /* namespace */

TEST_CASE("compose: the whole SRC-over-DST decision tree")
{
  /* Expected values worked through by hand from compose_op, one column per
     colour plane. The four simple branches (opaque source, transparent
     source, transparent target, opaque target) and the general closed-form
     case are all represented. */
  const int expected[3][N] = {
    /* plane 0: v1=200 v2=40  */
    { 200, 200,  40,  40, 200, 120,  80, 146,  89, 191, 200,  40 },
    /* plane 1: v1=100 v2=80  */
    { 100, 100,  80,  80, 100,  90,  85,  93,  86,  98, 100,  80 },
    /* plane 2: v1=50  v2=240 */
    {  50,  50, 240, 240,  50, 144, 192, 113, 181,  59,  50, 240 },
  };
  /* And the alpha channel, from compose_alpha_op. */
  const int expected_alpha[N] = { 255, 255, 255, 77, 128, 255, 255, 191, 207, 210, 255, 0 };

  imImage* src1 = compose_source(COMPOSE_V1, COMPOSE_A1);
  imImage* src2 = compose_source(COMPOSE_V2, COMPOSE_A2);
  imImage* dst  = create(IM_RGB | IM_ALPHA, IM_BYTE);

  imProcessCompose(src1, src2, dst);

  for (int p = 0; p < 3; p++)
  {
    const imbyte* data = bytes(dst, p);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(p); CAPTURE(i);
      CAPTURE((int)COMPOSE_A1[i]); CAPTURE((int)COMPOSE_A2[i]);
      CHECK((int)data[i] == expected[p][i]);
    }
  }

  const imbyte* alpha = bytes(dst, dst->depth);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)alpha[i] == expected_alpha[i]);
  }

  imImageDestroy(src1); imImageDestroy(src2); imImageDestroy(dst);
}

TEST_CASE("compose: an image without an alpha channel is declined, not composed")
{
  /* imProcessCompose returns early unless all three images carry alpha, so
     the destination must come back exactly as it went in. */
  imImage* src1 = compose_source(COMPOSE_V1, COMPOSE_A1);
  imImage* src2 = create(IM_RGB, IM_BYTE);          /* no alpha */
  imImage* dst  = create(IM_RGB | IM_ALPHA, IM_BYTE);

  fill_bytes(src2, 7);
  fill_bytes(dst, 99);
  memset(dst->data[dst->depth], 99, N);

  imProcessCompose(src1, src2, dst);

  for (int p = 0; p < 3; p++)
  {
    const imbyte* data = bytes(dst, p);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(p); CAPTURE(i);
      CHECK((int)data[i] == 99);
    }
  }

  imImageDestroy(src1); imImageDestroy(src2); imImageDestroy(dst);
}

TEST_CASE("compose: an opaque source stays opaque above 8 bits")
{
  /* compose_alpha_op used to end with a cast hard-coded to unsigned char,
     even though max comes from imColorMax and is 65535 for IM_USHORT, 32767
     for IM_SHORT and 8388607 for IM_INT. Composing an opaque 16-bit source
     wrote an alpha of 255 -- not opaque but very nearly transparent -- while
     the colour planes, which take the correct alpha1 == max branch, looked
     right. The byte case above could never have caught it, because
     (unsigned char)255 is 255. */
  const int type_max = 65535;

  imImage* src1 = imImageCreate(W, H, IM_RGB | IM_ALPHA, IM_USHORT);
  imImage* src2 = imImageCreate(W, H, IM_RGB | IM_ALPHA, IM_USHORT);
  imImage* dst  = imImageCreate(W, H, IM_RGB | IM_ALPHA, IM_USHORT);
  REQUIRE(src1 != NULL); REQUIRE(src2 != NULL); REQUIRE(dst != NULL);

  imushort* a1 = (imushort*)src1->data[src1->depth];
  imushort* a2 = (imushort*)src2->data[src2->depth];
  for (int i = 0; i < N; i++)
  {
    a1[i] = (imushort)type_max;    /* fully opaque source */
    a2[i] = (imushort)(type_max/2);
  }

  imProcessCompose(src1, src2, dst);

  const imushort* alpha = (const imushort*)dst->data[dst->depth];
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)alpha[i] == type_max);
  }

  imImageDestroy(src1); imImageDestroy(src2); imImageDestroy(dst);
}


/* ================================================================== *
 * Auto covariance
 * ================================================================== */

TEST_CASE("auto covariance: an image equal to its own mean has none")
{
  imImage* src  = create(IM_GRAY, IM_FLOAT);
  imImage* mean = create(IM_GRAY, IM_FLOAT);
  imImage* dst  = create(IM_GRAY, IM_FLOAT);

  float* s = floats(src);
  float* m = floats(mean);
  for (int i = 0; i < N; i++)
  {
    s[i] = 17.0f + i;
    m[i] = 17.0f + i;
  }

  CHECK(imProcessAutoCovariance(src, mean, dst) != 0);

  const float* result = floats(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i] == 0.0f);
  }

  imImageDestroy(src); imImageDestroy(mean); imImageDestroy(dst);
}

TEST_CASE("auto covariance: the sign follows the pattern and the overlap shrinks")
{
  /* A vertical stripe pattern: every sample is +2 on even columns and -2 on
     odd ones, about a mean of zero. The product of two samples then depends
     only on the horizontal offset -- +4 for an even offset, -4 for an odd one
     -- so the whole result is the size of the overlap times that sign,
     divided by width*height.
     *
     * Destination [y*W + x] holds the covariance at offset (x, y), and the
     * overlap at that offset is (H-y) * (W-x). On a 4x3 image:
     *
     *   offset (0,0): 3*4 = 12 samples, +4 each, /12  ->  +4
     *   offset (1,0): 3*3 =  9 samples, -4 each, /12  ->  -3
     *   offset (3,2): 1*1 =  1 sample,  -4,      /12  ->  -1/3
     *
     * A dropped minus sign, a transposed offset or an overlap computed from
     * the wrong dimension all break this; a division by the overlap instead
     * of by the sample count would break it too. */
  const float expected[N] = {
     4.0f,       -3.0f,        2.0f,       -1.0f,
     8.0f/3.0f,  -2.0f,        4.0f/3.0f,  -2.0f/3.0f,
     4.0f/3.0f,  -1.0f,        2.0f/3.0f,  -1.0f/3.0f,
  };

  imImage* src  = create(IM_GRAY, IM_FLOAT);
  imImage* mean = create(IM_GRAY, IM_FLOAT);
  imImage* dst  = create(IM_GRAY, IM_FLOAT);

  float* s = floats(src);
  float* m = floats(mean);
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      s[y*W + x] = (x % 2 == 0)? 2.0f: -2.0f;
      m[y*W + x] = 0.0f;
    }
  }

  CHECK(imProcessAutoCovariance(src, mean, dst) != 0);

  const float* result = floats(dst);
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(result[i] == doctest::Approx(expected[i]));
  }

  imImageDestroy(src); imImageDestroy(mean); imImageDestroy(dst);
}
