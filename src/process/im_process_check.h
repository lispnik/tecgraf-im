/** \file
 * \brief Shared precondition checks for the process operations
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_PROCESS_CHECK_H
#define __IM_PROCESS_CHECK_H

#include <im.h>
#include <im_image.h>

/* Most operations in this library write the destination through a pointer of
   the SOURCE's data type and loop over the SOURCE's dimensions, without ever
   consulting the destination. A destination that is narrower or smaller than
   the source therefore runs off the end of its buffer -- and every one of the
   headers states the rule ("images must be of the same type and size")
   without anything verifying it, which turns a documented misuse into a heap
   overflow rather than an error.

   Used as an assert followed by a real early return: the assert so a debug
   build fails at the point of the mistake, the return so a shipped build,
   where assert is compiled out, does not corrupt memory. That is the pattern
   the rest of the library uses for a precondition a void function cannot
   report, and what imProcessCompose has always done when handed an image with
   no alpha channel.

   Alpha is deliberately not compared. The operations that carry it already
   test both images -- "src->has_alpha && dst->has_alpha" -- and copy the
   extra plane only when both have one, so an alpha mismatch is handled rather
   than dangerous. Depth is compared, because a source with more planes than
   the destination indexes past the end of its data pointer array. */

/* Not every operation can use these two. A handful exist precisely to change
   the data type -- imProcessDirectConv and imProcessUnNormalize both write an
   IM_BYTE destination from a wider source -- so for those "the same type" is
   never true of a valid call and asserting it makes the function a no-op. Use
   imCheckSameSize with an explicit test of the destination type instead. */

static inline int imCheckSameSize(const imImage* a, const imImage* b)
{
  return a->depth == b->depth &&
         a->width == b->width && a->height == b->height;
}

static inline int imCheckSameType(const imImage* a, const imImage* b)
{
  return a->data_type == b->data_type && a->depth == b->depth;
}

static inline int imCheckSameTypeSize(const imImage* a, const imImage* b)
{
  return a->data_type == b->data_type && imCheckSameSize(a, b);
}

#endif
