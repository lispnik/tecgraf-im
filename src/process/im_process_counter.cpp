/** \file
 * \brief Processing Counter
 *
 * See Copyright Notice in im_lib.h
 */

#include "im_process_counter.h"

#include <stdlib.h>
#include <memory.h>


int im_process_mincount = 250000;   /* 500*500 image size */

int imProcessOpenMPSetMinCount(int min_count)
{
  int old_imin_count = im_process_mincount;
  im_process_mincount = min_count;
  return old_imin_count;
}

int imProcessOpenMPSetNumThreads(int count)
{
#ifdef _OPENMP
  int old_count = omp_get_num_threads();
  omp_set_num_threads(count);
  return old_count;
#else
  (void)count;
  return 1;
#endif
}

#ifdef _OPENMP

/* One lock for the whole counter API, rather than an omp_lock_t per counter.
 *
 * The old lock was allocated by a Begin wrapper and stored in the counter's
 * own user data, which is not this layer's to use: imCounterSetUserData is
 * public API, so a consumer setting user data on a counter overwrote the lock
 * pointer, and the End wrapper cleared whatever the consumer had set.
 *
 * Worse, it made Begin and End a matched pair that nothing enforced. A counter
 * begun with imCounterBegin and ended with imProcessCounterEnd called
 * omp_destroy_lock on a lock that was never allocated, and the process died at
 * address 0 -- only in the OpenMP build, and only with a callback attached,
 * because both wrappers returned early without one. imAnalyzeFindRegions and
 * imProcessCanny did exactly that, so a progress callback was fatal in any
 * consumer that attached one, while the same call without one worked.
 *
 * One lock for all of them has no per-counter state to mismatch, and is the
 * stronger guarantee besides: iCounterFunc is a single global function, and a
 * lock per counter let two parallel regions be inside it at once.
 *
 * NESTABLE, and that is the point of not writing `#pragma omp critical' here.
 * imCounterInc calls the user's callback while the lock is held, and a
 * callback is free to start another IM operation -- redrawing a preview is the
 * obvious reason to. Under the old per-counter locks that nested call took a
 * different lock and proceeded; under a plain critical section or a plain
 * omp_lock_t it would deadlock against itself. A nest lock may be re-entered
 * by its owner, so the nested operation behaves as it did before while other
 * threads still wait. */
namespace
{
  struct CounterLock
  {
    omp_nest_lock_t lock;
    CounterLock()  { omp_init_nest_lock(&lock); }
    ~CounterLock() { omp_destroy_nest_lock(&lock); }
  };

  /* Function-local so initialization is ordered by first use rather than by
     link order, and thread-safe by C++11 magic statics -- the first call can
     come from inside a parallel region. */
  omp_nest_lock_t* CounterLockInstance()
  {
    static CounterLock instance;
    return &instance.lock;
  }
}

int imCounterInc_OMP(int counter)
{
  int processing;

  if (counter == -1 || !imCounterHasCallback()) 
    return 1;

  omp_nest_lock_t* lck = CounterLockInstance();

  omp_set_nest_lock(lck);
  processing = imCounterInc(counter);
  omp_unset_nest_lock(lck);

  return processing;
}

#endif
