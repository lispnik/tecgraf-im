/** \file
 * \brief Binary Data Utilities
 *
 * See Copyright Notice in im_lib.h
 */

#include <assert.h>
#include <stddef.h>

#include "im_util.h"


int imBinCPUByteOrder(void)
{
  /* Not cached. The probe is a handful of instructions, and a lazily
     initialised static here would be a data race between concurrent callers
     -- benign in practice, since they all compute the same value, but a real
     one that a thread sanitizer reports. */
  unsigned short w = 0x0001;
  unsigned char* b = (unsigned char*)&w;

  return (b[0] == 0x01)? IM_LITTLEENDIAN: IM_BIGENDIAN;
}

void imBinSwapBytes(void *data, int count, int size)
{
  assert(data);

  /* assert() is compiled out of every shipped build -- all the CI jobs use
     Release or RelWithDebInfo -- so the guards below have to be real. */
  if (!data || count <= 0)
    return;

  switch(size)
  {
  case 1:
    /* A single byte has no order. Previously this fell through the switch to
       the same no-op, but by accident rather than by statement. */
    break;

  case 2:
    imBinSwapBytes2(data, count);
    break;

  case 4:
    imBinSwapBytes4(data, count);
    break;

  case 8:
    imBinSwapBytes8(data, count);
    break;

  case 16:
    {
      /* The only 16-byte type this library defines is IM_CDOUBLE: two doubles
         in sequence. Each half is swapped independently -- reversing all 16
         bytes would byte-swap correctly but transpose the real and imaginary
         parts. Before this case existed, imDataTypeSize(IM_CDOUBLE) fell
         through to a silent no-op and left the data in the file's order.

         Stepping element by element rather than calling
         imBinSwapBytes8(data, count*2) keeps the doubled count from
         overflowing int. */
      unsigned char* values = (unsigned char*)data;
      for (int i = 0; i < count; i++, values += 16)
        imBinSwapBytes8(values, 2);
    }
    break;

  default:
    /* Not a scalar width: there is no meaningful swap, and the signature has
       no way to report an error. Leaving the data untouched is the only safe
       answer, but it is a programming error, so say so where it can be heard.

       Note IM_CFLOAT is deliberately not special-cased. It is 8 bytes, and so
       is a double; element size alone cannot tell them apart, and doubles are
       far more common. Callers with complex data pass the size of one real
       component and twice the count -- see "treat complex as 2 real" in
       src/im_format_raw.cpp. */
    assert(0 && "imBinSwapBytes: size must be 1, 2, 4, 8 or 16");
    break;
  }
}

void imBinSwapBytes2(void *data, int count)
{
  assert(data);

  /* Return before the loop rather than relying on its condition: `count--`
     would still evaluate, and decrementing INT_MIN is itself signed overflow
     -- undefined behaviour that UBSan reports even though no swap happens. */
  if (!data || count <= 0)
    return;

  unsigned char lTemp;
  unsigned char *values = (unsigned char *)data;

  /* '> 0', not '!= 0': a negative count never reaches zero on the way down,
     so the original loop ran to integer wraparound -- some two billion
     iterations of writes marching off the end of the buffer. A negative count
     arrives here whenever imBinFile::Read/Write truncates its unsigned long
     pCount into this int parameter. */
  while (count-- > 0)
  {
    lTemp = values[1];
    values[1] = values[0];
    values[0] = lTemp;

    values += 2;
  }
}

void imBinSwapBytes4(void *data, int count)
{
  assert(data);

  /* Return before the loop rather than relying on its condition: `count--`
     would still evaluate, and decrementing INT_MIN is itself signed overflow
     -- undefined behaviour that UBSan reports even though no swap happens. */
  if (!data || count <= 0)
    return;

  unsigned char lTemp;
  unsigned char *values = (unsigned char *)data;

  while (count-- > 0)
  {
    lTemp = values[3];
    values[3] = values[0];
    values[0] = lTemp;

    lTemp = values[2];
    values[2] = values[1];
    values[1] = lTemp;

    values += 4;
  }
}

void imBinSwapBytes8(void *data, int count)
{
  assert(data);

  /* Return before the loop rather than relying on its condition: `count--`
     would still evaluate, and decrementing INT_MIN is itself signed overflow
     -- undefined behaviour that UBSan reports even though no swap happens. */
  if (!data || count <= 0)
    return;

  unsigned char lTemp;
  unsigned char *values = (unsigned char *)data;

  while (count-- > 0)
  {
    lTemp = values[7];
    values[7] = values[0];
    values[0] = lTemp;

    lTemp = values[6];
    values[6] = values[1];
    values[1] = lTemp;

    lTemp = values[5];
    values[5] = values[2];
    values[2] = lTemp;

    lTemp = values[4];
    values[4] = values[3];
    values[3] = lTemp;

    values += 8;
  }
}
