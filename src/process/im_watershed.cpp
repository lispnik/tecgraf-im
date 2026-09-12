/** \file
 * \brief Watershed Segmentation
 *
 * See Copyright Notice in im_lib.h
 */

#include <im.h>
#include <im_util.h>
#include <im_image.h>

#include "im_process_counter.h"
#include "im_process_check.h"
#include "im_process_glo.h"
#include "im_process_ana.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* Meyer's flooding watershed.
 *
 * Read the source image as a relief map and flood it from a set of marked
 * sources, lowest ground first. Every pixel ends up belonging to the marker
 * whose water reached it, and the lines where two floods meet are the
 * watershed.
 *
 * The reason this is worth having is that the library already ships both
 * halves of the classic recipe for separating touching objects and stops one
 * function short of joining them up. imProcessDistanceTransform gives a
 * distance map whose peaks are the centres of the objects;
 * imProcessRegionalMaximum finds those peaks. Flooding the NEGATED distance
 * map from those peaks splits the blob along the neck between the two
 * centres -- which is what imProcessWatershedSegment below does, and what
 * imAnalyzeFindRegions on its own cannot do, because two touching objects are
 * one connected region and no amount of labelling will make them two.
 *
 * MARKER-CONTROLLED, DELIBERATELY. A watershed seeded from every regional
 * minimum of the relief instead of from given markers is the textbook
 * presentation and is nearly useless on real data: noise puts a minimum every
 * few pixels and the result is thousands of tiny basins. Every practical use
 * supplies markers, so markers are the interface rather than an option.
 *
 * The priority queue is a binary heap keyed on the relief value, with the
 * insertion sequence as a tie-break. The tie-break is not decoration. A
 * distance transform of a disc is flat across its whole plateau, so a large
 * fraction of all pixels compare equal, and processing them in an arbitrary
 * order makes the boundary between two basins depend on heap ordering
 * accidents. First-in-first-out among equals is what makes the flood advance
 * as a front and puts the line midway along the neck.
 */

#define IM_WS_UNPROCESSED 0
#define IM_WS_QUEUED      1
#define IM_WS_DONE        2

/* The flood is serial -- a priority queue has no parallel form worth the
   contention at this scale -- so the counter is incremented directly instead
   of through IM_COUNT_PROCESSING.

   That is not a stylistic choice. IM_COUNT_PROCESSING opens a brace that
   IM_END_PROCESSING closes, so the two have to be lexically adjacent at the
   same nesting level; putting the count inside `if (popped % width == 0)'
   leaves the closing braces outside the conditional and the file does not
   compile under OpenMP -- while compiling perfectly well without it, which is
   the configuration most people build. Counting once per pixel instead would
   pair correctly and make the progress callback fire width times more often
   than every other operation in the library. */
#ifdef _OPENMP
#define IM_WS_COUNTER_INC(_c) imCounterInc_OMP(_c)
#else
#define IM_WS_COUNTER_INC(_c) imCounterInc(_c)
#endif

typedef struct _imWatershedEntry
{
  double value;
  int order;
  int index;
} imWatershedEntry;

typedef struct _imWatershedHeap
{
  imWatershedEntry* data;
  int count;
  int capacity;
  int sequence;
} imWatershedHeap;

static int iHeapLess(const imWatershedEntry* a, const imWatershedEntry* b)
{
  if (a->value < b->value) return 1;
  if (a->value > b->value) return 0;
  return a->order < b->order;
}

static void iHeapPush(imWatershedHeap* heap, double value, int index)
{
  /* Capacity is width*height and every pixel is pushed at most once, guarded
     by the state array in the caller, so this cannot overflow. The test is
     here because "cannot" is a claim about code elsewhere in the file. */
  if (heap->count >= heap->capacity)
    return;

  int i = heap->count++;

  heap->data[i].value = value;
  heap->data[i].order = heap->sequence++;
  heap->data[i].index = index;

  while (i > 0)
  {
    int parent = (i - 1) / 2;
    if (!iHeapLess(&heap->data[i], &heap->data[parent]))
      break;

    imWatershedEntry swap = heap->data[i];
    heap->data[i] = heap->data[parent];
    heap->data[parent] = swap;

    i = parent;
  }
}

static int iHeapPop(imWatershedHeap* heap)
{
  int index = heap->data[0].index;

  heap->data[0] = heap->data[--heap->count];

  int i = 0;
  for (;;)
  {
    int left = 2 * i + 1;
    int right = left + 1;
    int smallest = i;

    if (left < heap->count && iHeapLess(&heap->data[left], &heap->data[smallest]))
      smallest = left;
    if (right < heap->count && iHeapLess(&heap->data[right], &heap->data[smallest]))
      smallest = right;

    if (smallest == i)
      break;

    imWatershedEntry swap = heap->data[i];
    heap->data[i] = heap->data[smallest];
    heap->data[smallest] = swap;

    i = smallest;
  }

  return index;
}

/* The 8 neighbour offsets, 4-connected ones first so that a connect of 4
   takes the first four and a connect of 8 takes all of them. */
static const int iWatershedDX[8] = { 0,  0, -1,  1, -1,  1, -1,  1 };
static const int iWatershedDY[8] = { -1, 1,  0,  0, -1, -1,  1,  1 };

template<class T>
static int DoWatershed(const T* src_data, const imushort* marker_data, imushort* dst_data,
                       int width, int height, int connect, int mark_lines, int counter)
{
  int count = width * height;
  int neighbours = (connect == 4) ? 4 : 8;

  imbyte* state = (imbyte*)malloc(count * sizeof(imbyte));
  imWatershedEntry* entries = (imWatershedEntry*)malloc(count * sizeof(imWatershedEntry));

  if (!state || !entries)
  {
    if (state) free(state);
    if (entries) free(entries);
    return IM_PROCESS_ABORT;
  }

  imWatershedHeap heap;
  heap.data = entries;
  heap.count = 0;
  heap.capacity = count;
  heap.sequence = 0;

  memset(state, IM_WS_UNPROCESSED, count * sizeof(imbyte));

  /* Seed: the markers are already final, and every unlabelled pixel next to
     one goes into the queue at its own relief height. */
  for (int i = 0; i < count; i++)
  {
    dst_data[i] = marker_data[i];

    if (marker_data[i])
      state[i] = IM_WS_DONE;
  }

  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x++)
    {
      int offset = y * width + x;

      if (state[offset] != IM_WS_DONE)
        continue;

      for (int n = 0; n < neighbours; n++)
      {
        int nx = x + iWatershedDX[n];
        int ny = y + iWatershedDY[n];

        if (nx < 0 || nx >= width || ny < 0 || ny >= height)
          continue;

        int noffset = ny * width + nx;

        if (state[noffset] == IM_WS_UNPROCESSED)
        {
          state[noffset] = IM_WS_QUEUED;
          iHeapPush(&heap, (double)src_data[noffset], noffset);
        }
      }
    }
  }

  IM_INT_PROCESSING;

  int popped = 0;

  while (heap.count > 0)
  {
    int offset = iHeapPop(&heap);

    int x = offset % width;
    int y = offset / width;

    /* Which labels does this pixel already touch? Only settled neighbours
       count, and a settled neighbour carrying label 0 is a watershed pixel,
       which by definition propagates nothing. */
    imushort label = 0;
    int is_watershed = 0;

    for (int n = 0; n < neighbours; n++)
    {
      int nx = x + iWatershedDX[n];
      int ny = y + iWatershedDY[n];

      if (nx < 0 || nx >= width || ny < 0 || ny >= height)
        continue;

      int noffset = ny * width + nx;

      if (state[noffset] != IM_WS_DONE)
        continue;

      imushort neighbour_label = dst_data[noffset];
      if (neighbour_label == 0)
        continue;

      if (label == 0)
        label = neighbour_label;
      else if (label != neighbour_label)
      {
        is_watershed = 1;
        break;
      }
    }

    if (is_watershed && mark_lines)
    {
      dst_data[offset] = 0;
      state[offset] = IM_WS_DONE;
    }
    else
    {
      /* Without lines the pixel joins whichever basin reached it first,
         which is the lower of the two -- the heap ordering decided that
         before this pixel was ever popped. */
      dst_data[offset] = label;
      state[offset] = IM_WS_DONE;

      for (int n = 0; n < neighbours; n++)
      {
        int nx = x + iWatershedDX[n];
        int ny = y + iWatershedDY[n];

        if (nx < 0 || nx >= width || ny < 0 || ny >= height)
          continue;

        int noffset = ny * width + nx;

        if (state[noffset] == IM_WS_UNPROCESSED)
        {
          state[noffset] = IM_WS_QUEUED;
          iHeapPush(&heap, (double)src_data[noffset], noffset);
        }
      }
    }

    popped++;
    if (popped % width == 0)
    {
      if (!IM_WS_COUNTER_INC(counter))
      {
        processing = IM_PROCESS_ABORT;
        break;
      }
    }
  }

  free(state);
  free(entries);

  return processing;
}

int imProcessWatershed(const imImage* src_image, const imImage* marker_image, imImage* dst_image,
                       int connect, int mark_lines)
{
  assert(imCheckSameSize(src_image, marker_image) && imCheckSameSize(src_image, dst_image));
  if (!imCheckSameSize(src_image, marker_image) || !imCheckSameSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(src_image->depth == 1);
  if (src_image->depth != 1)
    return IM_PROCESS_ABORT;

  assert(marker_image->data_type == IM_USHORT && dst_image->data_type == IM_USHORT);
  if (marker_image->data_type != IM_USHORT || dst_image->data_type != IM_USHORT)
    return IM_PROCESS_ABORT;

  assert(connect == 4 || connect == 8);
  if (connect != 4 && connect != 8)
    return IM_PROCESS_ABORT;

  int ret;

  int counter = imProcessCounterBegin("Watershed");
  imCounterTotal(counter, src_image->height, "Processing...");

  const imushort* marker_data = (const imushort*)marker_image->data[0];
  imushort* dst_data = (imushort*)dst_image->data[0];

  switch (src_image->data_type)
  {
  case IM_BYTE:
    ret = DoWatershed((const imbyte*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  case IM_SHORT:
    ret = DoWatershed((const short*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  case IM_USHORT:
    ret = DoWatershed((const imushort*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  case IM_INT:
    ret = DoWatershed((const int*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  case IM_FLOAT:
    ret = DoWatershed((const float*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  case IM_DOUBLE:
    ret = DoWatershed((const double*)src_image->data[0], marker_data, dst_data,
                      src_image->width, src_image->height, connect, mark_lines, counter);
    break;
  default:
    ret = IM_PROCESS_ABORT;
    break;
  }

  imProcessCounterEnd(counter);

  return ret;
}

int imProcessWatershedSegment(const imImage* src_image, imImage* dst_image, int connect,
                              int mark_lines, int* region_count)
{
  assert(imCheckSameSize(src_image, dst_image));
  if (!imCheckSameSize(src_image, dst_image))
    return IM_PROCESS_ABORT;

  assert(src_image->color_space == IM_BINARY);
  if (src_image->color_space != IM_BINARY)
    return IM_PROCESS_ABORT;

  assert(dst_image->data_type == IM_USHORT);
  if (dst_image->data_type != IM_USHORT)
    return IM_PROCESS_ABORT;

  assert(connect == 4 || connect == 8);
  if (connect != 4 && connect != 8)
    return IM_PROCESS_ABORT;

  int width = src_image->width, height = src_image->height;
  int count = width * height;
  int ret = IM_PROCESS_ABORT;

  imImage* distance = imImageCreate(width, height, IM_GRAY, IM_FLOAT);
  imImage* maxima = imImageCreate(width, height, IM_BINARY, IM_BYTE);
  imImage* markers = imImageCreate(width, height, IM_GRAY, IM_USHORT);
  imImage* relief = imImageCreate(width, height, IM_GRAY, IM_FLOAT);

  if (!distance || !maxima || !markers || !relief)
    goto cleanup;

  imProcessDistanceTransform(src_image, distance);
  imProcessRegionalMaximum(distance, maxima);

  /* The maxima are the seeds, and each connected clump of them is one object.
     touch_border is 1: a cell at the edge of the frame is still a cell, and
     excluding it here would silently merge it into its neighbour rather than
     drop it. */
  if (!imAnalyzeFindRegions(maxima, markers, connect, 1, region_count))
    goto cleanup;

  /* Flood the negated distance map: the peaks of the distance transform,
     which are the object centres, become its basins.

     The background has distance zero and so relief zero, the highest value
     anywhere, which means it floods last -- every foreground pixel is settled
     before the first background pixel is popped. That is what lets this run
     over the whole frame and mask afterwards, instead of needing the flood
     itself to know where the object ends. */
  {
    const float* distance_data = (const float*)distance->data[0];
    float* relief_data = (float*)relief->data[0];

    for (int i = 0; i < count; i++)
      relief_data[i] = -distance_data[i];
  }

  if (!imProcessWatershed(relief, markers, dst_image, connect, mark_lines))
    goto cleanup;

  {
    const imbyte* src_data = (const imbyte*)src_image->data[0];
    imushort* dst_data = (imushort*)dst_image->data[0];

    for (int i = 0; i < count; i++)
    {
      if (!src_data[i])
        dst_data[i] = 0;
    }
  }

  ret = IM_PROCESS_OK;

cleanup:
  if (distance) imImageDestroy(distance);
  if (maxima) imImageDestroy(maxima);
  if (markers) imImageDestroy(markers);
  if (relief) imImageDestroy(relief);

  return ret;
}
