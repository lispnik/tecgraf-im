/* Covers src/process/im_decorrelate.cpp -- the decorrelation stretch.
 *
 * Most of these assert exact identities rather than tolerances, which is
 * unusual for an image operation and worth explaining. The transform is
 *
 *   T = diag(target) . Sigma^(-1/2)
 *
 * so the covariance of the result is diag(target^2) exactly, not
 * approximately: the whitening is algebraically exact and the off-diagonal
 * terms cancel to rounding. That only survives if nothing quantizes on the
 * way out, so the exact cases all run on IM_DOUBLE images and the byte cases
 * get a tolerance instead.
 *
 * The invariant is also stated in the WORKING space, not in RGB. For
 * IM_DECORR_RGB the two are the same thing; for the others the result is
 * decorrelated along the working space's axes and correlated again once it is
 * mapped back, which is the point of having more than one space.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_process.h>

#include <math.h>
#include <string.h>
#include <string>

namespace
{
  /* Deliberately not square, so a transform that transposed its indices could
     not pass, and large enough that the sample covariance is well determined. */
  const int W = 61;
  const int H = 43;

  imImage* create(int width, int height, int color_space, int data_type)
  {
    imImage* image = imImageCreate(width, height, color_space, data_type);
    REQUIRE(image != NULL);
    return image;
  }

  /* A deterministic generator. The suite must not depend on the platform's
     rand(), and the cloud has to be reproducible for the in-place and
     save-and-reapply comparisons to mean anything. */
  struct Random
  {
    unsigned int state;
    explicit Random(unsigned int seed) : state(seed) {}

    double uniform()
    {
      state = state*1103515245u + 12345u;
      return ((state >> 16) & 0x7fff) / 32768.0;
    }

    double normal()
    {
      double a = uniform(), b = uniform();
      return sqrt(-2.0*log(a + 1e-12)) * cos(2.0*3.14159265358979323846*b);
    }
  };

  /* Colours strung out along one axis: the shape of a faded pigment against
     rock, and the case the whole operation exists for. Centred so that a
     stretch at scale 1 stays inside 0-1 and nothing clips. */
  void fill_correlated(imImage* image, unsigned int seed = 12345)
  {
    double** data = (double**)image->data;
    Random r(seed);

    for (int i = 0; i < image->count; i++)
    {
      double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();

      data[0][i] = 0.50 + 0.07*u0;
      data[1][i] = 0.46 + 0.07*(0.97*u0 + 0.2431*u1);
      data[2][i] = 0.41 + 0.07*(0.94*u0 + 0.2465*u1 + 0.2323*u2);
    }
  }

  /* Mean and covariance of the three planes, computed here rather than by the
     library, so the assertions do not check the code against itself. */
  void measure(const imImage* image, double* mean, double* cov)
  {
    double** data = (double**)image->data;
    int n = image->count;

    for (int k = 0; k < 3; k++)
    {
      mean[k] = 0.0;
      for (int i = 0; i < n; i++)
        mean[k] += data[k][i];
      mean[k] /= n;
    }

    for (int a = 0; a < 3; a++)
    {
      for (int b = 0; b < 3; b++)
      {
        double s = 0.0;
        for (int i = 0; i < n; i++)
          s += (data[a][i] - mean[a]) * (data[b][i] - mean[b]);
        cov[a*3+b] = s/(n - 1.0);
      }
    }
  }

  double det3(const double* m)
  {
    return m[0]*(m[4]*m[8] - m[5]*m[7])
         - m[1]*(m[3]*m[8] - m[5]*m[6])
         + m[2]*(m[3]*m[7] - m[4]*m[6]);
  }

  bool same_bytes(const imImage* a, const imImage* b)
  {
    for (int p = 0; p < 3; p++)
    {
      if (memcmp(a->data[p], b->data[p], a->plane_size) != 0)
        return false;
    }
    return true;
  }
}


/* ====== The identity the whole operation rests on ====== */

TEST_CASE("decorrelate: the stretched bands are uncorrelated and keep their own spread")
{
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* dst = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  double in_mean[3], in_cov[9];
  measure(src, in_mean, in_cov);

  /* the input really is nearly collinear, or the test proves nothing */
  double correlation = in_cov[1] / sqrt(in_cov[0]*in_cov[4]);
  CHECK(correlation > 0.9);

  double scale;
  SUBCASE("no extra gain") { scale = 1.0; }
  SUBCASE("doubled")       { scale = 2.0; }

  REQUIRE(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, scale) != 0);

  double out_mean[3], out_cov[9];
  measure(dst, out_mean, out_cov);

  /* Working space is RGB here, so this is the invariant directly: the
     off-diagonal covariance is gone and each band sits at scale times the
     standard deviation it started with. */
  for (int a = 0; a < 3; a++)
  {
    CAPTURE(a);
    CHECK(out_cov[a*3+a] == doctest::Approx(scale*scale*in_cov[a*3+a]).epsilon(1e-9));
    CHECK(out_mean[a] == doctest::Approx(in_mean[a]).epsilon(1e-9));

    for (int b = 0; b < 3; b++)
    {
      if (a == b) continue;
      CAPTURE(b);
      /* relative to the variance, not absolute: an epsilon on a covariance
         means nothing without the scale it is measured against */
      CHECK(fabs(out_cov[a*3+b]) < 1e-9 * sqrt(in_cov[a*3+a]*in_cov[b*3+b]));
    }
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: the transform's determinant is fixed by the covariance alone")
{
  /* det T = product(target) / sqrt(det Sigma). An exact scalar identity, and
     it catches a transposed matrix, a mis-signed inverse and a wrong target
     without needing any reference data. */
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  double in_mean[3], in_cov[9];
  measure(src, in_mean, in_cov);

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.5, NULL, NULL,
                                              &transform) != 0);

  double want = transform.target[0]*transform.target[1]*transform.target[2] / sqrt(det3(in_cov));

  CHECK(det3(transform.matrix) == doctest::Approx(want).epsilon(1e-9));
  CHECK(transform.rank == 3);

  imImageDestroy(src);
}

TEST_CASE("decorrelate: the transform does not depend on the eigenvectors the solver picked")
{
  /* This is what lets the golden images be generated by LAPACK and checked
     against the Jacobi in im_decorrelate.cpp. T is a function of the
     covariance alone -- permuting the source planes permutes the covariance,
     and the transform must permute with it exactly, not merely closely. */
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* swapped = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  double** a = (double**)src->data;
  double** b = (double**)swapped->data;
  for (int i = 0; i < src->count; i++)
  {
    b[0][i] = a[2][i];
    b[1][i] = a[0][i];
    b[2][i] = a[1][i];
  }

  imDecorrelationTransform t1, t2;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL, &t1) != 0);
  REQUIRE(imProcessDecorrelationCalcTransform(swapped, IM_DECORR_RGB, 1.0, NULL, NULL, &t2) != 0);

  /* row/column 0,1,2 of t1 must appear at 1,2,0 of t2 */
  static const int p[3] = { 1, 2, 0 };
  for (int i = 0; i < 3; i++)
  {
    for (int j = 0; j < 3; j++)
    {
      CAPTURE(i); CAPTURE(j);
      CHECK(t2.matrix[p[i]*3+p[j]] == doctest::Approx(t1.matrix[i*3+j]).epsilon(1e-10));
    }
  }

  imImageDestroy(src);
  imImageDestroy(swapped);
}


/* ====== Degenerate inputs, where a naive implementation divides by zero ====== */

TEST_CASE("decorrelate: an image of one colour is returned unchanged")
{
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* dst = create(W, H, IM_RGB, IM_DOUBLE);

  double** data = (double**)src->data;
  for (int i = 0; i < src->count; i++)
  {
    data[0][i] = 0.25;
    data[1][i] = 0.50;
    data[2][i] = 0.75;
  }

  REQUIRE(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 1.0) != 0);

  /* every eigenvalue is zero, so 1/sqrt(lambda) would be infinite; the guard
     has to leave those directions alone rather than amplify or erase them */
  double** out = (double**)dst->data;
  for (int i = 0; i < dst->count; i++)
  {
    CAPTURE(i);
    REQUIRE(out[0][i] == doctest::Approx(0.25).epsilon(1e-12));
    REQUIRE(out[1][i] == doctest::Approx(0.50).epsilon(1e-12));
    REQUIRE(out[2][i] == doctest::Approx(0.75).epsilon(1e-12));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a grey image is reported as one-dimensional and stays grey")
{
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* dst = create(W, H, IM_RGB, IM_DOUBLE);

  double** data = (double**)src->data;
  for (int i = 0; i < src->count; i++)
    data[0][i] = data[1][i] = data[2][i] = 0.2 + 0.6*((double)i/src->count);

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL,
                                              &transform) != 0);
  CHECK(transform.rank == 1);

  REQUIRE(imProcessDecorrelationApplyTransform(src, dst, &transform) != 0);

  double** out = (double**)dst->data;
  for (int i = 0; i < dst->count; i++)
  {
    CAPTURE(i);
    REQUIRE(out[1][i] == doctest::Approx(out[0][i]).epsilon(1e-10));
    REQUIRE(out[2][i] == doctest::Approx(out[0][i]).epsilon(1e-10));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


TEST_CASE("decorrelate: a plane that is a combination of the others is reported as rank two")
{
  /* The case Crabu, Pes and Rodriguez single out: not a constant plane, but
     one that is exactly a linear combination of the other two, so the
     covariance is singular while every plane still varies. The rank has to
     come back 2, and the direction with no spread must pass through rather
     than be projected away -- with a zero gain the third plane would collapse
     and the result would come back still correlated. */
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* dst = create(W, H, IM_RGB, IM_DOUBLE);

  double** data = (double**)src->data;
  Random r(23);
  for (int i = 0; i < src->count; i++)
  {
    double a = 0.5 + 0.08*r.normal();
    double b = 0.5 + 0.08*r.normal();

    data[0][i] = a;
    data[1][i] = b;
    data[2][i] = 0.4*a + 0.6*b;   /* exactly dependent */
  }

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL,
                                              &transform) != 0);
  CHECK(transform.rank == 2);

  REQUIRE(imProcessDecorrelationApplyTransform(src, dst, &transform) != 0);

  /* nothing became NaN on the way through the singular direction */
  double** out = (double**)dst->data;
  for (int i = 0; i < dst->count; i++)
  {
    CAPTURE(i);
    for (int k = 0; k < 3; k++)
      REQUIRE(out[k][i] == out[k][i]);
  }

  /* The colours lay in a plane and they still do. Not the SAME plane -- an
     affine map carries it somewhere else, so the original 0.4/0.6 relation
     does not survive and asserting it would be asserting the wrong thing --
     but the result is still confined to two dimensions, which is what "the
     stretch could not manufacture a third direction" means and what the
     rank-2 report above is claiming. Measured as a singular covariance,
     normalized by the band spreads so the comparison is scale-free. */
  double out_mean[3], out_cov[9];
  measure(dst, out_mean, out_cov);

  double norm = sqrt(out_cov[0]) * sqrt(out_cov[4]) * sqrt(out_cov[8]);
  REQUIRE(norm > 0);
  CHECK(fabs(det3(out_cov)) / norm < 1e-9);

  /* and all three planes still carry something: a zero gain on the singular
     direction would have collapsed one of them */
  for (int k = 0; k < 3; k++)
  {
    CAPTURE(k);
    CHECK(out_cov[k*3+k] > 1e-6);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ====== The compute/apply split ====== */

TEST_CASE("decorrelate: computing and applying separately matches the one-shot call")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* one = create(W, H, IM_RGB, IM_BYTE);
  imImage* two = create(W, H, IM_RGB, IM_BYTE);

  imbyte** data = (imbyte**)src->data;
  Random r(7);
  for (int i = 0; i < src->count; i++)
  {
    double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();
    double v[3] = { 128 + 18*u0,
                    118 + 18*(0.97*u0 + 0.2431*u1),
                    105 + 18*(0.94*u0 + 0.2465*u1 + 0.2323*u2) };
    for (int k = 0; k < 3; k++)
      data[k][i] = (imbyte)(v[k] < 0? 0: v[k] > 255? 255: v[k] + 0.5);
  }

  REQUIRE(imProcessDecorrelationStretch(src, one, IM_DECORR_RGB, 1.0) != 0);

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL,
                                              &transform) != 0);
  REQUIRE(imProcessDecorrelationApplyTransform(src, two, &transform) != 0);

  /* byte-identical, not close: they are the same arithmetic */
  CHECK(same_bytes(one, two));

  SUBCASE("and the saved transform applies to a different image")
  {
    /* the reason the split exists: derive from one frame, apply to another */
    imImage* other = create(W, H, IM_RGB, IM_BYTE);
    imImage* out = create(W, H, IM_RGB, IM_BYTE);
    imbyte** od = (imbyte**)other->data;

    for (int i = 0; i < other->count; i++)
      for (int k = 0; k < 3; k++)
        od[k][i] = (imbyte)((data[k][i] + 40) % 256);

    REQUIRE(imProcessDecorrelationApplyTransform(other, out, &transform) != 0);

    /* check a handful of pixels against the affine spelled out by hand */
    imbyte** ou = (imbyte**)out->data;
    for (int i = 0; i < other->count; i += 137)
    {
      CAPTURE(i);
      for (int k = 0; k < 3; k++)
      {
        double v = transform.offset[k];
        for (int j = 0; j < 3; j++)
          v += transform.matrix[k*3+j] * (double)od[j][i];

        v = v < 0? v - 0.5: v + 0.5;
        int want = v <= 0? 0: v >= 255? 255: (int)v;
        REQUIRE((int)ou[k][i] == want);
      }
    }

    imImageDestroy(other);
    imImageDestroy(out);
  }

  imImageDestroy(src);
  imImageDestroy(one);
  imImageDestroy(two);
}

TEST_CASE("decorrelate: processing in place matches processing into a separate image")
{
  /* The kernel has to load all three components before it stores any. Written
     a plane at a time it corrupts silently, and only when src == dst. */
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* copy = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* separate = create(W, H, IM_RGB, IM_DOUBLE);

  fill_correlated(src);
  for (int p = 0; p < 3; p++)
    memcpy(copy->data[p], src->data[p], src->plane_size);

  REQUIRE(imProcessDecorrelationStretch(src, separate, IM_DECORR_YUV, 1.5) != 0);
  REQUIRE(imProcessDecorrelationStretch(copy, copy, IM_DECORR_YUV, 1.5) != 0);

  CHECK(same_bytes(copy, separate));

  imImageDestroy(src);
  imImageDestroy(copy);
  imImageDestroy(separate);
}


/* ====== The mask ====== */

TEST_CASE("decorrelate: a mask restricts the statistics to the pixels it selects")
{
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* mask = create(W, H, IM_BINARY, IM_BYTE);
  fill_correlated(src);

  /* shift the right-hand half well away, so a transform that ignored the mask
     could not accidentally agree */
  double** data = (double**)src->data;
  imbyte* m = (imbyte*)mask->data[0];
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      int i = y*W + x;
      m[i] = (x < W/2)? 1: 0;
      if (x >= W/2)
      {
        data[0][i] = 0.9 - data[0][i];
        data[1][i] = 0.9 - data[1][i];
        data[2][i] = 0.9 - data[2][i];
      }
    }
  }

  imDecorrelationTransform masked;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, mask,
                                              &masked) != 0);

  /* the same statistics taken by hand over the selected pixels only */
  double mean[3] = { 0, 0, 0 };
  double n = 0;
  for (int i = 0; i < src->count; i++)
  {
    if (!m[i]) continue;
    for (int k = 0; k < 3; k++) mean[k] += data[k][i];
    n += 1.0;
  }
  for (int k = 0; k < 3; k++) mean[k] /= n;

  for (int k = 0; k < 3; k++)
  {
    CAPTURE(k);
    CHECK(masked.mean[k] == doctest::Approx(mean[k]).epsilon(1e-12));
  }

  SUBCASE("and differs from the transform over the whole image")
  {
    imDecorrelationTransform whole;
    REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL,
                                                &whole) != 0);

    double worst = 0;
    for (int i = 0; i < 9; i++)
      worst = fabs(masked.matrix[i] - whole.matrix[i]) > worst?
                fabs(masked.matrix[i] - whole.matrix[i]): worst;

    CHECK(worst > 1e-3);
  }

  imImageDestroy(src);
  imImageDestroy(mask);
}


/* ====== The colour spaces ====== */

TEST_CASE("decorrelate: no two colour spaces are the same operation")
{
  /* A preset table is easy to get wrong in a way nothing else notices: two
     rows the same, or a row that is secretly its own base space. Every pair
     must actually differ. */
  static const int spaces[] = {
    IM_DECORR_RGB, IM_DECORR_CRGB, IM_DECORR_YUV, IM_DECORR_LAB,
    IM_DECORR_YDS, IM_DECORR_YBR, IM_DECORR_YBK, IM_DECORR_YRE,
    IM_DECORR_YRD, IM_DECORR_YYE,
    IM_DECORR_LDS, IM_DECORR_LRE, IM_DECORR_LBK, IM_DECORR_LYE
  };
  const int count = (int)(sizeof(spaces)/sizeof(spaces[0]));

  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  imImage* out[14];
  for (int s = 0; s < count; s++)
  {
    out[s] = create(W, H, IM_RGB, IM_DOUBLE);
    CAPTURE(spaces[s]);
    REQUIRE(imProcessDecorrelationStretch(src, out[s], spaces[s], 1.0) != 0);
  }

  for (int a = 0; a < count; a++)
  {
    for (int b = a+1; b < count; b++)
    {
      double worst = 0;
      for (int p = 0; p < 3; p++)
      {
        double* x = (double*)out[a]->data[p];
        double* y = (double*)out[b]->data[p];
        for (int i = 0; i < src->count; i++)
          worst = fabs(x[i] - y[i]) > worst? fabs(x[i] - y[i]): worst;
      }

      CAPTURE(spaces[a]); CAPTURE(spaces[b]);
      CHECK(worst > 1e-6);
    }
  }

  for (int s = 0; s < count; s++)
    imImageDestroy(out[s]);
  imImageDestroy(src);
}

TEST_CASE("decorrelate: a custom matrix reproduces the space it was copied from")
{
  /* IM_DECORR_CUSTOM is the escape hatch for anyone holding coefficients this
     library does not ship. Handing it the ITU-R 601 matrix must give exactly
     what IM_DECORR_YUV gives. */
  static const double ycbcr[9] = {
     0.299,  0.587,  0.114,
    -0.169, -0.331,  0.500,
     0.500, -0.419, -0.081
  };

  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* a = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* b = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  REQUIRE(imProcessDecorrelationStretch(src, a, IM_DECORR_YUV, 1.0) != 0);

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_CUSTOM, 1.0, ycbcr, NULL,
                                              &transform) != 0);
  REQUIRE(imProcessDecorrelationApplyTransform(src, b, &transform) != 0);

  CHECK(same_bytes(a, b));

  imImageDestroy(src);
  imImageDestroy(a);
  imImageDestroy(b);
}

TEST_CASE("decorrelate: the stretch changes the image and does not flatten it")
{
  /* An operation that returned its input, or a constant, would satisfy a
     surprising number of the assertions above. */
  imImage* src = create(W, H, IM_RGB, IM_DOUBLE);
  imImage* dst = create(W, H, IM_RGB, IM_DOUBLE);
  fill_correlated(src);

  REQUIRE(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 1.0) != 0);

  double error = 0;
  REQUIRE(imCalcRMSError(src, dst, &error) != 0);
  CHECK(error > 0.01);

  double mean[3], cov[9];
  measure(dst, mean, cov);
  for (int k = 0; k < 3; k++)
  {
    CAPTURE(k);
    CHECK(cov[k*3+k] > 1e-6);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ====== Integer destinations ====== */

TEST_CASE("decorrelate: a byte result clips at the ends of the range instead of wrapping")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_BYTE);

  imbyte** data = (imbyte**)src->data;
  Random r(31);
  for (int i = 0; i < src->count; i++)
  {
    double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();
    double v[3] = { 128 + 12*u0,
                    124 + 12*(0.98*u0 + 0.1990*u1),
                    120 + 12*(0.96*u0 + 0.2020*u1 + 0.1937*u2) };
    for (int k = 0; k < 3; k++)
      data[k][i] = (imbyte)(v[k] < 0? 0: v[k] > 255? 255: v[k] + 0.5);
  }

  /* far past what the range can hold, so both ends are exercised */
  REQUIRE(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 12.0) != 0);

  int at_min = 0, at_max = 0;
  imbyte** out = (imbyte**)dst->data;
  for (int p = 0; p < 3; p++)
  {
    for (int i = 0; i < dst->count; i++)
    {
      if (out[p][i] == 0) at_min++;
      if (out[p][i] == 255) at_max++;
    }
  }

  /* wrapping would scatter values across the range rather than pile them at
     the ends, so both piles have to be there */
  CHECK(at_min > 0);
  CHECK(at_max > 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a byte result is rounded, not truncated")
{
  /* Truncating would bias every band half a level toward zero, which is small
     enough to hide inside a one-level golden tolerance and large enough to
     spoil the covariance the operation promises. Compare the stored bytes
     against the transform evaluated in double. */
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_BYTE);

  imbyte** data = (imbyte**)src->data;
  Random r(5);
  for (int i = 0; i < src->count; i++)
  {
    double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();
    double v[3] = { 140 + 20*u0,
                    120 + 20*(0.97*u0 + 0.2431*u1),
                    100 + 20*(0.94*u0 + 0.2465*u1 + 0.2323*u2) };
    for (int k = 0; k < 3; k++)
      data[k][i] = (imbyte)(v[k] < 0? 0: v[k] > 255? 255: v[k] + 0.5);
  }

  imDecorrelationTransform transform;
  REQUIRE(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, NULL,
                                              &transform) != 0);
  REQUIRE(imProcessDecorrelationApplyTransform(src, dst, &transform) != 0);

  double bias = 0;
  int n = 0;
  imbyte** out = (imbyte**)dst->data;
  for (int i = 0; i < dst->count; i++)
  {
    for (int k = 0; k < 3; k++)
    {
      double v = transform.offset[k];
      for (int j = 0; j < 3; j++)
        v += transform.matrix[k*3+j] * (double)data[j][i];

      if (v <= 0 || v >= 255) continue;   /* clipped samples say nothing */

      bias += (double)out[k][i] - v;
      n++;
    }
  }

  REQUIRE(n > 100);
  /* rounding averages out; truncation would sit near -0.5 */
  CHECK(fabs(bias/n) < 0.02);

  imImageDestroy(src);
  imImageDestroy(dst);
}


TEST_CASE("decorrelate: a real destination keeps what falls outside the range")
{
  /* The header points hard stretches at IM_FLOAT precisely because nothing is
     clipped there, so the excursion can be brought back into gamut afterwards
     instead of being thrown away. If the store path ever started clamping real
     types to 0-1 the advice would be wrong and silently so. */
  imImage* src = create(W, H, IM_RGB, IM_FLOAT);
  imImage* dst = create(W, H, IM_RGB, IM_FLOAT);

  float** data = (float**)src->data;
  Random r(17);
  for (int i = 0; i < src->count; i++)
  {
    double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();
    data[0][i] = (float)(0.50 + 0.02*u0);
    data[1][i] = (float)(0.48 + 0.02*(0.97*u0 + 0.2431*u1));
    data[2][i] = (float)(0.46 + 0.02*(0.94*u0 + 0.2465*u1 + 0.2323*u2));
  }

  REQUIRE(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 30.0) != 0);

  int below = 0, above = 0;
  float** out = (float**)dst->data;
  for (int p = 0; p < 3; p++)
  {
    for (int i = 0; i < dst->count; i++)
    {
      if (out[p][i] < 0.0f) below++;
      if (out[p][i] > 1.0f) above++;
    }
  }

  CHECK(below > 0);
  CHECK(above > 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}


TEST_CASE("decorrelate: every data type survives every colour space")
{
  /* IM_INT through an L*a*b* space used to come back as a single wrong value
     in every pixel: that path normalizes with imColorQuantize, and handing it
     the full int range made it compute range = 2^32 and overflow imRound,
     which returns an int. Nothing covered IM_INT, so nothing said so.

     The assertion is deliberately weak -- the result varies, and stays inside
     the range -- because the point is to walk every type/space pair at all.
     The exact behaviour is pinned by the IM_DOUBLE cases above. */
  static const int types[] = { IM_BYTE, IM_SHORT, IM_USHORT, IM_INT, IM_FLOAT, IM_DOUBLE };
  static const int spaces[] = {
    IM_DECORR_RGB, IM_DECORR_CRGB, IM_DECORR_YUV, IM_DECORR_LAB,
    IM_DECORR_YDS, IM_DECORR_YBR, IM_DECORR_YBK, IM_DECORR_YRE,
    IM_DECORR_YRD, IM_DECORR_YYE,
    IM_DECORR_LDS, IM_DECORR_LRE, IM_DECORR_LBK, IM_DECORR_LYE
  };

  for (int t = 0; t < 6; t++)
  {
    CAPTURE(types[t]);

    /* mid-range, so the cloud is representable in every type; the real types
       want 0-1, which the L*a*b* spaces require */
    double centre[3], spread;
    switch (types[t])
    {
    case IM_BYTE:   centre[0] = 128;   centre[1] = 118;   centre[2] = 105;   spread = 18;    break;
    case IM_SHORT:  centre[0] = 8000;  centre[1] = 7400;  centre[2] = 6600;  spread = 1100;  break;
    case IM_USHORT: centre[0] = 32000; centre[1] = 29500; centre[2] = 26200; spread = 4500;  break;
    case IM_INT:    centre[0] = 4000000; centre[1] = 3700000; centre[2] = 3300000; spread = 560000; break;
    default:        centre[0] = 0.50;  centre[1] = 0.46;  centre[2] = 0.41;  spread = 0.07;  break;
    }

    for (int p = 0; p < 14; p++)
    {
      CAPTURE(spaces[p]);

      imImage* src = create(W, H, IM_RGB, types[t]);
      imImage* dst = create(W, H, IM_RGB, types[t]);
      Random r(1000 + 7*t + p);

      for (int i = 0; i < src->count; i++)
      {
        double u0 = r.normal(), u1 = r.normal(), u2 = r.normal();
        double v[3] = { centre[0] + spread*u0,
                        centre[1] + spread*(0.97*u0 + 0.2431*u1),
                        centre[2] + spread*(0.94*u0 + 0.2465*u1 + 0.2323*u2) };

        for (int k = 0; k < 3; k++)
        {
          switch (types[t])
          {
          case IM_BYTE:   ((imbyte**)src->data)[k][i]   = (imbyte)(v[k] < 0? 0: v[k] > 255? 255: v[k]); break;
          case IM_SHORT:  ((short**)src->data)[k][i]    = (short)v[k];    break;
          case IM_USHORT: ((imushort**)src->data)[k][i] = (imushort)v[k]; break;
          case IM_INT:    ((int**)src->data)[k][i]      = (int)v[k];      break;
          case IM_FLOAT:  ((float**)src->data)[k][i]    = (float)v[k];    break;
          default:        ((double**)src->data)[k][i]   = v[k];           break;
          }
        }
      }

      REQUIRE(imProcessDecorrelationStretch(src, dst, spaces[p], 1.0) != 0);

      /* the result has to actually vary: the overflow produced a constant */
      int distinct = 0;
      for (int i = 1; i < dst->count; i++)
      {
        switch (types[t])
        {
        case IM_BYTE:   if (((imbyte**)dst->data)[0][i]   != ((imbyte**)dst->data)[0][0])   distinct++; break;
        case IM_SHORT:  if (((short**)dst->data)[0][i]    != ((short**)dst->data)[0][0])    distinct++; break;
        case IM_USHORT: if (((imushort**)dst->data)[0][i] != ((imushort**)dst->data)[0][0]) distinct++; break;
        case IM_INT:    if (((int**)dst->data)[0][i]      != ((int**)dst->data)[0][0])      distinct++; break;
        case IM_FLOAT:  if (((float**)dst->data)[0][i]    != ((float**)dst->data)[0][0])    distinct++; break;
        default:        if (((double**)dst->data)[0][i]   != ((double**)dst->data)[0][0])   distinct++; break;
        }
      }
      REQUIRE(distinct > dst->count/10);

      imImageDestroy(src);
      imImageDestroy(dst);
    }
  }
}


/* ====== Preconditions ======
 *
 * Each of these violates a rule the header states, so each trips the assert
 * that sits in front of the runtime guard. doctest cannot catch an abort, so
 * they only run where the asserts are compiled out -- see the note in
 * test_datatype.cpp. The asserts job in ci-linux.yml builds Debug so the
 * asserts themselves are not dead code. */
#ifdef NDEBUG

TEST_CASE("decorrelate: a destination of a different size is refused")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W/2, H, IM_RGB, IM_BYTE);

  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 1.0) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a destination of a different data type is refused")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_FLOAT);

  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 1.0) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: an image that is not RGB is refused")
{
  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  imImage* dst = create(W, H, IM_GRAY, IM_BYTE);

  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 1.0) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a custom space with no matrix is refused")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imDecorrelationTransform transform;

  CHECK(imProcessDecorrelationCalcTransform(src, IM_DECORR_CUSTOM, 1.0, NULL, NULL,
                                            &transform) == 0);

  imImageDestroy(src);
}

TEST_CASE("decorrelate: a scale of zero or less is refused")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_BYTE);

  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, 0.0) == 0);
  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_RGB, -1.0) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a custom matrix that cannot be inverted is refused")
{
  /* Rank 2: the third row is the sum of the first two. Reporting success with
     a silently substituted identity would give the caller a no-op it could
     not tell apart from a real transform. */
  static const double singular[9] = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    1.0, 1.0, 0.0
  };

  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imDecorrelationTransform transform;

  CHECK(imProcessDecorrelationCalcTransform(src, IM_DECORR_CUSTOM, 1.0, singular, NULL,
                                            &transform) == 0);

  imImageDestroy(src);
}

TEST_CASE("decorrelate: the one-shot call refuses the custom space")
{
  /* There is nowhere to pass the matrix through this entry point. */
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* dst = create(W, H, IM_RGB, IM_BYTE);

  CHECK(imProcessDecorrelationStretch(src, dst, IM_DECORR_CUSTOM, 1.0) == 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("decorrelate: a mask of the wrong size is refused")
{
  imImage* src = create(W, H, IM_RGB, IM_BYTE);
  imImage* mask = create(W/2, H, IM_BINARY, IM_BYTE);
  imDecorrelationTransform transform;

  CHECK(imProcessDecorrelationCalcTransform(src, IM_DECORR_RGB, 1.0, NULL, mask,
                                            &transform) == 0);

  imImageDestroy(src);
  imImageDestroy(mask);
}

#endif
