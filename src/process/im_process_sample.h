/** \file
 * \brief Type-generic sample range and store-back
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_PROCESS_SAMPLE_H
#define __IM_PROCESS_SAMPLE_H

#include <im.h>

#include <math.h>

/* The operations added by this fork -- deconvolution, edge-preserving
   denoising, watershed -- all compute in double and then have to put the
   answer back into whatever type the destination is.

   The legacy sources spell that per data type at each call site, usually as
   (T)IM_BYTECROP(v) for IM_BYTE and a bare cast for everything else. The bare
   cast is wrong in two ways that matter here in a way they do not for a
   convolution: it truncates toward zero rather than rounding, and it is
   undefined behaviour when the value is outside the destination's range.
   Richardson-Lucy amplifies, so producing 300 from an IM_BYTE image is
   routine rather than exotic, and truncation of a converged result biases
   every sample down by half a level.

   Rounding is to nearest with halves away from zero, which is what floor(v +
   0.5) gives for positives and what the symmetric expression below gives for
   negatives -- floor(v + 0.5) alone rounds -2.5 to -2 and 2.5 to 3, which is
   a bias, not a rounding rule. */

template<class T> struct imSampleTraits;

template<> struct imSampleTraits<imbyte>
{
  enum { integral = 1 };
  static double low()  { return 0.0; }
  static double high() { return 255.0; }
};

template<> struct imSampleTraits<short>
{
  enum { integral = 1 };
  static double low()  { return -32768.0; }
  static double high() { return 32767.0; }
};

template<> struct imSampleTraits<imushort>
{
  enum { integral = 1 };
  static double low()  { return 0.0; }
  static double high() { return 65535.0; }
};

template<> struct imSampleTraits<int>
{
  enum { integral = 1 };
  static double low()  { return -2147483648.0; }
  static double high() { return 2147483647.0; }
};

template<> struct imSampleTraits<float>
{
  enum { integral = 0 };
  static double low()  { return 0.0; }
  static double high() { return 0.0; }
};

template<> struct imSampleTraits<double>
{
  enum { integral = 0 };
  static double low()  { return 0.0; }
  static double high() { return 0.0; }
};

template<class T>
static inline T imStoreSample(double value)
{
  if (imSampleTraits<T>::integral)
  {
    value = (value < 0.0) ? -floor(-value + 0.5) : floor(value + 0.5);

    if (value < imSampleTraits<T>::low())  value = imSampleTraits<T>::low();
    if (value > imSampleTraits<T>::high()) value = imSampleTraits<T>::high();
  }

  return (T)value;
}

#endif
