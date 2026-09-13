/* The progress counter, against libim_process_omp.
 *
 * A separate binary from im_tests, which links the plain libim_process on
 * purpose (see the note in CMakeLists.txt). Everything here is invisible from
 * there: the two libraries export identical symbol sets and differ only in
 * whether _OPENMP was defined, and the bug this file exists for lived entirely
 * in the OpenMP half.
 *
 * That bug: the OpenMP counter layer allocated an omp_lock_t in a Begin
 * wrapper and stored it in the counter's own user data. Any function that
 * began a counter with imCounterBegin and ended it with imProcessCounterEnd
 * therefore freed a lock that was never allocated, and any function that
 * incremented one that way dereferenced NULL. imAnalyzeFindRegions and
 * imProcessCanny both did. Neither could be seen without a callback attached,
 * because both wrappers returned early when there was none -- so the whole
 * class of failure only appeared in a consumer that reported progress, which
 * is the one thing no test here had ever done.
 *
 * The counter is now a named critical section with no per-counter state, so
 * the mismatch is not expressible. These cases are what says so.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_counter.h>
#include <im_process.h>

#include <string.h>

namespace {

int callback_count = 0;
void* callback_user_data_seen = nullptr;

int CountingCallback(int counter, void* user_data, const char* text, int progress)
{
  (void)counter;
  (void)text;
  (void)progress;
  callback_count++;
  callback_user_data_seen = user_data;
  return 1;
}

imImage* nested_src = nullptr;
imImage* nested_dst = nullptr;
int nested_ran = 0;

/* Runs one further IM operation, from inside the callback, exactly once. The
   nested operation reports progress of its own, so it reaches imCounterInc
   while the outer call already holds the lock. */
int ReentrantCallback(int counter, void* user_data, const char* text, int progress)
{
  (void)counter;
  (void)user_data;
  (void)text;
  (void)progress;

  if (!nested_ran && nested_src && nested_dst)
  {
    nested_ran = 1;
    imProcessGaussianConvolve(nested_src, nested_dst, 1.0);
  }

  return 1;
}

int user_data_marker = 0;
void* user_data_readback = nullptr;
bool user_data_checked = false;

/* Writes the caller's own pointer into the counter's user data on the opening
   call, and reads it back on a later one. */
int StashingCallback(int counter, void* user_data, const char* text, int progress)
{
  (void)user_data;
  (void)text;

  if (progress < 0)
  {
    imCounterSetUserData(counter, &user_data_marker);
  }
  else if (!user_data_checked)
  {
    user_data_readback = imCounterGetUserData(counter);
    user_data_checked = true;
  }

  return 1;
}

struct WithCallback
{
  WithCallback()
  {
    callback_count = 0;
    callback_user_data_seen = nullptr;
    imCounterSetCallback(nullptr, CountingCallback);
  }
  ~WithCallback()
  {
    imCounterSetCallback(nullptr, nullptr);
  }
};

/* A gray byte image with a bright square in the middle: enough of an edge for
   Canny to find, and enough of a region to label. */
imImage* SquareImage(int size = 64)
{
  imImage* image = imImageCreate(size, size, IM_GRAY, IM_BYTE);
  REQUIRE(image != nullptr);

  imbyte* data = (imbyte*)image->data[0];
  memset(data, 0, image->count);

  for (int y = size / 4; y < 3 * size / 4; y++)
    for (int x = size / 4; x < 3 * size / 4; x++)
      data[y * size + x] = 255;

  return image;
}

imImage* SquareBinary(int size = 64)
{
  imImage* image = imImageCreate(size, size, IM_BINARY, IM_BYTE);
  REQUIRE(image != nullptr);

  imbyte* data = (imbyte*)image->data[0];
  memset(data, 0, image->count);

  for (int y = size / 4; y < 3 * size / 4; y++)
    for (int x = size / 4; x < 3 * size / 4; x++)
      data[y * size + x] = 1;

  return image;
}

} /* namespace */

TEST_CASE("Canny runs with a progress callback attached")
{
  WithCallback attached;

  imImage* src = SquareImage();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_BYTE);
  REQUIRE(dst != nullptr);

  CHECK(imProcessCanny(src, dst, 1.4) == 1);
  CHECK(callback_count > 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("FindRegions runs with a progress callback attached")
{
  WithCallback attached;

  imImage* src = SquareBinary();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_USHORT);
  REQUIRE(dst != nullptr);

  int region_count = 0;
  CHECK(imAnalyzeFindRegions(src, dst, 8, 1, &region_count) == 1);
  CHECK(region_count == 1);
  CHECK(callback_count > 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("WatershedSegment runs with a progress callback attached")
{
  /* Reaches imAnalyzeFindRegions to group its markers, so it carried the same
     failure without having written it. */
  WithCallback attached;

  imImage* src = SquareBinary();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_USHORT);
  REQUIRE(dst != nullptr);

  int region_count = 0;
  CHECK(imProcessWatershedSegment(src, dst, 8, 0, &region_count) == 1);
  CHECK(region_count >= 1);
  CHECK(callback_count > 0);

  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("A counter's user data belongs to the caller")
{
  /* The other half of the same fix. imCounterSetUserData is public, and the
     OpenMP layer used to keep its lock there: a caller setting user data on a
     counter overwrote a live omp_lock_t*, and the End wrapper cleared whatever
     the caller had set. Nothing in libim_process may touch it now.

     The callback is the only place with a counter id to hand, which is why the
     round trip happens inside it. */
  user_data_marker = 0;
  user_data_readback = nullptr;
  user_data_checked = false;
  imCounterSetCallback(nullptr, StashingCallback);

  imImage* src = SquareImage();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_BYTE);
  REQUIRE(dst != nullptr);

  CHECK(imProcessGaussianConvolve(src, dst, 1.0) == 1);
  CHECK(user_data_checked);
  CHECK(user_data_readback == &user_data_marker);

  imCounterSetCallback(nullptr, nullptr);
  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("The callback's own user data is passed through")
{
  int payload = 0;
  callback_count = 0;
  callback_user_data_seen = nullptr;
  imCounterSetCallback(&payload, CountingCallback);

  imImage* src = SquareImage();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_BYTE);
  REQUIRE(dst != nullptr);

  CHECK(imProcessGaussianConvolve(src, dst, 1.0) == 1);
  CHECK(callback_count > 0);
  CHECK(callback_user_data_seen == &payload);

  imCounterSetCallback(nullptr, nullptr);
  imImageDestroy(src);
  imImageDestroy(dst);
}

TEST_CASE("a callback may start another operation while the counter is held")
{
  /* imCounterInc calls the callback with the counter lock held, and a callback
     is free to run another IM operation -- redrawing a preview is the obvious
     reason to. The old per-counter locks allowed it because the nested call
     took a different one. A single plain lock, or `#pragma omp critical', would
     deadlock against itself here; the lock is nestable so that it does not.

     A regression shows up as a hang rather than a failure, which is why this
     binary's CTest entries carry a timeout. */
  imImage* src = SquareImage();
  imImage* dst = imImageCreate(src->width, src->height, IM_GRAY, IM_BYTE);
  REQUIRE(dst != nullptr);

  nested_src = src;
  nested_dst = dst;
  nested_ran = 0;

  imImage* outer_src = SquareImage();
  imImage* outer_dst = imImageCreate(outer_src->width, outer_src->height, IM_GRAY, IM_BYTE);
  REQUIRE(outer_dst != nullptr);

  imCounterSetCallback(nullptr, ReentrantCallback);
  CHECK(imProcessGaussianConvolve(outer_src, outer_dst, 1.0) == 1);
  imCounterSetCallback(nullptr, nullptr);

  CHECK(nested_ran == 1);

  imImageDestroy(src);
  imImageDestroy(dst);
  imImageDestroy(outer_src);
  imImageDestroy(outer_dst);
}
