/* Covers src/process/im_analyze_shape.cpp -- bounding box, convex hull,
 * Feret diameters and per-region intensity statistics.
 *
 * Shapes with known answers, computed by hand, rather than round-trips.
 * A measurement that is self-consistently wrong -- a Feret diameter that
 * reported the longest hull EDGE instead of the longest hull DIAGONAL, say --
 * satisfies every invariant you can state without knowing the answer: it is
 * positive, it scales with the shape, it is the same for congruent regions,
 * and on a disc the two are nearly equal. Only a rectangle of known size,
 * where the diagonal and the edge differ by a factor you can write down,
 * separates them.
 *
 * The pixel-coordinate convention is worth stating once. All of these measure
 * between pixel CENTRES, so a solid run of n pixels has a Feret diameter of
 * n-1 and an n-by-m rectangle has a convex hull of area (n-1)(m-1). That is
 * the same convention imAnalyzeMeasureCentroid uses -- a single pixel at
 * (3,4) has its centroid at (3,4), not at (3.5,4.5) -- and differs by one
 * from the pixel COUNT that imAnalyzeMeasureArea reports. Both are right;
 * they measure different things, and mixing them is the standard way to get a
 * solidity slightly over 1.
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
  const int W = 64;
  const int H = 48;

  imImage* create(int width, int height, int color_space, int data_type)
  {
    imImage* image = imImageCreate(width, height, color_space, data_type);
    REQUIRE(image != NULL);
    return image;
  }

  imImage* labels()
  {
    imImage* image = create(W, H, IM_GRAY, IM_USHORT);
    memset(image->data[0], 0, (size_t)image->count * sizeof(imushort));
    return image;
  }

  void rectangle(imImage* image, int x0, int y0, int x1, int y1, int label)
  {
    imushort* data = (imushort*)image->data[0];
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++)
        data[y * image->width + x] = (imushort)label;
  }

  void filled_disc(imImage* image, double cx, double cy, double radius, int label)
  {
    imushort* data = (imushort*)image->data[0];
    for (int y = 0; y < image->height; y++)
    {
      for (int x = 0; x < image->width; x++)
      {
        double dx = x - cx, dy = y - cy;
        if (dx*dx + dy*dy <= radius*radius)
          data[y * image->width + x] = (imushort)label;
      }
    }
  }
}

TEST_CASE("MeasureBoundingBox: exact for known rectangles")
{
  imImage* image = labels();

  rectangle(image, 5, 7, 20, 11, 1);
  rectangle(image, 40, 30, 44, 44, 2);

  int xmin[2], xmax[2], ymin[2], ymax[2];
  REQUIRE(imAnalyzeMeasureBoundingBox(image, 2, xmin, xmax, ymin, ymax) != 0);

  CHECK(xmin[0] == 5);  CHECK(xmax[0] == 20);
  CHECK(ymin[0] == 7);  CHECK(ymax[0] == 11);

  CHECK(xmin[1] == 40); CHECK(xmax[1] == 44);
  CHECK(ymin[1] == 30); CHECK(ymax[1] == 44);

  /* Inclusive, so this is the pixel count along each axis. */
  CHECK(xmax[0] - xmin[0] + 1 == 16);
  CHECK(ymax[0] - ymin[0] + 1 == 5);

  imImageDestroy(image);
}

TEST_CASE("MeasureBoundingBox: a label that does not occur reports an empty box")
{
  imImage* image = labels();
  rectangle(image, 2, 2, 4, 4, 1);

  /* Three regions asked for, one present. Regions 2 and 3 must not come back
     with the internal sentinels, which would say the box runs from width to
     -1 and give a caller computing xmax-xmin+1 a negative width. */
  int xmin[3], xmax[3], ymin[3], ymax[3];
  REQUIRE(imAnalyzeMeasureBoundingBox(image, 3, xmin, xmax, ymin, ymax) != 0);

  for (int r = 1; r < 3; r++)
  {
    CHECK(xmin[r] == 0);
    CHECK(xmax[r] == -1);
    CHECK(ymin[r] == 0);
    CHECK(ymax[r] == -1);
    CHECK(xmax[r] - xmin[r] + 1 == 0);
  }

  imImageDestroy(image);
}

TEST_CASE("MeasureBoundingBox: any output may be NULL")
{
  imImage* image = labels();
  rectangle(image, 5, 7, 20, 11, 1);

  int ymax[1];
  REQUIRE(imAnalyzeMeasureBoundingBox(image, 1, NULL, NULL, NULL, ymax) != 0);
  CHECK(ymax[0] == 11);

  imImageDestroy(image);
}

TEST_CASE("MeasureConvexHull: a rectangle is its own hull")
{
  imImage* image = labels();
  rectangle(image, 10, 10, 25, 20, 1);

  double area[1], perim[1];
  REQUIRE(imAnalyzeMeasureConvexHull(image, 1, area, perim) != 0);

  /* Between pixel centres: 15 across and 10 down. */
  CHECK(area[0] == doctest::Approx(15.0 * 10.0));
  CHECK(perim[0] == doctest::Approx(2.0 * (15.0 + 10.0)));

  imImageDestroy(image);
}

TEST_CASE("MeasureConvexHull: solidity separates a convex region from a concave one")
{
  imImage* image = labels();

  /* Region 1: a solid 16x16 square, convex. */
  rectangle(image, 4, 4, 19, 19, 1);

  /* Region 2: a U -- the same 16x16 square with a notch 8 wide and 11 deep
     cut into one side.

     A U and not an L, deliberately. An L's convex hull is NOT its bounding
     box: the hull bridges the cut corner with a single diagonal and encloses
     nearly everything, so an L of three quarters of a square has a solidity
     around 0.97, not 0.75. A U keeps material in all four corners of its
     bounding box, so its hull IS the bounding box exactly -- which makes the
     expected hull area a number that can be written down rather than
     estimated, and makes the notch count fully against the solidity. */
  rectangle(image, 34, 4, 49, 19, 2);
  rectangle(image, 38, 4, 45, 14, 0);

  double hull_area[2];
  REQUIRE(imAnalyzeMeasureConvexHull(image, 2, hull_area, NULL) != 0);

  int area[2];
  REQUIRE(imAnalyzeMeasureArea(image, area, 2) != 0);

  CHECK(area[0] == 16 * 16);
  CHECK(hull_area[0] == doctest::Approx(15.0 * 15.0));

  /* 256 pixels less the 8x11 notch. */
  CHECK(area[1] == 256 - 88);

  /* The assertion that the hull really did bridge the notch: it is the whole
     bounding box, to the last digit. A "hull" that merely traced the outline
     would come back near 168 and the solidity below would be near 1. */
  CHECK(hull_area[1] == doctest::Approx(15.0 * 15.0));

  /* Solidity. Note the pixel-count vs pixel-centre difference described in
     the file header: the square's is slightly OVER 1, which is correct and is
     exactly why the concave case is compared against the square rather than
     against the number 1. */
  double square_solidity = (double)area[0] / hull_area[0];
  double u_solidity = (double)area[1] / hull_area[1];

  CHECK(u_solidity < square_solidity);
  CHECK(u_solidity == doctest::Approx(168.0 / 225.0).epsilon(0.01));

  imImageDestroy(image);
}

TEST_CASE("MeasureConvexHull: a disc's hull area approaches pi r squared")
{
  imImage* image = labels();
  filled_disc(image, 30.0, 24.0, 15.0, 1);

  double area[1], perim[1];
  REQUIRE(imAnalyzeMeasureConvexHull(image, 1, area, perim) != 0);

  /* A hull over pixel centres is inscribed in the true circle, so it comes in
     slightly under; 5% is comfortable at this radius. */
  CHECK(area[0] == doctest::Approx(3.14159265358979 * 15.0 * 15.0).epsilon(0.05));
  CHECK(perim[0] == doctest::Approx(2.0 * 3.14159265358979 * 15.0).epsilon(0.05));

  imImageDestroy(image);
}

TEST_CASE("MeasureFeret: a horizontal bar")
{
  imImage* image = labels();
  rectangle(image, 10, 20, 29, 20, 1);   /* one row, 20 pixels */

  double max_feret[1], max_angle[1], min_feret[1], min_angle[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, max_feret, max_angle, min_feret, min_angle) != 0);

  CHECK(max_feret[0] == doctest::Approx(19.0));
  CHECK(max_angle[0] == doctest::Approx(0.0));

  /* A line has no width. */
  CHECK(min_feret[0] == doctest::Approx(0.0));
  CHECK(min_angle[0] == doctest::Approx(90.0));

  imImageDestroy(image);
}

TEST_CASE("MeasureFeret: a square's maximum is its diagonal, not its side")
{
  imImage* image = labels();
  rectangle(image, 10, 10, 30, 30, 1);   /* 21 x 21, so 20 between centres */

  double max_feret[1], max_angle[1], min_feret[1], min_angle[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, max_feret, max_angle, min_feret, min_angle) != 0);

  /* This is the assertion that separates a real Feret diameter from the
     longest hull edge: 28.28 against 20. */
  CHECK(max_feret[0] == doctest::Approx(20.0 * sqrt(2.0)).epsilon(1e-9));
  CHECK(min_feret[0] == doctest::Approx(20.0).epsilon(1e-9));

  /* The diagonal of an axis-aligned square runs at 45 or 135 degrees; which
     of the two depends on which diagonal the search happened to find first,
     and both are correct answers. */
  CHECK((max_angle[0] == doctest::Approx(45.0) || max_angle[0] == doctest::Approx(135.0)));

  /* The minimum width is across a side, so the caliper points along an axis. */
  CHECK((min_angle[0] == doctest::Approx(0.0) || min_angle[0] == doctest::Approx(90.0)));

  imImageDestroy(image);
}

TEST_CASE("MeasureFeret: an elongated rectangle reports its orientation")
{
  imImage* image = labels();
  rectangle(image, 5, 22, 44, 25, 1);   /* 40 wide, 4 tall */

  double max_feret[1], max_angle[1], min_feret[1], min_angle[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, max_feret, max_angle, min_feret, min_angle) != 0);

  CHECK(max_feret[0] == doctest::Approx(sqrt(39.0*39.0 + 3.0*3.0)).epsilon(1e-9));
  CHECK(min_feret[0] == doctest::Approx(3.0).epsilon(1e-9));

  /* Long axis nearly horizontal. */
  CHECK(max_angle[0] < 10.0);

  /* And the narrow direction is across it. */
  CHECK(min_angle[0] > 80.0);
  CHECK(min_angle[0] < 100.0);

  imImageDestroy(image);
}

TEST_CASE("MeasureFeret: a disc is isotropic")
{
  imImage* image = labels();
  filled_disc(image, 32.0, 24.0, 14.0, 1);

  double max_feret[1], min_feret[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, max_feret, NULL, min_feret, NULL) != 0);

  CHECK(max_feret[0] == doctest::Approx(28.0).epsilon(0.06));
  CHECK(min_feret[0] == doctest::Approx(28.0).epsilon(0.06));

  /* Aspect ratio near 1 is the property that makes this a disc rather than
     merely a blob of the right size. */
  CHECK(max_feret[0] / min_feret[0] < 1.12);

  imImageDestroy(image);
}

/* No square brackets in this name, and not for style. doctest_discover_tests
   parses the output of --list-test-cases through a CMake regex, and a '[' in
   a case name opens a character class in it: the generated
   im_tests_tests-*.cmake then concatenates that case and EVERY case after it
   into one malformed add_test(), and ctest fails to load the whole suite with
   "add_test called with incorrect number of arguments". The cases still pass
   when the binary is run directly, which is what makes it confusing. The name
   below said "[0,180)" first and cost the rest of this file its
   registration. */
TEST_CASE("MeasureFeret: angles are reported in the half-open range 0 to 180")
{
  imImage* image = labels();

  /* A bar running up-left to down-right, whose natural atan2 answer is
     negative. Reporting -45 and 135 for two identical regions that happen to
     be traversed in opposite orders would make them compare unequal. */
  imushort* data = (imushort*)image->data[0];
  for (int i = 0; i < 20; i++)
    data[(10 + i) * W + (40 - i)] = 1;

  double max_angle[1], min_angle[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, NULL, max_angle, NULL, min_angle) != 0);

  CHECK(max_angle[0] >= 0.0);
  CHECK(max_angle[0] < 180.0);
  CHECK(min_angle[0] >= 0.0);
  CHECK(min_angle[0] < 180.0);

  CHECK(max_angle[0] == doctest::Approx(135.0).epsilon(0.02));

  imImageDestroy(image);
}

TEST_CASE("MeasureFeret: a region touching the frame edge is closed, not open")
{
  imImage* image = labels();

  /* Flush against the left edge. The boundary collector treats outside the
     image as a different label, so the edge column is part of the outline;
     if it did not, the hull would be missing its left side and both
     diameters would be short. */
  rectangle(image, 0, 10, 9, 20, 1);

  double max_feret[1], min_feret[1];
  REQUIRE(imAnalyzeMeasureFeret(image, 1, max_feret, NULL, min_feret, NULL) != 0);

  CHECK(max_feret[0] == doctest::Approx(sqrt(9.0*9.0 + 10.0*10.0)).epsilon(1e-9));
  CHECK(min_feret[0] == doctest::Approx(9.0).epsilon(1e-9));

  imImageDestroy(image);
}

TEST_CASE("MeasureIntensity: exact statistics for hand-built regions")
{
  imImage* label_image = labels();
  imImage* intensity = create(W, H, IM_GRAY, IM_DOUBLE);

  rectangle(label_image, 0, 0, 3, 0, 1);   /* four pixels */
  rectangle(label_image, 10, 0, 11, 0, 2); /* two pixels */

  double* values = (double*)intensity->data[0];
  memset(values, 0, (size_t)intensity->count * sizeof(double));

  values[0] = 10.0; values[1] = 20.0; values[2] = 30.0; values[3] = 40.0;
  values[10] = 7.0; values[11] = 9.0;

  double vmin[2], vmax[2], mean[2], stddev[2], sum[2];
  REQUIRE(imAnalyzeMeasureIntensity(label_image, intensity, 0, 2, vmin, vmax, mean, stddev, sum) != 0);

  CHECK(vmin[0] == doctest::Approx(10.0));
  CHECK(vmax[0] == doctest::Approx(40.0));
  CHECK(mean[0] == doctest::Approx(25.0));
  CHECK(sum[0] == doctest::Approx(100.0));
  /* Divided by n-1, matching imCalcImageStatistics: the deviations of
     10,20,30,40 about 25 are -15,-5,5,15, so the sum of squares is 500 and
     the answer is sqrt(500/3). Dividing by n instead gives sqrt(125) =
     11.18 against 12.91 -- close enough to look like rounding, which is
     exactly why this is pinned. */
  CHECK(stddev[0] == doctest::Approx(sqrt(500.0 / 3.0)));

  CHECK(vmin[1] == doctest::Approx(7.0));
  CHECK(vmax[1] == doctest::Approx(9.0));
  CHECK(mean[1] == doctest::Approx(8.0));
  CHECK(sum[1] == doctest::Approx(16.0));
  CHECK(stddev[1] == doctest::Approx(sqrt(2.0)));

  imImageDestroy(label_image);
  imImageDestroy(intensity);
}

TEST_CASE("MeasureIntensity: agrees with imCalcImageStatistics when one region covers everything")
{
  /* A cross-check against an implementation that was already here and was
     written by someone else. Hand-computed expectations and this function
     could agree on a wrong definition of standard deviation; the library's
     own statistics could not. */
  imImage* label_image = labels();
  imImage* intensity = create(W, H, IM_GRAY, IM_DOUBLE);

  rectangle(label_image, 0, 0, W - 1, H - 1, 1);

  double* values = (double*)intensity->data[0];
  unsigned int state = 12345u;
  for (int i = 0; i < intensity->count; i++)
  {
    state = state * 1103515245u + 12345u;
    values[i] = (double)((state >> 16) & 0x7FFF) / 100.0;
  }

  double vmin[1], vmax[1], mean[1], stddev[1], sum[1];
  REQUIRE(imAnalyzeMeasureIntensity(label_image, intensity, 0, 1, vmin, vmax, mean, stddev, sum) != 0);

  imStats stats;
  REQUIRE(imCalcImageStatistics(intensity, &stats) != 0);

  CHECK(vmin[0] == doctest::Approx(stats.min));
  CHECK(vmax[0] == doctest::Approx(stats.max));
  CHECK(mean[0] == doctest::Approx(stats.mean));
  CHECK(stddev[0] == doctest::Approx(stats.stddev).epsilon(1e-9));

  imImageDestroy(label_image);
  imImageDestroy(intensity);
}

TEST_CASE("MeasureIntensity: a uniform region has zero standard deviation, not a NaN")
{
  /* The sum-of-squares form can leave the variance a hair below zero through
     cancellation when every sample is identical -- which is what a saturated
     region looks like, so it is not a corner case. sqrt of that is a NaN,
     and a NaN in a measurement table propagates into every downstream
     average without announcing itself. */
  imImage* label_image = labels();
  imImage* intensity = create(W, H, IM_GRAY, IM_DOUBLE);

  rectangle(label_image, 5, 5, 30, 30, 1);

  double* values = (double*)intensity->data[0];
  for (int i = 0; i < intensity->count; i++)
    values[i] = 65535.0;

  double stddev[1], mean[1];
  REQUIRE(imAnalyzeMeasureIntensity(label_image, intensity, 0, 1, NULL, NULL, mean, stddev, NULL) != 0);

  CHECK(stddev[0] == doctest::Approx(0.0));
  CHECK(stddev[0] == stddev[0]);          /* not a NaN */
  CHECK(mean[0] == doctest::Approx(65535.0));

  imImageDestroy(label_image);
  imImageDestroy(intensity);
}

TEST_CASE("MeasureIntensity: reads the plane it is asked for")
{
  imImage* label_image = labels();
  imImage* rgb = create(W, H, IM_RGB, IM_BYTE);

  rectangle(label_image, 0, 0, 9, 0, 1);

  for (int p = 0; p < 3; p++)
    memset(rgb->data[p], 0, (size_t)rgb->count);

  for (int i = 0; i < 10; i++)
  {
    ((imbyte*)rgb->data[0])[i] = 10;
    ((imbyte*)rgb->data[1])[i] = 20;
    ((imbyte*)rgb->data[2])[i] = 30;
  }

  for (int p = 0; p < 3; p++)
  {
    double mean[1];
    REQUIRE(imAnalyzeMeasureIntensity(label_image, rgb, p, 1, NULL, NULL, mean, NULL, NULL) != 0);
    CHECK(mean[0] == doctest::Approx(10.0 * (p + 1)));
  }

  imImageDestroy(label_image);
  imImageDestroy(rgb);
}

TEST_CASE("MeasureIntensity: reads every supported data type")
{
  const int types[] = { IM_BYTE, IM_SHORT, IM_USHORT, IM_INT, IM_FLOAT, IM_DOUBLE };

  for (int t = 0; t < 6; t++)
  {
    imImage* label_image = labels();
    imImage* intensity = create(W, H, IM_GRAY, types[t]);

    rectangle(label_image, 0, 0, 3, 0, 1);

    memset(intensity->data[0], 0, (size_t)intensity->count * imDataTypeSize(types[t]));

    for (int i = 0; i < 4; i++)
    {
      double v = 10.0 * (i + 1);
      switch (types[t])
      {
      case IM_BYTE:   ((imbyte*)intensity->data[0])[i] = (imbyte)v;     break;
      case IM_SHORT:  ((short*)intensity->data[0])[i] = (short)v;       break;
      case IM_USHORT: ((imushort*)intensity->data[0])[i] = (imushort)v; break;
      case IM_INT:    ((int*)intensity->data[0])[i] = (int)v;           break;
      case IM_FLOAT:  ((float*)intensity->data[0])[i] = (float)v;       break;
      case IM_DOUBLE: ((double*)intensity->data[0])[i] = v;             break;
      }
    }

    double mean[1], sum[1];
    REQUIRE(imAnalyzeMeasureIntensity(label_image, intensity, 0, 1, NULL, NULL, mean, NULL, sum) != 0);

    CHECK(mean[0] == doctest::Approx(25.0));
    CHECK(sum[0] == doctest::Approx(100.0));

    imImageDestroy(label_image);
    imImageDestroy(intensity);
  }
}

TEST_CASE("the shape measurements agree with the region labeller they are fed by")
{
  /* End to end: threshold nothing, just build two discs, label them with the
     library's own function, and check that every measurement indexes the
     regions the way imAnalyzeFindRegions numbered them. Getting the 1-based
     label to 0-based array index wrong is the classic defect here, and it
     shows as every measurement being attributed to the wrong region while
     each individual number is perfectly plausible. */
  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  imbyte* mask = (imbyte*)binary->data[0];
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      double dx = x - 12.0, dy = y - 12.0;
      if (dx*dx + dy*dy <= 36.0) mask[y * W + x] = 1;        /* small, r=6 */

      dx = x - 44.0; dy = y - 30.0;
      if (dx*dx + dy*dy <= 169.0) mask[y * W + x] = 1;       /* large, r=13 */
    }
  }

  imImage* label_image = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imAnalyzeFindRegions(binary, label_image, 8, 1, &regions) != 0);
  REQUIRE(regions == 2);

  int area[2];
  double max_feret[2];
  int xmin[2], xmax[2], ymin[2], ymax[2];

  REQUIRE(imAnalyzeMeasureArea(label_image, area, 2) != 0);
  REQUIRE(imAnalyzeMeasureFeret(label_image, 2, max_feret, NULL, NULL, NULL) != 0);
  REQUIRE(imAnalyzeMeasureBoundingBox(label_image, 2, xmin, xmax, ymin, ymax) != 0);

  /* Whichever way round the labeller numbered them, the region with the
     larger area must also be the one with the larger Feret diameter and the
     larger bounding box -- and the three must agree on WHICH. */
  int big = (area[0] > area[1]) ? 0 : 1;
  int small = 1 - big;

  CHECK(max_feret[big] > max_feret[small]);
  CHECK(xmax[big] - xmin[big] > xmax[small] - xmin[small]);
  CHECK(ymax[big] - ymin[big] > ymax[small] - ymin[small]);

  /* And the numbers are the discs we drew. */
  CHECK(max_feret[big] == doctest::Approx(26.0).epsilon(0.08));
  CHECK(max_feret[small] == doctest::Approx(12.0).epsilon(0.12));

  imImageDestroy(binary);
  imImageDestroy(label_image);
}

#ifdef NDEBUG
TEST_CASE("the shape measurements check their label image")
{
  imImage* wrong_type = create(W, H, IM_GRAY, IM_INT);
  imImage* intensity = create(W, H, IM_GRAY, IM_DOUBLE);

  int box[1];
  double value[1];

  CHECK(imAnalyzeMeasureBoundingBox(wrong_type, 1, box, NULL, NULL, NULL) == 0);
  CHECK(imAnalyzeMeasureConvexHull(wrong_type, 1, value, NULL) == 0);
  CHECK(imAnalyzeMeasureFeret(wrong_type, 1, value, NULL, NULL, NULL) == 0);
  CHECK(imAnalyzeMeasureIntensity(wrong_type, intensity, 0, 1, NULL, NULL, value, NULL, NULL) == 0);

  imImage* label_image = labels();

  /* A plane index past the end of the intensity image. */
  CHECK(imAnalyzeMeasureIntensity(label_image, intensity, 1, 1, NULL, NULL, value, NULL, NULL) == 0);
  CHECK(imAnalyzeMeasureIntensity(label_image, intensity, -1, 1, NULL, NULL, value, NULL, NULL) == 0);

  /* And an intensity image of a different size. */
  imImage* small = create(W / 2, H, IM_GRAY, IM_DOUBLE);
  CHECK(imAnalyzeMeasureIntensity(label_image, small, 0, 1, NULL, NULL, value, NULL, NULL) == 0);
  imImageDestroy(small);

  imImageDestroy(wrong_type);
  imImageDestroy(intensity);
  imImageDestroy(label_image);
}
#endif
