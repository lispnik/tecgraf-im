/** \file
 * \brief Edge-preserving denoising
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

/* The library's existing smoothing is Gaussian, mean, median and the rank
   filters. All of them are defined by a neighbourhood and nothing else, so
   all of them blur an edge exactly as readily as they blur noise. That is the
   wrong trade for a measurement image: the edge is usually the thing being
   measured.

   The three filters here are the standard edge-preserving answers, and they
   fail in different enough ways to be worth having all three:

   - Bilateral weights a neighbour by intensity distance as well as spatial
     distance, so it does not average across a step. One pass, two parameters,
     no iteration count to get wrong. It flattens texture into patches when
     the range stddev is set much above the noise.

   - Anisotropic diffusion (Perona-Malik) runs a diffusion whose conductance
     falls off where the gradient is large. It preserves and even sharpens
     edges over many small steps, at the cost of an iteration count and a
     stability condition on the step size.

   - Non-local means averages over pixels whose *neighbourhoods* match, not
     whose positions are close, which is the only one of the three that
     recovers repeated fine structure rather than smoothing it. It is also
     one to two orders of magnitude more expensive.

   All three compute in double regardless of the image type, because all three
   accumulate a weighted mean and doing that in imbyte quantises the weights
   into uselessness. */


/*************************************************************************
  Bilateral filter
*************************************************************************/

template<class T>
static int DoBilateralFilter(const T* src_data, T* dst_data, int width, int height,
                             double spatial_stddev, double range_stddev, int counter)
{
  int radius = (int)ceil(2.0 * spatial_stddev);
  if (radius < 1) radius = 1;

  int size = 2 * radius + 1;

  /* The spatial term depends only on the offset, so it is a table. The range
     term depends on the pair of values and cannot be, for a float image. */
  double* spatial = (double*)malloc(size * size * sizeof(double));
  if (!spatial)
    return IM_PROCESS_ABORT;

  double spatial_factor = -1.0 / (2.0 * spatial_stddev * spatial_stddev);
  for (int dy = -radius; dy <= radius; dy++)
  {
    for (int dx = -radius; dx <= radius; dx++)
      spatial[(dy + radius) * size + (dx + radius)] = exp(spatial_factor * (double)(dx*dx + dy*dy));
  }

  double range_factor = -1.0 / (2.0 * range_stddev * range_stddev);

  IM_INT_PROCESSING;

#ifdef _OPENMP
#pragma omp parallel for if (IM_OMP_MINHEIGHT(height))
#endif
  for (int y = 0; y < height; y++)
  {
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
    IM_BEGIN_PROCESSING;

    int line_offset = y * width;

    for (int x = 0; x < width; x++)
    {
      double centre = (double)src_data[line_offset + x];
      double sum = 0.0, weight_sum = 0.0;

      int y0 = y - radius; if (y0 < 0) y0 = 0;
      int y1 = y + radius; if (y1 > height - 1) y1 = height - 1;
      int x0 = x - radius; if (x0 < 0) x0 = 0;
      int x1 = x + radius; if (x1 > width - 1) x1 = width - 1;

      for (int ny = y0; ny <= y1; ny++)
      {
        const T* src_line = src_data + ny * width;
        const double* spatial_line = spatial + (ny - y + radius) * size + radius;

        for (int nx = x0; nx <= x1; nx++)
        {
          double value = (double)src_line[nx];
          double difference = value - centre;
          double weight = spatial_line[nx - x] * exp(range_factor * difference * difference);

          sum += weight * value;
          weight_sum += weight;
        }
      }

      /* weight_sum includes the centre, whose two weights are both exp(0), so
         it is never zero and this is a division rather than a special case. */
      dst_data[line_offset + x] = imStoreSample<T>(sum / weight_sum);
    }

    IM_COUNT_PROCESSING;
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
    IM_END_PROCESSING;
  }

  free(spatial);

  return processing;
}

int imProcessBilateralFilter(const imImage* src_image, imImage* dst_image, double spatial_stddev, double range_stddev)
{
  assert(imCheckSameTypeSize(src_image, dst_image));
  if (!imCheckSameTypeSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(spatial_stddev > 0 && range_stddev > 0);
  if (!(spatial_stddev > 0) || !(range_stddev > 0))
    return IM_PROCESS_ABORT;

  int ret = IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("BilateralFilter");
  imCounterTotal(counter, src_image->depth * src_image->height, "Processing...");

  for (int i = 0; i < src_image->depth; i++)
  {
    switch (src_image->data_type)
    {
    case IM_BYTE:
      ret = DoBilateralFilter((const imbyte*)src_image->data[i], (imbyte*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    case IM_SHORT:
      ret = DoBilateralFilter((const short*)src_image->data[i], (short*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    case IM_USHORT:
      ret = DoBilateralFilter((const imushort*)src_image->data[i], (imushort*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    case IM_INT:
      ret = DoBilateralFilter((const int*)src_image->data[i], (int*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    case IM_FLOAT:
      ret = DoBilateralFilter((const float*)src_image->data[i], (float*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    case IM_DOUBLE:
      ret = DoBilateralFilter((const double*)src_image->data[i], (double*)dst_image->data[i],
                              src_image->width, src_image->height, spatial_stddev, range_stddev, counter);
      break;
    default:
      ret = IM_PROCESS_ABORT;
      break;
    }

    if (!ret)
      break;
  }

  imProcessCounterEnd(counter);

  return ret;
}


/*************************************************************************
  Anisotropic diffusion (Perona-Malik)
*************************************************************************/

static inline double iConductance(double gradient, double kappa, int func)
{
  double ratio = gradient / kappa;

  switch (func)
  {
  case IM_DIFFUSION_QUADRATIC:
    /* Favours wide regions over smaller ones. */
    return 1.0 / (1.0 + ratio * ratio);

  case IM_DIFFUSION_TUKEY:
    /* Compactly supported: a gradient past the threshold conducts nothing at
       all, rather than a little. Edges stop moving instead of creeping. */
    if (ratio * ratio > 1.0)
      return 0.0;
    else
    {
      double t = 1.0 - ratio * ratio;
      return 0.5 * t * t;
    }

  case IM_DIFFUSION_EXPONENTIAL:
  default:
    /* Favours high-contrast edges over low-contrast ones. */
    return exp(-ratio * ratio);
  }
}

static int DoAnisotropicDiffusion(double* buffer, double* next, int width, int height,
                                  double time_step, double kappa, int iterations, int func,
                                  int counter)
{
  IM_INT_PROCESSING;

  for (int iteration = 0; iteration < iterations; iteration++)
  {
#ifdef _OPENMP
#pragma omp parallel for if (IM_OMP_MINHEIGHT(height))
#endif
    for (int y = 0; y < height; y++)
    {
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
      IM_BEGIN_PROCESSING;

      int offset = y * width;

      /* Neumann boundary: the image is extended by replication, so a border
         pixel sees a zero gradient across the border and nothing diffuses in
         or out of it. Clamping the index is how that is spelled. */
      int north = (y > 0) ? offset - width : offset;
      int south = (y < height - 1) ? offset + width : offset;

      for (int x = 0; x < width; x++)
      {
        int west = (x > 0) ? x - 1 : x;
        int east = (x < width - 1) ? x + 1 : x;

        double centre = buffer[offset + x];

        double gn = buffer[north + x] - centre;
        double gs = buffer[south + x] - centre;
        double gw = buffer[offset + west] - centre;
        double ge = buffer[offset + east] - centre;

        next[offset + x] = centre + time_step * (iConductance(fabs(gn), kappa, func) * gn +
                                                 iConductance(fabs(gs), kappa, func) * gs +
                                                 iConductance(fabs(gw), kappa, func) * gw +
                                                 iConductance(fabs(ge), kappa, func) * ge);
      }

      IM_COUNT_PROCESSING;
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
      IM_END_PROCESSING;
    }

    if (!processing)
      return IM_PROCESS_ABORT;

    /* The buffers are swapped rather than copied. The caller is handed
       whichever one holds the last iteration, which is why this returns
       through the pointer-to-pointer dance in the wrapper below rather than
       promising the result is in `buffer'. */
    double* swap = buffer;
    buffer = next;
    next = swap;
  }

  return processing;
}

template<class T>
static int DoAnisotropicDiffusionPlane(const T* src_data, T* dst_data, int width, int height,
                                       double time_step, double kappa, int iterations, int func,
                                       int counter)
{
  int count = width * height;

  double* buffer = (double*)malloc(count * sizeof(double));
  double* next = (double*)malloc(count * sizeof(double));
  if (!buffer || !next)
  {
    if (buffer) free(buffer);
    if (next) free(next);
    return IM_PROCESS_ABORT;
  }

  for (int i = 0; i < count; i++)
    buffer[i] = (double)src_data[i];

  int ret = DoAnisotropicDiffusion(buffer, next, width, height, time_step, kappa, iterations, func, counter);

  /* An even iteration count leaves the result in `buffer', an odd one in
     `next'. Recomputing the parity here is the whole reason the swap loop
     above is allowed to be as terse as it is. */
  const double* result = (iterations % 2 == 0) ? buffer : next;

  if (ret)
  {
    for (int i = 0; i < count; i++)
      dst_data[i] = imStoreSample<T>(result[i]);
  }

  free(buffer);
  free(next);

  return ret;
}

int imProcessAnisotropicDiffusion(const imImage* src_image, imImage* dst_image, double time_step,
                                  double kappa, int iterations, int func)
{
  assert(imCheckSameTypeSize(src_image, dst_image));
  if (!imCheckSameTypeSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(kappa > 0 && iterations >= 0);
  if (!(kappa > 0) || iterations < 0)
    return IM_PROCESS_ABORT;

  /* The explicit scheme is stable only for a step of 1/4 or less with four
     neighbours -- past that the iteration oscillates and then diverges, which
     looks like a checkerboard rather than like an error. Documented as a
     precondition and enforced, because a silently wrong image is the one
     failure this whole file exists to avoid. */
  assert(time_step > 0 && time_step <= 0.25);
  if (!(time_step > 0) || time_step > 0.25)
    return IM_PROCESS_ABORT;

  int ret = IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("AnisotropicDiffusion");
  imCounterTotal(counter, src_image->depth * src_image->height * iterations, "Processing...");

  for (int i = 0; i < src_image->depth; i++)
  {
    switch (src_image->data_type)
    {
    case IM_BYTE:
      ret = DoAnisotropicDiffusionPlane((const imbyte*)src_image->data[i], (imbyte*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    case IM_SHORT:
      ret = DoAnisotropicDiffusionPlane((const short*)src_image->data[i], (short*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    case IM_USHORT:
      ret = DoAnisotropicDiffusionPlane((const imushort*)src_image->data[i], (imushort*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    case IM_INT:
      ret = DoAnisotropicDiffusionPlane((const int*)src_image->data[i], (int*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    case IM_FLOAT:
      ret = DoAnisotropicDiffusionPlane((const float*)src_image->data[i], (float*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    case IM_DOUBLE:
      ret = DoAnisotropicDiffusionPlane((const double*)src_image->data[i], (double*)dst_image->data[i],
                                        src_image->width, src_image->height, time_step, kappa, iterations, func, counter);
      break;
    default:
      ret = IM_PROCESS_ABORT;
      break;
    }

    if (!ret)
      break;
  }

  imProcessCounterEnd(counter);

  return ret;
}


/*************************************************************************
  Non-local means
*************************************************************************/

template<class T>
static int DoNonLocalMeans(const T* src_data, T* dst_data, int width, int height,
                           int search_radius, int patch_radius, double filter_stddev,
                           int counter)
{
  int patch_size = 2 * patch_radius + 1;
  int patch_count = patch_size * patch_size;

  double filter_factor = -1.0 / (filter_stddev * filter_stddev);

  IM_INT_PROCESSING;

#ifdef _OPENMP
#pragma omp parallel for if (IM_OMP_MINHEIGHT(height))
#endif
  for (int y = 0; y < height; y++)
  {
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
    IM_BEGIN_PROCESSING;

    int line_offset = y * width;

    for (int x = 0; x < width; x++)
    {
      double sum = 0.0, weight_sum = 0.0, max_weight = 0.0;

      int sy0 = y - search_radius; if (sy0 < 0) sy0 = 0;
      int sy1 = y + search_radius; if (sy1 > height - 1) sy1 = height - 1;
      int sx0 = x - search_radius; if (sx0 < 0) sx0 = 0;
      int sx1 = x + search_radius; if (sx1 > width - 1) sx1 = width - 1;

      for (int cy = sy0; cy <= sy1; cy++)
      {
        for (int cx = sx0; cx <= sx1; cx++)
        {
          if (cx == x && cy == y)
            continue;   /* handled after the loop, see below */

          /* Mean squared difference between the two patches. Both are
             clamped to the image, and clamping each independently would
             compare a patch against a differently-shaped one, so the offsets
             are clamped jointly: the patch is whatever window is in range for
             BOTH centres. */
          double distance = 0.0;

          for (int py = -patch_radius; py <= patch_radius; py++)
          {
            int ay = y + py, by = cy + py;
            if (ay < 0) ay = 0; else if (ay > height - 1) ay = height - 1;
            if (by < 0) by = 0; else if (by > height - 1) by = height - 1;

            const T* a_line = src_data + ay * width;
            const T* b_line = src_data + by * width;

            for (int px = -patch_radius; px <= patch_radius; px++)
            {
              int ax = x + px, bx = cx + px;
              if (ax < 0) ax = 0; else if (ax > width - 1) ax = width - 1;
              if (bx < 0) bx = 0; else if (bx > width - 1) bx = width - 1;

              double difference = (double)a_line[ax] - (double)b_line[bx];
              distance += difference * difference;
            }
          }

          distance /= (double)patch_count;

          double weight = exp(filter_factor * distance);

          if (weight > max_weight)
            max_weight = weight;

          sum += weight * (double)src_data[cy * width + cx];
          weight_sum += weight;
        }
      }

      /* The centre pixel's distance from itself is zero, so its weight would
         be exactly 1 -- larger than every other weight, by construction, and
         usually by a lot. That makes the filter a no-op in the flat regions
         it is supposed to smooth most. Giving the centre the largest weight
         any OTHER pixel earned is the standard fix, and the reason the centre
         is skipped in the loop above rather than special-cased inside it.

         max_weight is zero only when the search window holds no other pixel,
         which needs a 1x1 image; the centre then keeps its own value. */
      if (max_weight == 0.0)
        max_weight = 1.0;

      sum += max_weight * (double)src_data[line_offset + x];
      weight_sum += max_weight;

      dst_data[line_offset + x] = imStoreSample<T>(sum / weight_sum);
    }

    IM_COUNT_PROCESSING;
#ifdef _OPENMP
#pragma omp flush (processing)
#endif
    IM_END_PROCESSING;
  }

  return processing;
}

int imProcessNonLocalMeans(const imImage* src_image, imImage* dst_image, int search_radius,
                           int patch_radius, double filter_stddev)
{
  assert(imCheckSameTypeSize(src_image, dst_image));
  if (!imCheckSameTypeSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(search_radius > 0 && patch_radius > 0 && filter_stddev > 0);
  if (search_radius <= 0 || patch_radius <= 0 || !(filter_stddev > 0))
    return IM_PROCESS_ABORT;

  int ret = IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("NonLocalMeans");
  imCounterTotal(counter, src_image->depth * src_image->height, "Processing...");

  for (int i = 0; i < src_image->depth; i++)
  {
    switch (src_image->data_type)
    {
    case IM_BYTE:
      ret = DoNonLocalMeans((const imbyte*)src_image->data[i], (imbyte*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    case IM_SHORT:
      ret = DoNonLocalMeans((const short*)src_image->data[i], (short*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    case IM_USHORT:
      ret = DoNonLocalMeans((const imushort*)src_image->data[i], (imushort*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    case IM_INT:
      ret = DoNonLocalMeans((const int*)src_image->data[i], (int*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    case IM_FLOAT:
      ret = DoNonLocalMeans((const float*)src_image->data[i], (float*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    case IM_DOUBLE:
      ret = DoNonLocalMeans((const double*)src_image->data[i], (double*)dst_image->data[i],
                            src_image->width, src_image->height, search_radius, patch_radius, filter_stddev, counter);
      break;
    default:
      ret = IM_PROCESS_ABORT;
      break;
    }

    if (!ret)
      break;
  }

  imProcessCounterEnd(counter);

  return ret;
}
