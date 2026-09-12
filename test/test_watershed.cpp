/* Covers src/process/im_watershed.cpp -- marker-controlled watershed and the
 * touching-object segmentation built on it.
 *
 * The headline case is the one this whole operation exists for: two discs
 * that overlap are ONE connected region, imAnalyzeFindRegions says 1, and
 * imProcessWatershedSegment says 2. That pair of assertions in a single case
 * is the entire argument for the function, so it is written as a pair.
 *
 * The rest guard the properties that a watershed can get subtly wrong while
 * still producing a plausible label image:
 *
 *   - every foreground pixel is labelled, and only with labels that exist;
 *   - with lines on, no two different labels are ever adjacent;
 *   - with lines off, no pixel is left unlabelled;
 *   - the split of two equal discs is near the neck, not near one centre.
 *
 * That last one is what the FIFO tie-break in the priority queue buys. A
 * distance transform is flat across a plateau, so a large fraction of all
 * pixels compare equal; processing them in heap order rather than insertion
 * order still produces a segmentation, and still splits the blob in two, but
 * the line wanders off the neck. Nothing but a measurement catches it.
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
  imImage* create(int width, int height, int color_space, int data_type)
  {
    imImage* image = imImageCreate(width, height, color_space, data_type);
    REQUIRE(image != NULL);
    return image;
  }

  void disc(imImage* image, double cx, double cy, double radius)
  {
    imbyte* data = (imbyte*)image->data[0];

    for (int y = 0; y < image->height; y++)
    {
      for (int x = 0; x < image->width; x++)
      {
        double dx = x - cx, dy = y - cy;
        if (dx*dx + dy*dy <= radius*radius)
          data[y * image->width + x] = 1;
      }
    }
  }

  int label_at(imImage* image, int x, int y)
  {
    return (int)((imushort*)image->data[0])[y * image->width + x];
  }

  /* How many distinct non-zero labels the image carries. Static rather than
     automatic: an imushort label space is 65536 entries, and a quarter of a
     megabyte is more stack than a test should take. */
  int distinct_labels(imImage* image)
  {
    static int seen[65536];
    memset(seen, 0, sizeof(seen));

    int count = 0;
    for (int i = 0; i < image->count; i++)
    {
      int label = (int)((imushort*)image->data[0])[i];
      if (label && !seen[label])
      {
        seen[label] = 1;
        count++;
      }
    }
    return count;
  }
}

TEST_CASE("WatershedSegment: splits two touching discs that FindRegions cannot")
{
  const int W = 80, H = 48;

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  /* Two discs of radius 12 whose centres are 18 apart: they overlap by six
     pixels, so they are one 8-connected blob with a visible neck. */
  disc(binary, 31.0, 24.0, 12.0);
  disc(binary, 49.0, 24.0, 12.0);

  imImage* labels = create(W, H, IM_GRAY, IM_USHORT);

  /* What the library could do before. One region, because they touch. */
  int found = 0;
  REQUIRE(imAnalyzeFindRegions(binary, labels, 8, 1, &found) != 0);
  CHECK(found == 1);

  /* What it can do now. */
  imImage* separated = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imProcessWatershedSegment(binary, separated, 8, 1, &regions) != 0);
  CHECK(regions == 2);

  /* And the two labels really are the two discs: the pixel at each centre
     carries a different one. */
  int left = label_at(separated, 31, 24);
  int right = label_at(separated, 49, 24);
  CHECK(left != 0);
  CHECK(right != 0);
  CHECK(left != right);

  imImageDestroy(binary);
  imImageDestroy(labels);
  imImageDestroy(separated);
}

TEST_CASE("WatershedSegment: leaves separated discs alone")
{
  const int W = 80, H = 48;

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  disc(binary, 20.0, 24.0, 10.0);
  disc(binary, 60.0, 24.0, 10.0);

  imImage* separated = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imProcessWatershedSegment(binary, separated, 8, 1, &regions) != 0);

  /* Two objects before, two after: the segmentation must not invent a split
     in an isolated convex object. That is the failure mode opposite to the
     one it is for, and the easier one to introduce. */
  CHECK(regions == 2);

  imImage* labels = create(W, H, IM_GRAY, IM_USHORT);
  int found = 0;
  REQUIRE(imAnalyzeFindRegions(binary, labels, 8, 1, &found) != 0);
  CHECK(found == 2);

  imImageDestroy(binary);
  imImageDestroy(separated);
  imImageDestroy(labels);
}

TEST_CASE("WatershedSegment: the background stays background, and the split is at the neck")
{
  const int W = 80, H = 48;

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  disc(binary, 31.0, 24.0, 12.0);
  disc(binary, 49.0, 24.0, 12.0);

  imImage* separated = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imProcessWatershedSegment(binary, separated, 8, 0, &regions) != 0);
  REQUIRE(regions == 2);

  /* Nothing outside the binary object is labelled. The flood runs over the
     whole frame -- the background has distance zero and so floods last -- and
     is masked afterwards, so this is the assertion that the mask happened. */
  const imbyte* mask = (const imbyte*)binary->data[0];
  for (int i = 0; i < separated->count; i++)
  {
    if (!mask[i])
      CHECK((int)((imushort*)separated->data[0])[i] == 0);
  }

  /* With mark_lines off, every foreground pixel gets one. */
  for (int i = 0; i < separated->count; i++)
  {
    if (mask[i])
      CHECK((int)((imushort*)separated->data[0])[i] != 0);
  }

  /* The two discs are the same size, so the areas should be close to equal.
     A split that wandered off the neck -- which is what losing the queue's
     FIFO tie-break does on the distance transform's flat plateau -- shows up
     here as a lopsided pair while every structural check above still passes. */
  int area[3] = { 0, 0, 0 };
  for (int i = 0; i < separated->count; i++)
  {
    int label = (int)((imushort*)separated->data[0])[i];
    if (label >= 1 && label <= 2)
      area[label]++;
  }

  CHECK(area[1] > 0);
  CHECK(area[2] > 0);

  double ratio = (double)area[1] / (double)area[2];
  CHECK(ratio > 0.8);
  CHECK(ratio < 1.25);

  imImageDestroy(binary);
  imImageDestroy(separated);
}

TEST_CASE("WatershedSegment: mark_lines leaves a gap and never abuts two labels")
{
  const int W = 80, H = 48;

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  disc(binary, 31.0, 24.0, 12.0);
  disc(binary, 49.0, 24.0, 12.0);

  imImage* separated = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imProcessWatershedSegment(binary, separated, 8, 1, &regions) != 0);
  REQUIRE(regions == 2);

  /* The defining property of a watershed line: no pixel of one basin is
     4-adjacent to a pixel of another. If that fails the line has a hole in
     it, and the two objects are joined again by a one-pixel bridge -- which
     a later imAnalyzeFindRegions would merge back into one region, silently
     undoing the whole operation. */
  const imushort* data = (const imushort*)separated->data[0];

  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      int here = (int)data[y * W + x];
      if (!here)
        continue;

      if (x + 1 < W)
      {
        int right = (int)data[y * W + x + 1];
        if (right) CHECK(right == here);
      }
      if (y + 1 < H)
      {
        int below = (int)data[(y + 1) * W + x];
        if (below) CHECK(below == here);
      }
    }
  }

  /* And there really is a line: some foreground pixel is unlabelled. */
  const imbyte* mask = (const imbyte*)binary->data[0];
  int line_pixels = 0;
  for (int i = 0; i < separated->count; i++)
  {
    if (mask[i] && !data[i])
      line_pixels++;
  }
  CHECK(line_pixels > 0);

  imImageDestroy(binary);
  imImageDestroy(separated);
}

TEST_CASE("WatershedSegment: relabelling the result recovers the object count")
{
  /* The practical consequence of the case above, and the reason anyone wants
     mark_lines: once the objects are separated by a gap, the ordinary region
     labeller finds them. This is the whole pipeline, end to end. */
  const int W = 80, H = 48;

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  memset(binary->data[0], 0, (size_t)binary->count);

  disc(binary, 31.0, 24.0, 12.0);
  disc(binary, 49.0, 24.0, 12.0);

  imImage* separated = create(W, H, IM_GRAY, IM_USHORT);
  int regions = 0;
  REQUIRE(imProcessWatershedSegment(binary, separated, 8, 1, &regions) != 0);

  /* Turn the labelled result back into a binary mask and re-find regions. */
  imImage* split_mask = create(W, H, IM_BINARY, IM_BYTE);
  for (int i = 0; i < split_mask->count; i++)
    ((imbyte*)split_mask->data[0])[i] = ((imushort*)separated->data[0])[i] ? 1 : 0;

  imImage* relabelled = create(W, H, IM_GRAY, IM_USHORT);
  int found = 0;
  REQUIRE(imAnalyzeFindRegions(split_mask, relabelled, 4, 1, &found) != 0);
  CHECK(found == 2);

  imImageDestroy(binary);
  imImageDestroy(separated);
  imImageDestroy(split_mask);
  imImageDestroy(relabelled);
}

TEST_CASE("Watershed: floods a hand-built relief from two markers")
{
  /* A valley either side of a ridge. Marker 1 sits in the left valley,
     marker 2 in the right; the flood must meet on the ridge. This exercises
     imProcessWatershed on its own, without the distance transform, so a bug
     in the flooding is not masked by one in the marker generation. */
  const int W = 21, H = 5;

  imImage* relief = create(W, H, IM_GRAY, IM_BYTE);
  imImage* markers = create(W, H, IM_GRAY, IM_USHORT);
  imImage* result = create(W, H, IM_GRAY, IM_USHORT);

  /* A V on each side: height grows toward the middle column. */
  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      int height = (x <= 10) ? x : (20 - x);
      ((imbyte*)relief->data[0])[y * W + x] = (imbyte)(height * 10);
    }
  }

  memset(markers->data[0], 0, (size_t)markers->count * sizeof(imushort));
  for (int y = 0; y < H; y++)
  {
    ((imushort*)markers->data[0])[y * W + 0] = 1;
    ((imushort*)markers->data[0])[y * W + 20] = 2;
  }

  REQUIRE(imProcessWatershed(relief, markers, result, 4, 0) != 0);

  /* Everything left of the ridge belongs to 1, everything right to 2. */
  CHECK(label_at(result, 3, 2) == 1);
  CHECK(label_at(result, 17, 2) == 2);
  CHECK(distinct_labels(result) == 2);

  /* Every pixel assigned, since mark_lines is off. */
  for (int i = 0; i < result->count; i++)
    CHECK((int)((imushort*)result->data[0])[i] != 0);

  imImageDestroy(relief);
  imImageDestroy(markers);
  imImageDestroy(result);
}

TEST_CASE("Watershed: an unmarked region is never labelled")
{
  /* The flood segments the markers it is given; it does not go looking for
     more. A relief with three basins and two markers must come back with two
     labels, not three -- a version that seeded itself from regional minima
     would quietly produce a third. */
  const int W = 30, H = 6;

  imImage* relief = create(W, H, IM_GRAY, IM_BYTE);
  imImage* markers = create(W, H, IM_GRAY, IM_USHORT);
  imImage* result = create(W, H, IM_GRAY, IM_USHORT);

  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      ((imbyte*)relief->data[0])[y * W + x] = (imbyte)((x % 10) * 10);

  memset(markers->data[0], 0, (size_t)markers->count * sizeof(imushort));
  ((imushort*)markers->data[0])[2 * W + 0] = 1;
  ((imushort*)markers->data[0])[2 * W + 10] = 2;

  REQUIRE(imProcessWatershed(relief, markers, result, 8, 0) != 0);

  CHECK(distinct_labels(result) == 2);

  imImageDestroy(relief);
  imImageDestroy(markers);
  imImageDestroy(result);
}

TEST_CASE("Watershed: writing into the marker image is allowed")
{
  const int W = 21, H = 5;

  imImage* relief = create(W, H, IM_GRAY, IM_BYTE);
  imImage* markers = create(W, H, IM_GRAY, IM_USHORT);
  imImage* separate = create(W, H, IM_GRAY, IM_USHORT);
  imImage* reference = create(W, H, IM_GRAY, IM_USHORT);

  for (int y = 0; y < H; y++)
  {
    for (int x = 0; x < W; x++)
    {
      int height = (x <= 10) ? x : (20 - x);
      ((imbyte*)relief->data[0])[y * W + x] = (imbyte)(height * 10);
    }
  }

  memset(markers->data[0], 0, (size_t)markers->count * sizeof(imushort));
  memset(separate->data[0], 0, (size_t)separate->count * sizeof(imushort));
  for (int y = 0; y < H; y++)
  {
    ((imushort*)markers->data[0])[y * W + 0] = 1;
    ((imushort*)markers->data[0])[y * W + 20] = 2;
    ((imushort*)separate->data[0])[y * W + 0] = 1;
    ((imushort*)separate->data[0])[y * W + 20] = 2;
  }

  REQUIRE(imProcessWatershed(relief, separate, reference, 4, 1) != 0);
  REQUIRE(imProcessWatershed(relief, markers, markers, 4, 1) != 0);

  for (int i = 0; i < markers->count; i++)
    CHECK((int)((imushort*)markers->data[0])[i] == (int)((imushort*)reference->data[0])[i]);

  imImageDestroy(relief);
  imImageDestroy(markers);
  imImageDestroy(separate);
  imImageDestroy(reference);
}

TEST_CASE("Watershed: reads every supported relief type the same way")
{
  const int W = 21, H = 5;

  const int types[] = { IM_BYTE, IM_SHORT, IM_USHORT, IM_INT, IM_FLOAT, IM_DOUBLE };

  imImage* reference = NULL;

  for (int t = 0; t < 6; t++)
  {
    imImage* relief = create(W, H, IM_GRAY, types[t]);
    imImage* markers = create(W, H, IM_GRAY, IM_USHORT);
    imImage* result = create(W, H, IM_GRAY, IM_USHORT);

    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        double height = ((x <= 10) ? x : (20 - x)) * 10.0;
        int i = y * W + x;

        switch (types[t])
        {
        case IM_BYTE:   ((imbyte*)relief->data[0])[i] = (imbyte)height;     break;
        case IM_SHORT:  ((short*)relief->data[0])[i] = (short)height;       break;
        case IM_USHORT: ((imushort*)relief->data[0])[i] = (imushort)height; break;
        case IM_INT:    ((int*)relief->data[0])[i] = (int)height;           break;
        case IM_FLOAT:  ((float*)relief->data[0])[i] = (float)height;       break;
        case IM_DOUBLE: ((double*)relief->data[0])[i] = height;             break;
        }
      }
    }

    memset(markers->data[0], 0, (size_t)markers->count * sizeof(imushort));
    for (int y = 0; y < H; y++)
    {
      ((imushort*)markers->data[0])[y * W + 0] = 1;
      ((imushort*)markers->data[0])[y * W + 20] = 2;
    }

    REQUIRE(imProcessWatershed(relief, markers, result, 4, 1) != 0);

    if (t == 0)
    {
      reference = create(W, H, IM_GRAY, IM_USHORT);
      memcpy(reference->data[0], result->data[0], (size_t)result->count * sizeof(imushort));
    }
    else
    {
      /* The same relief expressed in a different type must segment
         identically. A missing case in the switch would leave the
         destination untouched and this would catch it. */
      for (int i = 0; i < result->count; i++)
        CHECK((int)((imushort*)result->data[0])[i] == (int)((imushort*)reference->data[0])[i]);
    }

    imImageDestroy(relief);
    imImageDestroy(markers);
    imImageDestroy(result);
  }

  imImageDestroy(reference);
}

#ifdef NDEBUG
TEST_CASE("Watershed: checks its arguments")
{
  const int W = 16, H = 12;

  imImage* relief = create(W, H, IM_GRAY, IM_BYTE);
  imImage* markers = create(W, H, IM_GRAY, IM_USHORT);
  imImage* result = create(W, H, IM_GRAY, IM_USHORT);

  imImage* small = create(W / 2, H, IM_GRAY, IM_USHORT);
  CHECK(imProcessWatershed(relief, small, result, 8, 0) == 0);
  CHECK(imProcessWatershed(relief, markers, small, 8, 0) == 0);
  imImageDestroy(small);

  imImage* wrong_type = create(W, H, IM_GRAY, IM_INT);
  CHECK(imProcessWatershed(relief, wrong_type, result, 8, 0) == 0);
  CHECK(imProcessWatershed(relief, markers, wrong_type, 8, 0) == 0);
  imImageDestroy(wrong_type);

  CHECK(imProcessWatershed(relief, markers, result, 6, 0) == 0);

  imImage* binary = create(W, H, IM_BINARY, IM_BYTE);
  int regions = 0;
  CHECK(imProcessWatershedSegment(relief, result, 8, 0, &regions) == 0);  /* not binary */
  CHECK(imProcessWatershedSegment(binary, relief, 8, 0, &regions) == 0);  /* not ushort */
  CHECK(imProcessWatershedSegment(binary, result, 5, 0, &regions) == 0);  /* not 4 or 8 */
  imImageDestroy(binary);

  imImageDestroy(relief);
  imImageDestroy(markers);
  imImageDestroy(result);
}
#endif
