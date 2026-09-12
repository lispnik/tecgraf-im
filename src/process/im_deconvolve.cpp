/** \file
 * \brief Deconvolution
 *
 * See Copyright Notice in im_lib.h
 */

#include <im.h>
#include <im_util.h>

#include "im_process_counter.h"
#include "im_process_check.h"
#include "im_process_sample.h"
#include "im_process_loc.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* Richardson-Lucy deconvolution.
 *
 * Given an observed image g, a point spread function h, and the model
 *
 *     g = (f * h) + noise,     noise Poisson
 *
 * the maximum-likelihood estimate of f is the fixed point of
 *
 *     f_{k+1} = f_k . ( (g / (f_k * h)) * h^T )
 *
 * where * is convolution, h^T is h reflected through its centre, and the
 * multiplication and division are pixelwise. Richardson (1972) and Lucy
 * (1974) derived it independently; it is the standard restoration for
 * microscopy and astronomy, which is where the Poisson assumption comes from
 * -- both count photons.
 *
 * Two properties are worth knowing before using it. The iteration preserves
 * total flux, because h sums to one and each update is a ratio, which is what
 * makes the result photometrically meaningful rather than merely sharper.
 * And it does not converge to something pleasant: past a few tens of
 * iterations it begins fitting the noise, and the characteristic result is
 * ringing around bright features that grows with every further iteration.
 * The iteration count is the regularisation. There is no stopping rule here
 * because there is no good general one.
 *
 * SPATIAL DOMAIN, DELIBERATELY. The textbook implementation transforms into
 * the frequency domain, where each convolution is a multiply, and for a large
 * PSF that is much faster. It is not done here, for two reasons. The first is
 * that this library's FFT entry points are duplicated by the optional
 * libim_fftw3, which redefines six of libim_process's symbols; a call to
 * imProcessFFT from inside libim_process would be resolved by the dynamic
 * loader's search order rather than by anything written here, so which
 * implementation ran would depend on what the consumer happened to load. The
 * second is that frequency-domain convolution is circular: bright content at
 * one edge wraps around and contaminates the opposite edge, and the usual fix
 * (pad, transform, crop) costs most of the speed back. A direct convolution
 * with replicated borders has neither problem, and for the PSF sizes that
 * matter here -- a Gaussian of a few pixels, an airy disc, a measured bead --
 * it is fast enough.
 */


/* One convolution pass, borders replicated, computing in double.
   `flip' reflects the kernel through its centre, which is the difference
   between the blur step and the correction step of the iteration. */
static void iConvolveDouble(const double* src_data, double* dst_data, int width, int height,
                            const double* kernel, int kernel_width, int kernel_height, int flip)
{
  int kw2 = kernel_width / 2;
  int kh2 = kernel_height / 2;

#ifdef _OPENMP
#pragma omp parallel for if (IM_OMP_MINHEIGHT(height))
#endif
  for (int y = 0; y < height; y++)
  {
    int line_offset = y * width;

    for (int x = 0; x < width; x++)
    {
      double sum = 0.0;

      for (int ky = -kh2; ky <= kh2; ky++)
      {
        int sy = y + ky;
        if (sy < 0) sy = 0; else if (sy > height - 1) sy = height - 1;

        const double* src_line = src_data + sy * width;

        /* Reflecting the kernel is the same as negating both offsets when
           reading it, and the kernel is the small array, so it is read
           backwards rather than the image. */
        const double* kernel_line = flip ? kernel + (kh2 - ky) * kernel_width + kw2
                                         : kernel + (ky + kh2) * kernel_width + kw2;

        for (int kx = -kw2; kx <= kw2; kx++)
        {
          int sx = x + kx;
          if (sx < 0) sx = 0; else if (sx > width - 1) sx = width - 1;

          sum += kernel_line[flip ? -kx : kx] * src_line[sx];
        }
      }

      dst_data[line_offset + x] = sum;
    }
  }
}

/* The PSF as a normalised double array. A PSF that does not sum to one
   changes the image's total flux on every iteration -- it converges to
   something, but not to a restoration of the input -- and IM's own
   imProcessRenderGaussian does not normalise, so this is the difference
   between the obvious call sequence working and appearing to. */
static double* iNormalizePSF(const imImage* psf_image)
{
  int count = psf_image->width * psf_image->height;

  double* kernel = (double*)malloc(count * sizeof(double));
  if (!kernel)
    return NULL;

  double sum = 0.0;

  for (int i = 0; i < count; i++)
  {
    double value;

    switch (psf_image->data_type)
    {
    case IM_BYTE:   value = (double)((imbyte*)psf_image->data[0])[i];   break;
    case IM_SHORT:  value = (double)((short*)psf_image->data[0])[i];    break;
    case IM_USHORT: value = (double)((imushort*)psf_image->data[0])[i]; break;
    case IM_INT:    value = (double)((int*)psf_image->data[0])[i];      break;
    case IM_FLOAT:  value = (double)((float*)psf_image->data[0])[i];    break;
    case IM_DOUBLE: value = ((double*)psf_image->data[0])[i];           break;
    default:        value = 0.0;                                       break;
    }

    /* A negative PSF sample is not a point spread function. Clamping rather
       than rejecting is deliberate: a Gaussian rendered into a signed type
       and then rescaled can carry a sample or two of numerical undershoot,
       and refusing that would fail a call that means exactly what it says. */
    if (value < 0.0) value = 0.0;

    kernel[i] = value;
    sum += value;
  }

  if (sum <= 0.0)
  {
    free(kernel);
    return NULL;
  }

  for (int i = 0; i < count; i++)
    kernel[i] /= sum;

  return kernel;
}

template<class T>
static int DoRichardsonLucy(const T* src_data, T* dst_data, int width, int height,
                            const double* kernel, int kernel_width, int kernel_height,
                            int iterations, int counter)
{
  int count = width * height;

  double* observed = (double*)malloc(count * sizeof(double));
  double* estimate = (double*)malloc(count * sizeof(double));
  double* blurred = (double*)malloc(count * sizeof(double));
  double* ratio = (double*)malloc(count * sizeof(double));
  double* correction = (double*)malloc(count * sizeof(double));

  if (!observed || !estimate || !blurred || !ratio || !correction)
  {
    if (observed) free(observed);
    if (estimate) free(estimate);
    if (blurred) free(blurred);
    if (ratio) free(ratio);
    if (correction) free(correction);
    return IM_PROCESS_ABORT;
  }

  /* The observed image is both the data and the initial estimate. Starting
     from a flat image converges to the same place and takes longer.

     The update divides by the blurred estimate and multiplies by the old one,
     so a zero is a fixed point that nothing can ever lift back off zero, and
     a negative value makes the likelihood meaningless. Both are ordinary in
     real data -- a dark-subtracted frame is full of them -- so the input is
     floored here rather than the caller being refused. */
  for (int i = 0; i < count; i++)
  {
    double value = (double)src_data[i];
    if (value < 0.0) value = 0.0;

    observed[i] = value;
    estimate[i] = value;
  }

  IM_INT_PROCESSING;

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    IM_BEGIN_PROCESSING;

    iConvolveDouble(estimate, blurred, width, height, kernel, kernel_width, kernel_height, 0);

    for (int i = 0; i < count; i++)
    {
      /* Where the blurred estimate has gone to zero the observed value can
         only be zero too (the PSF is non-negative and so is the estimate), so
         the ratio is 0/0. One is the neutral element of the update, which is
         the right answer: leave that pixel alone. */
      ratio[i] = (blurred[i] > 0.0) ? observed[i] / blurred[i] : 1.0;
    }

    iConvolveDouble(ratio, correction, width, height, kernel, kernel_width, kernel_height, 1);

    for (int i = 0; i < count; i++)
    {
      double value = estimate[i] * correction[i];
      estimate[i] = (value > 0.0) ? value : 0.0;
    }

    IM_COUNT_PROCESSING;
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
    IM_END_PROCESSING;

    if (processing == IM_PROCESS_ABORT)
      break;
  }

  if (processing)
  {
    for (int i = 0; i < count; i++)
      dst_data[i] = imStoreSample<T>(estimate[i]);
  }

  free(observed);
  free(estimate);
  free(blurred);
  free(ratio);
  free(correction);

  return processing;
}

int imProcessRichardsonLucy(const imImage* src_image, const imImage* psf_image, imImage* dst_image, int iterations)
{
  assert(imCheckSameTypeSize(src_image, dst_image));
  if (!imCheckSameTypeSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(iterations >= 0);
  if (iterations < 0)
    return IM_PROCESS_ABORT;

  /* An even-sided PSF has no centre pixel, so "the centre" would be a choice
     between two, and the restored image would come out shifted by half a
     pixel with nothing to say so. */
  assert(psf_image->width % 2 == 1 && psf_image->height % 2 == 1);
  if (psf_image->width % 2 != 1 || psf_image->height % 2 != 1)
    return IM_PROCESS_ABORT;

  assert(psf_image->depth == 1);
  if (psf_image->depth != 1)
    return IM_PROCESS_ABORT;

  double* kernel = iNormalizePSF(psf_image);
  if (!kernel)
    return IM_PROCESS_ABORT;

  int ret = IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("RichardsonLucy");
  imCounterTotal(counter, src_image->depth * iterations, "Processing...");

  for (int i = 0; i < src_image->depth; i++)
  {
    switch (src_image->data_type)
    {
    case IM_BYTE:
      ret = DoRichardsonLucy((const imbyte*)src_image->data[i], (imbyte*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    case IM_SHORT:
      ret = DoRichardsonLucy((const short*)src_image->data[i], (short*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    case IM_USHORT:
      ret = DoRichardsonLucy((const imushort*)src_image->data[i], (imushort*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    case IM_INT:
      ret = DoRichardsonLucy((const int*)src_image->data[i], (int*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    case IM_FLOAT:
      ret = DoRichardsonLucy((const float*)src_image->data[i], (float*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    case IM_DOUBLE:
      ret = DoRichardsonLucy((const double*)src_image->data[i], (double*)dst_image->data[i],
                             src_image->width, src_image->height, kernel,
                             psf_image->width, psf_image->height, iterations, counter);
      break;
    default:
      ret = IM_PROCESS_ABORT;
      break;
    }

    if (!ret)
      break;
  }

  free(kernel);

  imProcessCounterEnd(counter);

  return ret;
}
