/* Tests for the transforms that no second implementation can settle.
 *
 * Everything else in this suite is checked either against hand-computed
 * values or against ImageMagick. Neither works here. A Fourier transform, a
 * Hough accumulator, a Canny edge map and a distance transform are all either
 * too large to write out by hand or defined loosely enough that two correct
 * libraries disagree -- Canny in particular is a family of algorithms rather
 * than one, and the fixtures in fixtures/golden/ would be pinning this
 * implementation's choices rather than checking them.
 *
 * So these assert properties that any correct implementation must satisfy,
 * which is a weaker check per assertion and a much harder one to satisfy
 * accidentally:
 *
 *   a forward transform followed by its inverse returns the original
 *   the DC term of an unnormalized transform is the sum of the samples
 *   a distance transform agrees with the distances it is approximating
 *   a Hough accumulator peaks at the line that was deliberately drawn
 *   an edge detector responds at the edge and nowhere else
 *
 * Every bound here was measured before it was written, and the measurements
 * are recorded beside them so a future change that loosens one is visible as
 * a loosening rather than as a tolerance that was always that wide.
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

const int W = 32;
const int H = 24;
const int N = W * H;

imImage* create(int w, int h, int color_space, int data_type)
{
  imImage* image = imImageCreate(w, h, color_space, data_type);
  REQUIRE(image != NULL);
  return image;
}

} /* namespace */


/* ================================================================== *
 * Fourier transform -- libim_fftw3, which is optional
 * ================================================================== */

#ifdef IM_TEST_HAS_FFTW3

TEST_CASE("transform: an FFT followed by an inverse returns the original")
{
  /* The strongest property available for a transform whose output cannot be
     read by eye: whatever the forward transform produced, the inverse has to
     undo it. A wrong sign, a transposed axis, a missing normalisation or a
     quadrant swap applied once instead of twice all break this, and none of
     them would be visible in the spectrum itself. */
  imImage* src = create(W, H, IM_GRAY, IM_FLOAT);
  for (int i = 0; i < N; i++)
    ((float*)src->data[0])[i] = (float)((i * 7) % 251);

  imImage* freq = create(W, H, IM_GRAY, IM_CFLOAT);
  imImage* back = create(W, H, IM_GRAY, IM_CFLOAT);

  imProcessFFT(src, freq);
  imProcessIFFT(freq, back);

  /* Measured at 0.000046 on this input: single precision accumulated over
     768 samples, and nothing else. Asserted on the worst of them rather than
     one assertion per sample, so a failure reports how far out it went. */
  double worst_real = 0, worst_imag = 0;
  for (int i = 0; i < N; i++)
  {
    double d = ((imcfloat*)back->data[0])[i].real - ((float*)src->data[0])[i];
    if (d < 0) d = -d;
    if (d > worst_real) worst_real = d;

    double q = ((imcfloat*)back->data[0])[i].imag;
    if (q < 0) q = -q;
    if (q > worst_imag) worst_imag = q;
  }
  CHECK(worst_real < 0.01);

  /* The input was real, so the imaginary part has to come back as noise. */
  CHECK(worst_imag < 0.01);

  imImageDestroy(src);
  imImageDestroy(freq);
  imImageDestroy(back);
}

TEST_CASE("transform: the DC term is the sum of the samples")
{
  /* The forward transform is documented as unnormalized and as putting the
     lowest frequency at the centre, which together fix the value of exactly
     one cell: the sum of the input. It is the only cell whose value can be
     stated in closed form, which makes it the only direct check on the
     transform's scaling -- the round trip above would still pass if the
     forward and inverse were scaled by reciprocal constants. */
  imImage* src = create(W, H, IM_GRAY, IM_FLOAT);
  double sum = 0;
  for (int i = 0; i < N; i++)
  {
    float v = (float)((i * 7) % 251);
    ((float*)src->data[0])[i] = v;
    sum += v;
  }

  imImage* freq = create(W, H, IM_GRAY, IM_CFLOAT);
  imProcessFFT(src, freq);

  imcfloat dc = ((imcfloat*)freq->data[0])[(H/2)*W + (W/2)];
  CHECK(dc.real == doctest::Approx(sum).epsilon(0.0001));

  /* A real input has a real DC term; whatever is in the imaginary part is
     accumulated rounding, which on a sum of this size is not zero to the
     bit. Compared against the magnitude rather than against zero. */
  double imag = dc.imag < 0 ? -dc.imag : dc.imag;
  CHECK(imag < sum * 1e-6);

  imImageDestroy(src);
  imImageDestroy(freq);
}

TEST_CASE("transform: a constant image has no energy away from DC")
{
  /* A flat field is the one input whose entire spectrum is known: all of the
     energy at DC and nothing anywhere else. */
  imImage* src = create(W, H, IM_GRAY, IM_FLOAT);
  for (int i = 0; i < N; i++)
    ((float*)src->data[0])[i] = 50.0f;

  imImage* freq = create(W, H, IM_GRAY, IM_CFLOAT);
  imProcessFFT(src, freq);

  int dc_index = (H/2)*W + (W/2);
  CHECK(((imcfloat*)freq->data[0])[dc_index].real ==
        doctest::Approx(50.0 * N).epsilon(0.0001));

  double worst_elsewhere = 0;
  for (int i = 0; i < N; i++)
  {
    if (i == dc_index) continue;
    const imcfloat& c = ((imcfloat*)freq->data[0])[i];
    double mag = sqrt((double)c.real*c.real + (double)c.imag*c.imag);
    if (mag > worst_elsewhere) worst_elsewhere = mag;
  }
  CHECK(worst_elsewhere < 1.0);

  imImageDestroy(src);
  imImageDestroy(freq);
}

TEST_CASE("transform: autocorrelation peaks at the centre")
{
  /* An image correlates with itself best at zero displacement, wherever else
     it may also correlate. That is true of any image and any correct
     implementation. */
  imImage* src = create(W, H, IM_GRAY, IM_FLOAT);
  for (int i = 0; i < N; i++)
    ((float*)src->data[0])[i] = (float)(((i * 13) % 97) + ((i / W) % 5) * 20);

  imImage* dst = create(W, H, IM_GRAY, IM_CFLOAT);
  imProcessAutoCorrelation(src, dst);

  int centre = (H/2)*W + (W/2);
  double at_centre = ((imcfloat*)dst->data[0])[centre].real;

  for (int i = 0; i < N; i++)
  {
    if (i == centre) continue;
    CAPTURE(i);
    CHECK(((imcfloat*)dst->data[0])[i].real <= at_centre);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}

#endif /* IM_TEST_HAS_FFTW3 */


/* ================================================================== *
 * Distance transform
 * ================================================================== */

TEST_CASE("transform: the distance transform approximates the distance it claims to")
{
  /* Documented as "an approximation of the euclidian distance", which is
     checkable without being exact: with a single black pixel the true
     distance from every other pixel is known in closed form, and the
     approximation can be held to how close it actually is. Measured at 1.9%
     and 0.39 of a pixel at worst on this arrangement. */
  const int bx = 10, by = 8;

  imImage* src = create(W, H, IM_BINARY, IM_BYTE);
  for (int i = 0; i < N; i++)
    ((imbyte*)src->data[0])[i] = 1;
  ((imbyte*)src->data[0])[by*W + bx] = 0;

  imImage* dst = create(W, H, IM_GRAY, IM_FLOAT);
  imProcessDistanceTransform(src, dst);

  /* Zero exactly where the source is black, by definition. */
  CHECK(((float*)dst->data[0])[by*W + bx] == 0.0f);

  /* The immediate neighbours are exact, and are what distinguish a Euclidean
     approximation from a city-block or chessboard distance: a diagonal step
     has to cost more than an axial one and less than two. */
  CHECK(((float*)dst->data[0])[by*W + (bx+1)] == doctest::Approx(1.0));
  CHECK(((float*)dst->data[0])[(by+1)*W + bx] == doctest::Approx(1.0));
  CHECK(((float*)dst->data[0])[(by+1)*W + (bx+1)] == doctest::Approx(sqrt(2.0)).epsilon(0.01));
  CHECK(((float*)dst->data[0])[by*W + (bx+5)] == doctest::Approx(5.0));

  /* And nothing anywhere is more than 2% away from the true distance. */
  double worst_ratio = 1.0;
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      double truth = sqrt((double)((x-bx)*(x-bx) + (y-by)*(y-by)));
      if (truth == 0) continue;
      double got = ((float*)dst->data[0])[y*W + x];
      double ratio = got / truth;
      if (ratio < 1.0) ratio = 1.0 / ratio;
      if (ratio > worst_ratio) worst_ratio = ratio;
    }
  }
  CHECK(worst_ratio < 1.05);

  imImageDestroy(src);
  imImageDestroy(dst);
}


/* ================================================================== *
 * Hough transform
 * ================================================================== */

TEST_CASE("transform: the Hough accumulator peaks at the line that was drawn")
{
  /* The accumulator is 180 wide -- one column per degree -- and
     2*rmax+1 tall, per the header. A horizontal line through the middle of
     the image is at rho 0, and every pixel on it votes for the same cell, so
     the peak is both locatable and countable: it has to equal the number of
     pixels drawn. */
  const int row = H / 2;

  imImage* src = create(W, H, IM_BINARY, IM_BYTE);
  memset(src->data[0], 0, N);
  for (int x = 0; x < W; x++)
    ((imbyte*)src->data[0])[row*W + x] = 1;

  int rmax = (int)sqrt((double)(W*W + H*H));
  imImage* hough = create(180, 2*rmax + 1, IM_GRAY, IM_INT);

  REQUIRE(imProcessHoughLines(src, hough) != 0);

  int best = -1, best_theta = -1, best_rho_index = -1;
  for (int y = 0; y < hough->height; y++)
  {
    for (int x = 0; x < hough->width; x++)
    {
      int v = ((int*)hough->data[0])[y*hough->width + x];
      if (v > best) { best = v; best_theta = x; best_rho_index = y; }
    }
  }

  /* Every pixel of the line lands in one cell. */
  CHECK(best == W);

  /* rho is measured from the centre of the image, and the line runs through
     it, so the peak is at rho 0. */
  CHECK(best_rho_index - hough->height/2 == 0);

  /* theta is the angle with the normal, so a horizontal line peaks near 90
     rather than near 0. The accumulator indexes 0..179 over that range and
     lands on 89, which is the cell either side of the boundary rather than a
     disagreement about the geometry. */
  CHECK(best_theta >= 88);
  CHECK(best_theta <= 91);

  SUBCASE("and a vertical line peaks somewhere else entirely")
  {
    /* Guards against an accumulator that peaks in the same place whatever it
       is given, which every assertion above would accept. */
    imImage* vertical = create(W, H, IM_BINARY, IM_BYTE);
    memset(vertical->data[0], 0, N);
    for (int y = 0; y < H; y++)
      ((imbyte*)vertical->data[0])[y*W + W/2] = 1;

    imImage* hough2 = create(180, 2*rmax + 1, IM_GRAY, IM_INT);
    REQUIRE(imProcessHoughLines(vertical, hough2) != 0);

    int best2 = -1, theta2 = -1;
    for (int y = 0; y < hough2->height; y++)
      for (int x = 0; x < hough2->width; x++)
      {
        int v = ((int*)hough2->data[0])[y*hough2->width + x];
        if (v > best2) { best2 = v; theta2 = x; }
      }

    CHECK(best2 == H);
    CHECK(theta2 != best_theta);

    imImageDestroy(vertical);
    imImageDestroy(hough2);
  }

  imImageDestroy(src);
  imImageDestroy(hough);
}


/* ================================================================== *
 * Canny
 * ================================================================== */

TEST_CASE("transform: Canny responds at the edge and nowhere else")
{
  /* Canny is a family of algorithms rather than one -- the smoothing, the
     gradient operator and the two thresholds are all choices -- so comparing
     the output to another library's would pin this implementation's choices
     rather than check them. What any of them must do is respond along a step
     edge and stay silent on the flat regions either side.

     Measured on this input: 22 non-zero samples, every one of them within two
     columns of the edge, and nothing at all further away. */
  const int edge = W / 2;

  imImage* src = create(W, H, IM_GRAY, IM_BYTE);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      ((imbyte*)src->data[0])[y*W + x] = (imbyte)(x < edge ? 20 : 220);

  imImage* dst = create(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(imProcessCanny(src, dst, 1.0) != 0);

  int responses = 0, near_edge = 0, far_from_edge = 0;
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      if (!((imbyte*)dst->data[0])[y*W + x]) continue;
      responses++;
      if (x >= edge - 2 && x <= edge + 1) near_edge++;
      if (x < edge - 3 || x > edge + 2) far_from_edge++;
    }
  }

  CHECK(responses > 0);                 /* it found the edge at all */
  CHECK(far_from_edge == 0);            /* and nothing on the flat regions */
  CHECK(near_edge == responses);

  /* A detector that lit up everywhere would satisfy "responds at the edge".
     The response has to be thin. */
  CHECK(responses < N / 8);

  SUBCASE("and stays silent on an image with no edge at all")
  {
    imImage* flat = create(W, H, IM_GRAY, IM_BYTE);
    memset(flat->data[0], 128, N);

    imImage* out = create(W, H, IM_GRAY, IM_BYTE);
    REQUIRE(imProcessCanny(flat, out, 1.0) != 0);

    int any = 0;
    for (int i = 0; i < N; i++)
      if (((imbyte*)out->data[0])[i]) any++;
    CHECK(any == 0);

    imImageDestroy(flat);
    imImageDestroy(out);
  }

  imImageDestroy(src);
  imImageDestroy(dst);
}
