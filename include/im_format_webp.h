/** \file
 * \brief Register the WebP Format
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_WEBP_H
#define __IM_FORMAT_WEBP_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup webp WebP - Google WebP Image Format
 * \section Description
 *
 * \par
 * Google's RIFF-based image format, lossy (VP8) or lossless (VP8L),
 * with optional alpha and animation.\n
 * https://developers.google.com/speed/webp \n
 * \par
 * You must link the application with "im_webp.lib"
 * and you must call the function \ref imFormatRegisterWebP once
 * to register the format into the IM core library.
 * In Lua call require"imlua_webp". \n
 * \par
 * Access to the file format uses libwebp and libwebpdemux 1.2 or newer \n
 * https://chromium.googlesource.com/webm/libwebp \n
 * Copyright (c) 2010 Google Inc., licensed BSD-3-Clause.
 *
 * \par
 * See \ref im_format_webp.h
 *
 * \section Features
 *
\verbatim
    Data Types: Byte
    Color Spaces: RGB (Gray and Binary are written as RGB, and read back as RGB)
    Compressions:
      VP8  - lossy     [default]
      VP8L - lossless
    Can have more than one image (animated files are read frame by frame,
      each composited onto the full canvas; only one image is written).
    Can have an alpha channel.
    Internally the components are always packed (interleaved).
    Internally the lines are arranged from top down to bottom.
    Handle() returns NULL.

    Attributes:
      Quality IM_INT (1) [write only, 0-100, default 75]
      Lossless IM_INT (1) [write only, 0 or 1, default 0; 1 is the same as
                           selecting the VP8L compression]

    Comments:
      WebP has no grayscale mode, so a gray image is stored as RGB with three
      equal channels and reads back as IM_RGB.
      A lossless write keeps the colour of fully transparent pixels exactly
      (libwebp's "exact" option); by default libwebp would discard it.
      The largest image WebP can hold is 16383 x 16383.
      ICC, EXIF and XMP chunks are neither read nor written.
\endverbatim
 * \ingroup format */

/** Register the WebP Format. \n
 * In Lua, when using require"imlua_webp" this function will be automatically called.
 * \ingroup webp */
void imFormatRegisterWebP(void);


#if defined(__cplusplus)
}
#endif

#endif
