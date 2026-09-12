/* Covers src/process/im_deconvolve.cpp and src/process/im_denoise.cpp --
 * Richardson-Lucy deconvolution and the three edge-preserving filters.
 *
 * The hard part of testing a restoration is that "looks sharper" is not an
 * assertion. Everything here is either an exact identity or a comparison
 * against a second operation the library already has, because a filter that
 * is self-consistently wrong satisfies every structural check: a bilateral
 * filter that ignored its range term would pass "output differs from input",
 * "output is bounded by the input's range" and "a constant image is
 * unchanged", and would be an ordinary Gaussian blur.
 *
 * The identities that carry the deconvolution cases:
 *
 *   - A delta PSF makes the iteration the identity map, to the last bit. That
 *     pins kernel centring, the reflection in the correction step, and the
 *     normalisation all at once -- get any of them wrong and a delta PSF
 *     shifts, scales or smears the image.
 *   - The PSF is normalised internally, so scaling it changes nothing.
 *   - Flux is preserved, because the update is a ratio and the PSF sums to 1.
 *
 * And for the filters:
 *
 *   - Anisotropic diffusion conserves total intensity EXACTLY. Each interior
 *     edge contributes c*(b-a) to one pixel and c*(a-b) to the other, and the
 *     replicated boundary contributes nothing because a border pixel's
 *     outside neighbour is itself. A scheme that got the boundary wrong, or
 *     that used a different conductance for the two directions of one edge,
 *     would leak.
 *   - An edge-preserving filter must beat a Gaussian of matched width at
 *     keeping a step step-like while doing comparable work on noise. Both
 *     halves are asserted, because either alone is trivially satisfiable --
 *     by doing nothing, and by blurring everything.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_process.h>

#include <math.h>
#include <string.h>

namespace
{
  /* Deliberately not square, so an operation that transposed its indices
     could not pass. */
  const int W = 48;
  const int H = 34;

  imImage* create(int width, int height, int color_space, int data_type)
  {
    imImage* image = imImageCreate(width, height, color_space, data_type);
    REQUIRE(image != NULL);
    return image;
  }

  /* A deterministic generator: the suite must not depend on the platform's
     rand(), and a case that fails only on one libc is worse than no case. */
  struct Random
  {
    unsigned int state;
    Random(unsigned int seed) : state(seed) {}

    double next()
    {
      state = state * 1103515245u + 12345u;
      return (double)((state >> 16) & 0x7FFF) / 32767.0;
    }

    /* Box-Muller, so the noise really is Gaussian rather than merely
       zero-mean; the filters are tuned against a standard deviation. */
    double gaussian()
    {
      double u1 = next(), u2 = next();
      if (u1 < 1e-12) u1 = 1e-12;
      return sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
    }
  };

  double* plane(imImage* image) { return (double*)image->data[0]; }

  double total(imImage* image)
  {
    double sum = 0.0;
    for (int i = 0; i < image->count; i++)
      sum += plane(image)[i];
    return sum;
  }

  double rms_difference(imImage* a, imImage* b)
  {
    double sum = 0.0;
    for (int i = 0; i < a->count; i++)
    {
      double d = plane(a)[i] - plane(b)[i];
      sum += d * d;
    }
    return sqrt(sum / (double)a->count);
  }

  /* A PSF with a single 1 at the centre: convolving with it is the identity. */
  imImage* delta_psf(int size)
  {
    imImage* psf = create(size, size, IM_GRAY, IM_DOUBLE);
    memset(psf->data[0], 0, (size_t)psf->count * sizeof(double));
    ((double*)psf->data[0])[(size / 2) * size + (size / 2)] = 1.0;
    return psf;
  }

  imImage* gaussian_psf(int size, double stddev)
  {
    imImage* psf = create(size, size, IM_GRAY, IM_DOUBLE);
    int half = size / 2;
    double* data = (double*)psf->data[0];

    for (int y = 0; y < size; y++)
    {
      for (int x = 0; x < size; x++)
      {
        double dx = x - half, dy = y - half;
        data[y * size + x] = exp(-(dx*dx + dy*dy) / (2.0 * stddev * stddev));
      }
    }
    return psf;
  }

  /* Something with structure at several scales, so a filter cannot do well by
     preserving only one of them: a bright disc, a step, and a thin bar. */
  void fill_scene(imImage* image)
  {
    double* data = plane(image);

    for (int y = 0; y < image->height; y++)
    {
      for (int x = 0; x < image->width; x++)
      {
        double value = (x < image->width / 2) ? 40.0 : 120.0;

        double dx = x - 14.0, dy = y - 12.0;
        if (dx*dx + dy*dy < 36.0)
          value = 200.0;

        if (y >= 26 && y <= 27 && x >= 8 && x <= 40)
          value = 230.0;

        data[y * image->width + x] = value;
      }
    }
  }
}

TEST_CASE("RichardsonLucy: a delta PSF is the identity")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* psf = delta_psf(5);

  fill_scene(src);

  REQUIRE(imProcessRichardsonLucy(src, psf, dst, 20) != 0);

  /* Exact, not approximate. Every step of the iteration multiplies by a ratio
     that is identically 1 when the PSF is a delta, so any drift here is a
     real defect -- a mis-centred kernel, or a reflection applied to one of
     the two convolutions and not the other. */
  for (int i = 0; i < src->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(plane(src)[i]).epsilon(1e-12));

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(psf);
}

TEST_CASE("RichardsonLucy: an off-centre delta shifts the image, which is what pins the centring")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* psf = create(3, 3, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  /* A PSF whose only non-zero sample is one to the right of centre.
     The forward operator, like every other "convolution" in this library, is
     a correlation: blurred[x] = sum over k of psf[k] * src[x+k], so a PSF
     weighted at +1 makes the blurred image READ from one to the right, which
     moves the picture LEFT. Deconvolving must therefore move it RIGHT:
     dst[x] == src[x-1].

     Working that through by hand for one iteration is the point of the case.
     estimate starts at g, blurred[x] = g[x+1], ratio[x] = g[x]/g[x+1], and
     the correction step applies the ADJOINT -- the kernel reflected -- giving
     correction[x] = g[x-1]/g[x]. The product is g[x-1]. If the correction
     step used the same kernel instead of its reflection the two shifts would
     cancel and dst would equal src, which is precisely what a symmetric PSF
     can never reveal, and why the delta case above is not enough on its
     own. */
  memset(psf->data[0], 0, (size_t)psf->count * sizeof(double));
  ((double*)psf->data[0])[1 * 3 + 2] = 1.0;

  REQUIRE(imProcessRichardsonLucy(src, psf, dst, 1) != 0);

  for (int y = 0; y < H; y++)
  {
    for (int x = 1; x < W; x++)
      CHECK(plane(dst)[y * W + x] == doctest::Approx(plane(src)[y * W + (x - 1)]).epsilon(1e-9));
  }

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(psf);
}

TEST_CASE("RichardsonLucy: the PSF is normalised, so its scale does not matter")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* one = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* thousand = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  imImage* psf = gaussian_psf(7, 1.2);
  REQUIRE(imProcessRichardsonLucy(src, psf, one, 5) != 0);

  for (int i = 0; i < psf->count; i++)
    ((double*)psf->data[0])[i] *= 1000.0;
  REQUIRE(imProcessRichardsonLucy(src, psf, thousand, 5) != 0);

  for (int i = 0; i < src->count; i++)
    CHECK(plane(thousand)[i] == doctest::Approx(plane(one)[i]).epsilon(1e-9));

  imImageDestroy(src);
  imImageDestroy(one);
  imImageDestroy(thousand);
  imImageDestroy(psf);
}

TEST_CASE("RichardsonLucy: recovers a known blur, and preserves flux doing it")
{
  imImage* truth = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* blurred = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* restored = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(truth);

  imImage* psf = gaussian_psf(9, 1.5);
  REQUIRE(imProcessGaussianConvolve(truth, blurred, 1.5) != 0);

  REQUIRE(imProcessRichardsonLucy(blurred, psf, restored, 40) != 0);

  /* The assertion that means something: the restoration is closer to the
     truth than the blurred image was. An operation that merely sharpened --
     an unsharp mask, say -- can easily move AWAY from the truth while looking
     better, so this is measured against the original rather than eyeballed. */
  double before = rms_difference(blurred, truth);
  double after = rms_difference(restored, truth);

  CHECK(after < before);
  CHECK(after < before * 0.7);

  /* And the iteration is doing the work, rather than the first step doing it
     all: 40 iterations must beat 5. Stated as a comparison between two runs
     of this operation, because the absolute figure depends on the scene --
     a step edge is the slowest thing for Richardson-Lucy to converge on, and
     this scene is mostly step edges. */
  imImage* barely = create(W, H, IM_GRAY, IM_DOUBLE);
  REQUIRE(imProcessRichardsonLucy(blurred, psf, barely, 5) != 0);
  CHECK(after < rms_difference(barely, truth));
  imImageDestroy(barely);

  /* Flux is preserved because the update is a ratio and the PSF sums to one.
     The tolerance is loose because the borders are replicated rather than
     periodic, so a little intensity does cross the frame edge. */
  CHECK(total(restored) == doctest::Approx(total(blurred)).epsilon(0.02));

  imImageDestroy(truth);
  imImageDestroy(blurred);
  imImageDestroy(restored);
  imImageDestroy(psf);
}

TEST_CASE("RichardsonLucy: zero iterations copies, and in-place works")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* psf = gaussian_psf(5, 1.0);

  fill_scene(src);

  REQUIRE(imProcessRichardsonLucy(src, psf, dst, 0) != 0);
  for (int i = 0; i < src->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(plane(src)[i]));

  /* In place: the source is read into its own buffers before anything is
     written back, so src == dst is legal. */
  imImage* both = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* separate = create(W, H, IM_GRAY, IM_DOUBLE);
  fill_scene(both);
  fill_scene(separate);

  imImage* reference = create(W, H, IM_GRAY, IM_DOUBLE);
  REQUIRE(imProcessRichardsonLucy(separate, psf, reference, 6) != 0);
  REQUIRE(imProcessRichardsonLucy(both, psf, both, 6) != 0);

  for (int i = 0; i < both->count; i++)
    CHECK(plane(both)[i] == doctest::Approx(plane(reference)[i]).epsilon(1e-12));

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(both);
  imImageDestroy(separate);
  imImageDestroy(reference);
  imImageDestroy(psf);
}

TEST_CASE("RichardsonLucy: negative input is floored rather than propagated")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* psf = delta_psf(3);

  fill_scene(src);
  plane(src)[10 * W + 10] = -500.0;

  REQUIRE(imProcessRichardsonLucy(src, psf, dst, 5) != 0);

  /* A negative sample makes the Poisson likelihood meaningless, and the
     update is a ratio, so one negative would otherwise spread a sign flip
     across the frame. It is treated as zero and nothing else moves. */
  CHECK(plane(dst)[10 * W + 10] == doctest::Approx(0.0));
  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] >= 0.0);

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(psf);
}

/* The precondition guards return rather than compute, but the matching
   assert() fires first in a build that has them, and doctest cannot catch an
   abort. See the note in test_datatype.cpp. */
#ifdef NDEBUG
TEST_CASE("RichardsonLucy: rejects an even-sided PSF and a mismatched destination")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  imImage* even = create(4, 5, IM_GRAY, IM_DOUBLE);
  memset(even->data[0], 0, (size_t)even->count * sizeof(double));
  ((double*)even->data[0])[0] = 1.0;
  CHECK(imProcessRichardsonLucy(src, even, dst, 3) == 0);
  imImageDestroy(even);

  imImage* psf = delta_psf(3);

  imImage* small = create(W / 2, H, IM_GRAY, IM_DOUBLE);
  CHECK(imProcessRichardsonLucy(src, psf, small, 3) == 0);
  imImageDestroy(small);

  imImage* wrong_type = create(W, H, IM_GRAY, IM_FLOAT);
  CHECK(imProcessRichardsonLucy(src, psf, wrong_type, 3) == 0);
  imImageDestroy(wrong_type);

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(psf);
}
#endif

TEST_CASE("BilateralFilter: a constant image is unchanged")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 77.0;

  REQUIRE(imProcessBilateralFilter(src, dst, 2.0, 10.0) != 0);

  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(77.0));

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("BilateralFilter: keeps a step that a Gaussian of the same width destroys")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* bilateral = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* gaussian = create(W, H, IM_GRAY, IM_DOUBLE);

  /* A clean step at the middle column, 0 on the left and 100 on the right. */
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      plane(src)[y * W + x] = (x < W / 2) ? 0.0 : 100.0;

  REQUIRE(imProcessBilateralFilter(src, bilateral, 2.0, 8.0) != 0);
  REQUIRE(imProcessGaussianConvolve(src, gaussian, 2.0) != 0);

  /* Across the step, at a row well away from the frame edges. The range term
     is 8 against a jump of 100, so neighbours on the far side weigh
     exp(-78) -- nothing. The Gaussian has no such term and lands near the
     midpoint. */
  int y = H / 2;
  int left = W / 2 - 1, right = W / 2;

  CHECK(plane(bilateral)[y * W + left] == doctest::Approx(0.0).epsilon(0.02));
  CHECK(plane(bilateral)[y * W + right] == doctest::Approx(100.0).epsilon(0.02));

  CHECK(plane(gaussian)[y * W + left] > 10.0);
  CHECK(plane(gaussian)[y * W + right] < 90.0);

  imImageDestroy(src);
  imImageDestroy(bilateral);
  imImageDestroy(gaussian);
}

TEST_CASE("BilateralFilter: a large range stddev degenerates to a Gaussian")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* bilateral = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* gaussian = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  /* With the range term wide enough to be flat over the whole intensity
     range, every neighbour weighs what the spatial term alone says, which is
     an ordinary Gaussian. This is the case that would still pass if the
     range term were dropped entirely -- which is exactly why the step case
     above exists as well. */
  REQUIRE(imProcessBilateralFilter(src, bilateral, 1.5, 1.0e6) != 0);
  REQUIRE(imProcessGaussianConvolve(src, gaussian, 1.5) != 0);

  CHECK(rms_difference(bilateral, gaussian) < 2.0);

  imImageDestroy(src);
  imImageDestroy(bilateral);
  imImageDestroy(gaussian);
}

TEST_CASE("BilateralFilter: reduces noise in a flat region")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  Random random(20260912u);
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 100.0 + 12.0 * random.gaussian();

  REQUIRE(imProcessBilateralFilter(src, dst, 2.0, 20.0) != 0);

  double before = 0.0, after = 0.0;
  for (int i = 0; i < src->count; i++)
  {
    before += (plane(src)[i] - 100.0) * (plane(src)[i] - 100.0);
    after += (plane(dst)[i] - 100.0) * (plane(dst)[i] - 100.0);
  }

  CHECK(after < before * 0.5);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("AnisotropicDiffusion: conserves total intensity exactly")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);
  double before = total(src);

  REQUIRE(imProcessAnisotropicDiffusion(src, dst, 0.2, 20.0, 15, IM_DIFFUSION_EXPONENTIAL) != 0);

  /* Exact to rounding, and this is the case that catches a boundary bug. Each
     interior edge moves the same quantity out of one pixel and into the
     other; a border pixel's outside neighbour is itself, so nothing leaves
     the frame. Get the clamping wrong -- wrap instead of replicate, or index
     past the end -- and the total moves. */
  CHECK(total(dst) == doctest::Approx(before).epsilon(1e-10));

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("AnisotropicDiffusion: zero iterations copies, a constant image is fixed")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  REQUIRE(imProcessAnisotropicDiffusion(src, dst, 0.2, 20.0, 0, IM_DIFFUSION_EXPONENTIAL) != 0);
  for (int i = 0; i < src->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(plane(src)[i]));

  /* Zero iterations returning the source is not a triviality here: the
     implementation swaps two buffers per iteration and hands back whichever
     holds the last one, so the even/odd bookkeeping has a base case. */
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 31.0;

  REQUIRE(imProcessAnisotropicDiffusion(src, dst, 0.25, 5.0, 9, IM_DIFFUSION_QUADRATIC) != 0);
  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(31.0));

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("AnisotropicDiffusion: an odd iteration count returns the right buffer")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* odd = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* even = create(W, H, IM_GRAY, IM_DOUBLE);

  /* Noise, NOT fill_scene. The scene is piecewise constant, and Perona-Malik
     is a diffusion: inside a flat patch every gradient is zero and nothing
     moves, while across a step of 80 with kappa at 15 the conductance is
     exp(-28), which is also nothing. A piecewise-constant image is very
     nearly a fixed point of this filter, so running it for seven iterations
     and eight produced two images that differed by 8e-13 and the case could
     not see a buffer bug through the noise floor. It needs gradients of the
     same order as kappa to have anything to do. */
  Random random(4242u);
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 100.0 + 20.0 * random.gaussian();

  /* Seven iterations and eight must differ from each other and BOTH from the
     source. Returning the wrong buffer of the pair is a real hazard in a
     ping-pong loop, and its signature is that odd counts come back one
     iteration stale -- so 7 would equal 6, not 7. Comparing 7 against 8
     catches a stale result; comparing both against the source catches a loop
     that never ran. */
  REQUIRE(imProcessAnisotropicDiffusion(src, odd, 0.2, 15.0, 7, IM_DIFFUSION_EXPONENTIAL) != 0);
  REQUIRE(imProcessAnisotropicDiffusion(src, even, 0.2, 15.0, 8, IM_DIFFUSION_EXPONENTIAL) != 0);

  CHECK(rms_difference(odd, even) > 1e-9);
  CHECK(rms_difference(odd, src) > 1e-9);
  CHECK(rms_difference(even, src) > 1e-9);

  imImageDestroy(src);
  imImageDestroy(odd);
  imImageDestroy(even);
}

TEST_CASE("AnisotropicDiffusion: keeps a step while smoothing noise")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  Random random(7u);
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      double base = (x < W / 2) ? 20.0 : 180.0;
      plane(src)[y * W + x] = base + 6.0 * random.gaussian();
    }
  }

  /* kappa of 25 sits above the noise (6) and well below the step (160), which
     is the whole art of setting it: gradients from noise conduct, the step
     does not. */
  REQUIRE(imProcessAnisotropicDiffusion(src, dst, 0.2, 25.0, 20, IM_DIFFUSION_EXPONENTIAL) != 0);

  double noise_before = 0.0, noise_after = 0.0;
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      double base = (x < W / 2) ? 20.0 : 180.0;
      noise_before += (plane(src)[y * W + x] - base) * (plane(src)[y * W + x] - base);
      noise_after += (plane(dst)[y * W + x] - base) * (plane(dst)[y * W + x] - base);
    }
  }

  CHECK(noise_after < noise_before * 0.3);

  /* And the step itself is still a step: the two columns either side of it
     are still most of 160 apart. */
  int y = H / 2;
  double jump = plane(dst)[y * W + W / 2] - plane(dst)[y * W + W / 2 - 1];
  CHECK(jump > 140.0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("AnisotropicDiffusion: all three conductance functions run and differ")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* a = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* b = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* c = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  REQUIRE(imProcessAnisotropicDiffusion(src, a, 0.2, 30.0, 10, IM_DIFFUSION_EXPONENTIAL) != 0);
  REQUIRE(imProcessAnisotropicDiffusion(src, b, 0.2, 30.0, 10, IM_DIFFUSION_QUADRATIC) != 0);
  REQUIRE(imProcessAnisotropicDiffusion(src, c, 0.2, 30.0, 10, IM_DIFFUSION_TUKEY) != 0);

  CHECK(rms_difference(a, b) > 1e-6);
  CHECK(rms_difference(a, c) > 1e-6);
  CHECK(rms_difference(b, c) > 1e-6);

  /* All three conserve, whatever the conductance. */
  CHECK(total(a) == doctest::Approx(total(src)).epsilon(1e-10));
  CHECK(total(b) == doctest::Approx(total(src)).epsilon(1e-10));
  CHECK(total(c) == doctest::Approx(total(src)).epsilon(1e-10));

  imImageDestroy(src);
  imImageDestroy(a);
  imImageDestroy(b);
  imImageDestroy(c);
}

#ifdef NDEBUG
TEST_CASE("AnisotropicDiffusion: refuses a time step past the stability limit")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  fill_scene(src);

  /* Above a quarter the explicit scheme diverges into a checkerboard, which
     is a plausible-looking image rather than an error, so it is refused. */
  CHECK(imProcessAnisotropicDiffusion(src, dst, 0.26, 20.0, 5, IM_DIFFUSION_EXPONENTIAL) == 0);
  CHECK(imProcessAnisotropicDiffusion(src, dst, 0.0, 20.0, 5, IM_DIFFUSION_EXPONENTIAL) == 0);
  CHECK(imProcessAnisotropicDiffusion(src, dst, 0.25, 20.0, 5, IM_DIFFUSION_EXPONENTIAL) != 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}
#endif

TEST_CASE("NonLocalMeans: a constant image is unchanged, and noise is reduced")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 64.0;

  REQUIRE(imProcessNonLocalMeans(src, dst, 3, 1, 10.0) != 0);
  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(64.0));

  Random random(99u);
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 100.0 + 10.0 * random.gaussian();

  REQUIRE(imProcessNonLocalMeans(src, dst, 4, 1, 12.0) != 0);

  double before = 0.0, after = 0.0;
  for (int i = 0; i < src->count; i++)
  {
    before += (plane(src)[i] - 100.0) * (plane(src)[i] - 100.0);
    after += (plane(dst)[i] - 100.0) * (plane(dst)[i] - 100.0);
  }

  CHECK(after < before * 0.5);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("NonLocalMeans: the centre pixel does not dominate its own average")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(W, H, IM_GRAY, IM_DOUBLE);

  /* A flat field with one spike. The spike's own patch matches nothing, so
     every other weight is tiny; if the centre kept its natural weight of 1 --
     larger than every other, since its distance from itself is zero -- the
     spike would survive untouched and the filter would be a no-op exactly
     where it is needed. Giving the centre the largest weight any other pixel
     earned is what pulls it down. */
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 50.0;
  plane(src)[(H / 2) * W + (W / 2)] = 250.0;

  REQUIRE(imProcessNonLocalMeans(src, dst, 4, 1, 15.0) != 0);

  CHECK(plane(dst)[(H / 2) * W + (W / 2)] < 150.0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("NonLocalMeans: keeps an edge sharper than a mean filter of the same reach")
{
  imImage* src = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* nlm = create(W, H, IM_GRAY, IM_DOUBLE);
  imImage* mean = create(W, H, IM_GRAY, IM_DOUBLE);

  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      plane(src)[y * W + x] = (x < W / 2) ? 30.0 : 170.0;

  REQUIRE(imProcessNonLocalMeans(src, nlm, 3, 1, 10.0) != 0);
  REQUIRE(imProcessMeanConvolve(src, mean, 7) != 0);

  int y = H / 2;
  double nlm_jump = plane(nlm)[y * W + W / 2] - plane(nlm)[y * W + W / 2 - 1];
  double mean_jump = plane(mean)[y * W + W / 2] - plane(mean)[y * W + W / 2 - 1];

  CHECK(nlm_jump > mean_jump);
  CHECK(nlm_jump > 130.0);

  imImageDestroy(src);
  imImageDestroy(nlm);
  imImageDestroy(mean);
}

TEST_CASE("the filters run over every supported data type and both image depths")
{
  const int types[] = { IM_BYTE, IM_SHORT, IM_USHORT, IM_INT, IM_FLOAT, IM_DOUBLE };

  for (int t = 0; t < 6; t++)
  {
    imImage* src = create(20, 16, IM_GRAY, types[t]);
    imImage* dst = create(20, 16, IM_GRAY, types[t]);

    /* A ramp, written through the widest type the switch will see. */
    for (int i = 0; i < src->count; i++)
    {
      double v = (double)(i % 200);
      switch (types[t])
      {
      case IM_BYTE:   ((imbyte*)src->data[0])[i] = (imbyte)v;     break;
      case IM_SHORT:  ((short*)src->data[0])[i] = (short)v;       break;
      case IM_USHORT: ((imushort*)src->data[0])[i] = (imushort)v; break;
      case IM_INT:    ((int*)src->data[0])[i] = (int)v;           break;
      case IM_FLOAT:  ((float*)src->data[0])[i] = (float)v;       break;
      case IM_DOUBLE: ((double*)src->data[0])[i] = v;             break;
      }
    }

    CHECK(imProcessBilateralFilter(src, dst, 1.5, 20.0) != 0);
    CHECK(imProcessAnisotropicDiffusion(src, dst, 0.2, 20.0, 3, IM_DIFFUSION_EXPONENTIAL) != 0);
    CHECK(imProcessNonLocalMeans(src, dst, 2, 1, 20.0) != 0);

    imImage* psf = create(3, 3, IM_GRAY, IM_DOUBLE);
    memset(psf->data[0], 0, (size_t)psf->count * sizeof(double));
    ((double*)psf->data[0])[4] = 1.0;
    CHECK(imProcessRichardsonLucy(src, psf, dst, 2) != 0);
    imImageDestroy(psf);

    imImageDestroy(src);
    imImageDestroy(dst);
  }

  /* An RGB image: every one of these loops over depth, and a filter that
     read data[0] for every plane would pass all the gray cases above. */
  imImage* rgb = create(20, 16, IM_RGB, IM_BYTE);
  imImage* out = create(20, 16, IM_RGB, IM_BYTE);

  for (int p = 0; p < 3; p++)
    for (int i = 0; i < rgb->count; i++)
      ((imbyte*)rgb->data[p])[i] = (imbyte)((i * (p + 1)) % 256);

  CHECK(imProcessBilateralFilter(rgb, out, 1.5, 20.0) != 0);

  int differ = 0;
  for (int i = 0; i < out->count; i++)
  {
    if (((imbyte*)out->data[1])[i] != ((imbyte*)out->data[0])[i])
      differ++;
  }
  CHECK(differ > 0);

  imImageDestroy(rgb);
  imImageDestroy(out);
}

TEST_CASE("MeanConvolve on an IM_DOUBLE image reads its kernel at the right width")
{
  /* Not a test of anything added here. It pins a bug in im_convolve.cpp that
     the non-local-means comparison above tripped over: DoConvolveStep's
     IM_DOUBLE case read the kernel as double* whatever type it actually was,
     while every other case in the same switch branches on it.
     imProcessMeanConvolve always builds an IM_INT kernel, so every call of it
     on an IM_DOUBLE image read 2x its kernel allocation -- a 7x7 kernel is
     196 bytes and was being read as 392.

     The check is a value, not a crash. A constant image convolved with a
     normalised averaging kernel is that constant, and the overrun made the
     kernel total garbage, so the result was garbage too -- which is worth an
     assertion of its own, because the out-of-bounds read is only visible
     under a sanitizer while the wrong answer ships. */
  imImage* src = create(32, 24, IM_GRAY, IM_DOUBLE);
  imImage* dst = create(32, 24, IM_GRAY, IM_DOUBLE);

  for (int i = 0; i < src->count; i++)
    plane(src)[i] = 50.0;

  REQUIRE(imProcessMeanConvolve(src, dst, 7) != 0);

  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx(50.0));

  /* And it agrees with the same operation on the same data in IM_FLOAT,
     which took the branch that was already correct. */
  imImage* fsrc = create(32, 24, IM_GRAY, IM_FLOAT);
  imImage* fdst = create(32, 24, IM_GRAY, IM_FLOAT);

  for (int i = 0; i < fsrc->count; i++)
    ((float*)fsrc->data[0])[i] = (float)((i * 7) % 101);
  for (int i = 0; i < src->count; i++)
    plane(src)[i] = (double)((i * 7) % 101);

  REQUIRE(imProcessMeanConvolve(fsrc, fdst, 5) != 0);
  REQUIRE(imProcessMeanConvolve(src, dst, 5) != 0);

  for (int i = 0; i < dst->count; i++)
    CHECK(plane(dst)[i] == doctest::Approx((double)((float*)fdst->data[0])[i]).epsilon(1e-5));

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(fsrc);
  imImageDestroy(fdst);
}

TEST_CASE("rounding on store is to nearest, not truncation toward zero")
{
  /* An IM_BYTE image of a single value run through a filter that cannot
     change it must come back as that value. With truncation the weighted mean
     lands a hair under and every sample loses a level -- a bias of half a
     level over a whole image, which no structural assertion would notice. */
  imImage* src = create(16, 16, IM_GRAY, IM_BYTE);
  imImage* dst = create(16, 16, IM_GRAY, IM_BYTE);

  for (int i = 0; i < src->count; i++)
    ((imbyte*)src->data[0])[i] = 101;

  REQUIRE(imProcessBilateralFilter(src, dst, 2.0, 10.0) != 0);
  for (int i = 0; i < dst->count; i++)
    CHECK((int)((imbyte*)dst->data[0])[i] == 101);

  /* And a value the mean lands exactly between two levels on rounds up
     rather than down: half the image 10, half 11, filtered with a range
     stddev wide enough to mix them. */
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      ((imbyte*)src->data[0])[y * 16 + x] = (imbyte)((y < 8) ? 10 : 11);

  REQUIRE(imProcessBilateralFilter(src, dst, 3.0, 1000.0) != 0);

  int row = 7;
  int above = ((imbyte*)dst->data[0])[row * 16 + 8];
  CHECK(above >= 10);
  CHECK(above <= 11);

  imImageDestroy(src);
  imImageDestroy(dst);
}
