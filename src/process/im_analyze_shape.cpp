/** \file
 * \brief Region Shape and Intensity Measurements
 *
 * See Copyright Notice in im_lib.h
 */

#include <im.h>
#include <im_util.h>
#include <im_image.h>

#include "im_process_counter.h"
#include "im_process_ana.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* Four measurements over the labelled image that imAnalyzeFindRegions
 * produces, filling the gaps between what the library already measures --
 * area, centroid, perimeter, holes, principal axes -- and what particle
 * analysis actually reports.
 *
 * The one that is not a shape measurement is the important one.
 * imAnalyzeMeasureIntensity takes a SECOND image and reports the statistics
 * of its samples under each label. Everything else in im_process_ana.h
 * measures the label image alone, which means the library can tell you that a
 * region is 412 pixels and elongated but not how bright it is -- and in every
 * field that counts objects, how bright it is IS the measurement. The label
 * image usually came from thresholding the intensity image in the first
 * place, so the pairing is the normal case rather than an exotic one.
 *
 * The three shape measurements are the ones not derivable from what is
 * already there. Circularity is 4.pi.area/perimeter^2 and eccentricity comes
 * straight out of imAnalyzeMeasurePrincipalAxis, so neither is here; a
 * function that divides two numbers the caller already has is not an
 * addition. Feret diameters, the convex hull and the bounding box all need a
 * pass over the region's geometry that nothing else does.
 */


/* Convex hull of a region, by Andrew's monotone chain.
 *
 * The input is the region's boundary pixels as integer points. Sorting them
 * lexicographically and sweeping twice builds the lower and upper hulls in
 * O(n log n); the cross product decides each turn.
 *
 * Collinear points are DROPPED (the test is <= 0, not < 0). Keeping them
 * would not change the hull's area or its Feret diameters, but it puts
 * arbitrarily many points on a straight edge, and the rotating calipers below
 * assume consecutive hull vertices turn. */

#define IM_SHAPE_PI 3.14159265358979323846

typedef struct _imShapePoint
{
  int x, y;
} imShapePoint;

static int iComparePoints(const void* a, const void* b)
{
  const imShapePoint* p = (const imShapePoint*)a;
  const imShapePoint* q = (const imShapePoint*)b;

  if (p->x != q->x) return p->x - q->x;
  return p->y - q->y;
}

static double iCross(const imShapePoint* o, const imShapePoint* a, const imShapePoint* b)
{
  return (double)(a->x - o->x) * (double)(b->y - o->y) -
         (double)(a->y - o->y) * (double)(b->x - o->x);
}

/* Writes the hull into `hull', which must hold at least 2*count entries, and
   returns the number of vertices. The hull is counter-clockwise and the first
   vertex is not repeated at the end. */
static int iConvexHull(imShapePoint* points, int count, imShapePoint* hull)
{
  if (count < 3)
  {
    for (int i = 0; i < count; i++)
      hull[i] = points[i];
    return count;
  }

  qsort(points, (size_t)count, sizeof(imShapePoint), iComparePoints);

  int k = 0;

  for (int i = 0; i < count; i++)
  {
    while (k >= 2 && iCross(&hull[k-2], &hull[k-1], &points[i]) <= 0)
      k--;
    hull[k++] = points[i];
  }

  int lower = k + 1;

  for (int i = count - 2; i >= 0; i--)
  {
    while (k >= lower && iCross(&hull[k-2], &hull[k-1], &points[i]) <= 0)
      k--;
    hull[k++] = points[i];
  }

  /* The last point is the first one again. */
  return (k > 1) ? k - 1 : k;
}

/* Collect the boundary pixels of every region in one pass.
 *
 * Only boundary pixels are collected, not every pixel: the convex hull of a
 * region equals the convex hull of its boundary, and for a solid blob of
 * radius r that is O(r) points rather than O(r^2). A pixel is on the boundary
 * when at least one of its four neighbours carries a different label, with
 * outside the image counting as different -- so a region running off the edge
 * of the frame contributes the edge pixels rather than being left open.
 *
 * Returns an array of per-region point arrays and their counts, or NULL. */
static imShapePoint** iCollectBoundaries(const imImage* image, int region_count, int** point_counts)
{
  const imushort* img_data = (const imushort*)image->data[0];
  int width = image->width, height = image->height;

  int* counts = (int*)calloc((size_t)region_count, sizeof(int));
  imShapePoint** points = (imShapePoint**)calloc((size_t)region_count, sizeof(imShapePoint*));

  if (!counts || !points)
  {
    if (counts) free(counts);
    if (points) free(points);
    return NULL;
  }

  /* Two passes: count, allocate exactly, then fill. One pass with a growing
     array would allocate and copy for every region on an image that can hold
     65535 of them. */
  for (int pass = 0; pass < 2; pass++)
  {
    if (pass == 1)
    {
      for (int r = 0; r < region_count; r++)
      {
        if (counts[r] > 0)
        {
          points[r] = (imShapePoint*)malloc((size_t)counts[r] * sizeof(imShapePoint));
          if (!points[r])
          {
            for (int j = 0; j < r; j++)
              if (points[j]) free(points[j]);
            free(points);
            free(counts);
            return NULL;
          }
        }
        counts[r] = 0;   /* reused as the fill cursor */
      }
    }

    for (int y = 0; y < height; y++)
    {
      int offset = y * width;

      for (int x = 0; x < width; x++)
      {
        imushort label = img_data[offset + x];
        if (!label)
          continue;

        int index = label - 1;
        if (index >= region_count)
          continue;

        /* The order of these six terms is load-bearing, not cosmetic. Each
           edge test comes immediately BEFORE the neighbour read it protects,
           and || short-circuits, so a pixel on the left edge answers true at
           `x == 0' and never evaluates the read at offset + x - 1. Reordering
           this to put the four reads together is an out-of-bounds read on
           every border pixel -- and one that returns a plausible label rather
           than crashing, so it would present as a wrong hull, not as a
           fault. */
        int boundary =
          (x == 0)          || img_data[offset + x - 1] != label ||
          (x == width - 1)  || img_data[offset + x + 1] != label ||
          (y == 0)          || img_data[offset - width + x] != label ||
          (y == height - 1) || img_data[offset + width + x] != label;

        if (!boundary)
          continue;

        if (pass == 0)
          counts[index]++;
        else
        {
          points[index][counts[index]].x = x;
          points[index][counts[index]].y = y;
          counts[index]++;
        }
      }
    }
  }

  *point_counts = counts;
  return points;
}

static void iFreeBoundaries(imShapePoint** points, int* counts, int region_count)
{
  if (points)
  {
    for (int r = 0; r < region_count; r++)
      if (points[r]) free(points[r]);
    free(points);
  }
  if (counts) free(counts);
}


/*************************************************************************
  Bounding box
*************************************************************************/

int imAnalyzeMeasureBoundingBox(const imImage* image, int region_count,
                                int* xmin, int* xmax, int* ymin, int* ymax)
{
  assert(image->data_type == IM_USHORT && image->color_space == IM_GRAY);
  if (image->data_type != IM_USHORT || image->color_space != IM_GRAY)
    return IM_PROCESS_ABORT;

  const imushort* img_data = (const imushort*)image->data[0];
  int width = image->width, height = image->height;

  int counter = imProcessCounterBegin("MeasureBoundingBox");
  imCounterTotal(counter, height, "Analyzing...");

  for (int r = 0; r < region_count; r++)
  {
    if (xmin) xmin[r] = width;
    if (xmax) xmax[r] = -1;
    if (ymin) ymin[r] = height;
    if (ymax) ymax[r] = -1;
  }

  IM_INT_PROCESSING;

  /* Serial on purpose. The inner operation is a min/max against a shared
     array, which OpenMP can only do here with an atomic per sample or a
     per-thread copy of four arrays sized by the region count -- and the region
     count is bounded by 65535, so the private copies can be larger than the
     image. The loop is a single pass of comparisons; it is not the bottleneck
     in any pipeline that produced a labelled image to hand it. */
  for (int y = 0; y < height; y++)
  {
    IM_BEGIN_PROCESSING;

    int offset = y * width;

    for (int x = 0; x < width; x++)
    {
      imushort label = img_data[offset + x];
      if (!label)
        continue;

      int r = label - 1;
      if (r >= region_count)
        continue;

      if (xmin && x < xmin[r]) xmin[r] = x;
      if (xmax && x > xmax[r]) xmax[r] = x;
      if (ymin && y < ymin[r]) ymin[r] = y;
      if (ymax && y > ymax[r]) ymax[r] = y;
    }

    IM_COUNT_PROCESSING;
    IM_END_PROCESSING;
  }

  /* A label that never appeared keeps its sentinels, which would report a
     box of negative width. Report an empty box at the origin instead, so a
     caller that loops over every region gets numbers it can subtract. */
  if (processing)
  {
    for (int r = 0; r < region_count; r++)
    {
      int empty = (xmax && xmax[r] < 0) || (ymax && ymax[r] < 0);
      if (empty)
      {
        if (xmin) xmin[r] = 0;
        if (xmax) xmax[r] = -1;
        if (ymin) ymin[r] = 0;
        if (ymax) ymax[r] = -1;
      }
    }
  }

  imProcessCounterEnd(counter);

  return processing;
}


/*************************************************************************
  Convex hull area and perimeter
*************************************************************************/

int imAnalyzeMeasureConvexHull(const imImage* image, int region_count,
                               double* hull_area, double* hull_perim)
{
  assert(image->data_type == IM_USHORT && image->color_space == IM_GRAY);
  if (image->data_type != IM_USHORT || image->color_space != IM_GRAY)
    return IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("MeasureConvexHull");
  imCounterTotal(counter, region_count, "Analyzing...");

  int* counts = NULL;
  imShapePoint** points = iCollectBoundaries(image, region_count, &counts);
  if (!points)
  {
    imProcessCounterEnd(counter);
    return IM_PROCESS_ABORT;
  }

  imShapePoint* hull = NULL;
  int hull_capacity = 0;

  IM_INT_PROCESSING;

  for (int r = 0; r < region_count; r++)
  {
    IM_BEGIN_PROCESSING;

    if (hull_area) hull_area[r] = 0.0;
    if (hull_perim) hull_perim[r] = 0.0;

    if (counts[r] >= 1)
    {
      if (2 * counts[r] > hull_capacity)
      {
        hull_capacity = 2 * counts[r];
        imShapePoint* grown = (imShapePoint*)realloc(hull, (size_t)hull_capacity * sizeof(imShapePoint));
        if (!grown)
        {
          processing = IM_PROCESS_ABORT;
          break;
        }
        hull = grown;
      }

      int vertices = iConvexHull(points[r], counts[r], hull);

      /* Shoelace, and the closing edge back to the first vertex. A hull of
         one or two vertices has zero area and a perimeter that is twice the
         segment -- which is what a degenerate polygon's perimeter is, and is
         why a single-pixel region reports 0 rather than 1. */
      double area2 = 0.0, perim = 0.0;

      for (int i = 0; i < vertices; i++)
      {
        int j = (i + 1) % vertices;

        area2 += (double)hull[i].x * (double)hull[j].y -
                 (double)hull[j].x * (double)hull[i].y;

        double dx = (double)hull[j].x - (double)hull[i].x;
        double dy = (double)hull[j].y - (double)hull[i].y;
        perim += sqrt(dx*dx + dy*dy);
      }

      if (hull_area) hull_area[r] = fabs(area2) / 2.0;
      if (hull_perim) hull_perim[r] = perim;
    }

    IM_COUNT_PROCESSING;
    IM_END_PROCESSING;
  }

  if (hull) free(hull);
  iFreeBoundaries(points, counts, region_count);

  imProcessCounterEnd(counter);

  return processing;
}


/*************************************************************************
  Feret diameters
*************************************************************************/

int imAnalyzeMeasureFeret(const imImage* image, int region_count,
                          double* max_feret, double* max_angle,
                          double* min_feret, double* min_angle)
{
  assert(image->data_type == IM_USHORT && image->color_space == IM_GRAY);
  if (image->data_type != IM_USHORT || image->color_space != IM_GRAY)
    return IM_PROCESS_ABORT;

  int counter = imProcessCounterBegin("MeasureFeret");
  imCounterTotal(counter, region_count, "Analyzing...");

  int* counts = NULL;
  imShapePoint** points = iCollectBoundaries(image, region_count, &counts);
  if (!points)
  {
    imProcessCounterEnd(counter);
    return IM_PROCESS_ABORT;
  }

  imShapePoint* hull = NULL;
  int hull_capacity = 0;

  IM_INT_PROCESSING;

  for (int r = 0; r < region_count; r++)
  {
    IM_BEGIN_PROCESSING;

    if (max_feret) max_feret[r] = 0.0;
    if (max_angle) max_angle[r] = 0.0;
    if (min_feret) min_feret[r] = 0.0;
    if (min_angle) min_angle[r] = 0.0;

    if (counts[r] >= 1)
    {
      if (2 * counts[r] > hull_capacity)
      {
        hull_capacity = 2 * counts[r];
        imShapePoint* grown = (imShapePoint*)realloc(hull, (size_t)hull_capacity * sizeof(imShapePoint));
        if (!grown)
        {
          processing = IM_PROCESS_ABORT;
          break;
        }
        hull = grown;
      }

      int vertices = iConvexHull(points[r], counts[r], hull);

      /* Maximum Feret: the diameter of the hull, which is the largest
         distance between any two vertices. O(h^2) over hull vertices rather
         than rotating calipers' O(h) -- a hull has tens of vertices for the
         regions anyone measures, and the quadratic version has no degenerate
         cases to get wrong. */
      double best = 0.0, best_angle = 0.0;

      for (int i = 0; i < vertices; i++)
      {
        for (int j = i + 1; j < vertices; j++)
        {
          double dx = (double)hull[j].x - (double)hull[i].x;
          double dy = (double)hull[j].y - (double)hull[i].y;
          double distance = sqrt(dx*dx + dy*dy);

          if (distance > best)
          {
            best = distance;
            best_angle = atan2(dy, dx) * 180.0 / IM_SHAPE_PI;
          }
        }
      }

      /* Minimum Feret is NOT the smallest vertex-to-vertex distance -- that
         is usually the length of one short hull edge and has nothing to do
         with the width of the shape. It is the smallest width over all
         directions, and by the rotating-calipers theorem the minimum is
         attained with one hull edge flush against the caliper. So: for each
         edge, measure the greatest perpendicular distance to it. */
      double narrowest = -1.0, narrowest_angle = 0.0;

      if (vertices >= 3)
      {
        for (int i = 0; i < vertices; i++)
        {
          int j = (i + 1) % vertices;

          double ex = (double)hull[j].x - (double)hull[i].x;
          double ey = (double)hull[j].y - (double)hull[i].y;
          double length = sqrt(ex*ex + ey*ey);

          if (length == 0.0)
            continue;

          double width = 0.0;

          for (int k = 0; k < vertices; k++)
          {
            double px = (double)hull[k].x - (double)hull[i].x;
            double py = (double)hull[k].y - (double)hull[i].y;

            double distance = fabs(px * ey - py * ex) / length;
            if (distance > width)
              width = distance;
          }

          if (narrowest < 0.0 || width < narrowest)
          {
            narrowest = width;
            /* The width is measured perpendicular to the edge, so the
               direction the caliper points is the edge's normal. */
            narrowest_angle = atan2(ey, ex) * 180.0 / IM_SHAPE_PI + 90.0;
          }
        }
      }

      if (narrowest < 0.0)
      {
        /* Fewer than three hull vertices: the region is a point or a line,
           whose width across is zero and whose length is the max Feret. */
        narrowest = 0.0;
        narrowest_angle = best_angle + 90.0;
      }

      /* Angles are reported in [0,180): a diameter has no direction, so 190
         degrees and 10 degrees are the same measurement and reporting both
         spellings makes two identical regions compare unequal. */
      while (best_angle < 0.0) best_angle += 180.0;
      while (best_angle >= 180.0) best_angle -= 180.0;
      while (narrowest_angle < 0.0) narrowest_angle += 180.0;
      while (narrowest_angle >= 180.0) narrowest_angle -= 180.0;

      if (max_feret) max_feret[r] = best;
      if (max_angle) max_angle[r] = best_angle;
      if (min_feret) min_feret[r] = narrowest;
      if (min_angle) min_angle[r] = narrowest_angle;
    }

    IM_COUNT_PROCESSING;
    IM_END_PROCESSING;
  }

  if (hull) free(hull);
  iFreeBoundaries(points, counts, region_count);

  imProcessCounterEnd(counter);

  return processing;
}


/*************************************************************************
  Per-region intensity statistics
*************************************************************************/

template<class T>
static void iAccumulateIntensity(const imushort* label_data, const T* src_data, int count,
                                 int region_count, double* sum, double* sum2,
                                 double* min_value, double* max_value, int* pixels)
{
  for (int i = 0; i < count; i++)
  {
    imushort label = label_data[i];
    if (!label)
      continue;

    int r = label - 1;
    if (r >= region_count)
      continue;

    double value = (double)src_data[i];

    sum[r] += value;
    sum2[r] += value * value;

    if (pixels[r] == 0 || value < min_value[r]) min_value[r] = value;
    if (pixels[r] == 0 || value > max_value[r]) max_value[r] = value;

    pixels[r]++;
  }
}

int imAnalyzeMeasureIntensity(const imImage* label_image, const imImage* image, int plane,
                              int region_count, double* min_value, double* max_value,
                              double* mean, double* stddev, double* sum_value)
{
  assert(label_image->data_type == IM_USHORT && label_image->color_space == IM_GRAY);
  if (label_image->data_type != IM_USHORT || label_image->color_space != IM_GRAY)
    return IM_PROCESS_ABORT;

  assert(label_image->width == image->width && label_image->height == image->height);
  if (label_image->width != image->width || label_image->height != image->height)
    return IM_PROCESS_ABORT;

  assert(plane >= 0 && plane < image->depth);
  if (plane < 0 || plane >= image->depth)
    return IM_PROCESS_ABORT;

  int count = label_image->count;

  double* sum = (double*)calloc((size_t)region_count, sizeof(double));
  double* sum2 = (double*)calloc((size_t)region_count, sizeof(double));
  int* pixels = (int*)calloc((size_t)region_count, sizeof(int));
  double* local_min = (double*)calloc((size_t)region_count, sizeof(double));
  double* local_max = (double*)calloc((size_t)region_count, sizeof(double));

  if (!sum || !sum2 || !pixels || !local_min || !local_max)
  {
    if (sum) free(sum);
    if (sum2) free(sum2);
    if (pixels) free(pixels);
    if (local_min) free(local_min);
    if (local_max) free(local_max);
    return IM_PROCESS_ABORT;
  }

  int counter = imProcessCounterBegin("MeasureIntensity");
  imCounterTotal(counter, 1, "Analyzing...");

  const imushort* label_data = (const imushort*)label_image->data[0];
  int ret = IM_PROCESS_OK;

  switch (image->data_type)
  {
  case IM_BYTE:
    iAccumulateIntensity(label_data, (const imbyte*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  case IM_SHORT:
    iAccumulateIntensity(label_data, (const short*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  case IM_USHORT:
    iAccumulateIntensity(label_data, (const imushort*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  case IM_INT:
    iAccumulateIntensity(label_data, (const int*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  case IM_FLOAT:
    iAccumulateIntensity(label_data, (const float*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  case IM_DOUBLE:
    iAccumulateIntensity(label_data, (const double*)image->data[plane], count, region_count,
                         sum, sum2, local_min, local_max, pixels);
    break;
  default:
    ret = IM_PROCESS_ABORT;
    break;
  }

  if (ret)
  {
    for (int r = 0; r < region_count; r++)
    {
      int n = pixels[r];

      if (sum_value) sum_value[r] = sum[r];
      if (min_value) min_value[r] = (n > 0) ? local_min[r] : 0.0;
      if (max_value) max_value[r] = (n > 0) ? local_max[r] : 0.0;
      if (mean) mean[r] = (n > 0) ? sum[r] / (double)n : 0.0;

      if (stddev)
      {
        if (n > 1)
        {
          /* Divided by n-1, not n. That is the SAMPLE standard deviation, and
             it is chosen to agree with imCalcImageStatistics
             (im_statistics.cpp) rather than because a region's pixels are a
             sample of anything. Two measurements of the same quantity that
             differ by a factor of sqrt(n/(n-1)) are worse than either
             convention alone: the discrepancy is a fraction of a percent on a
             large region, which reads as a rounding difference rather than as
             two definitions.

             The sum-of-squares form can leave the variance a hair below zero
             through cancellation when every sample in a region is identical
             -- not a corner case, it is what a saturated region looks like --
             so it is floored rather than handed to sqrt, which would return a
             NaN that then propagates silently through every downstream
             average. */
          double variance = (sum2[r] - sum[r] * sum[r] / (double)n) / ((double)n - 1.0);
          stddev[r] = (variance > 0.0) ? sqrt(variance) : 0.0;
        }
        else
          stddev[r] = 0.0;
      }
    }

    imCounterInc(counter);
  }

  free(sum);
  free(sum2);
  free(pixels);
  free(local_min);
  free(local_max);

  imProcessCounterEnd(counter);

  return ret;
}
