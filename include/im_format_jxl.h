/** \file
 * \brief Register the JPEG XL Format
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_JXL_H
#define __IM_FORMAT_JXL_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup jxl JXL - JPEG XL
 * \section Description
 *
 * \par
 * ISO/IEC 18181, JPEG XL. Both the bare codestream and the ISOBMFF
 * container are read.\n
 * https://jpeg.org/jpegxl/ \n
 * \par
 * You must link the application with "im_jxl.lib"
 * and you must call the function \ref imFormatRegisterJXL once
 * to register the format into the IM core library.
 * In Lua call require"imlua_jxl". \n
 * \par
 * Access to the file format uses libjxl 0.7 or newer \n
 * https://github.com/libjxl/libjxl \n
 * Copyright (c) the JPEG XL Project Authors, licensed BSD-3-Clause.
 *
 * \par
 * See \ref im_format_jxl.h
 *
 * \section Features
 *
\verbatim
    Data Types: Byte, UShort and Float
    Color Spaces: Gray and RGB (Binary is written as Gray)
    Compressions:
      JXL - JPEG XL  [default]
    Only one image (an animated file reads as its first frame).
    Can have an alpha channel.
    Internally the components are always packed (interleaved).
    Internally the lines are arranged from top down to bottom.
    Handle() returns NULL.

    Attributes:
      Quality IM_INT (1) [write only, 0-100, default 90]
      Lossless IM_INT (1) [write only, 0 or 1, default 0]

    Comments:
      Lossless is exact for every data type, IM_FLOAT included.
      Quality maps to libjxl's Butteraugli distance the way cjxl's -q does:
      90 is distance 1.0, "visually lossless".
      Files with more than 8 bits per sample are read as IM_USHORT scaled to
      the full 16 bit range, and floating point files as IM_FLOAT.
      The EXIF orientation is applied on read, so width and height are those
      of the image as displayed.
      Pixels are written tagged as sRGB; no ICC profile is read or written.
      A JPEG recompressed losslessly into JPEG XL (cjxl's default for JPEG
      input) is decoded to pixels like any other file.
\endverbatim
 * \ingroup format */

/** Register the JPEG XL Format. \n
 * In Lua, when using require"imlua_jxl" this function will be automatically called.
 * \ingroup jxl */
void imFormatRegisterJXL(void);


#if defined(__cplusplus)
}
#endif

#endif
