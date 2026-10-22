/** \file
 * \brief Register the Camera Raw Format
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_CAMRAW_H
#define __IM_FORMAT_CAMRAW_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup camraw CAMRAW - Digital Camera Raw Files
 * \section Description
 *
 * \par
 * The unprocessed sensor files digital cameras write -- DNG, Canon CR2/CR3,
 * Nikon NEF, Sony ARW, Fujifilm RAF, Olympus ORF, Panasonic RW2 and the
 * several hundred other models LibRaw knows.\n
 * https://www.libraw.org \n
 * \par
 * You must link the application with "im_camraw.lib"
 * and you must call the function \ref imFormatRegisterCAMRAW once
 * to register the format into the IM core library.
 * In Lua call require"imlua_camraw". \n
 * \par
 * Access to the files uses LibRaw 0.20 or newer \n
 * Copyright (C) 2008-2024 LibRaw LLC, licensed LGPL-2.1 or CDDL-1.0.
 *
 * \par
 * See \ref im_format_camraw.h
 *
 * \section Features
 *
\verbatim
    Read only.
    Data Types: UShort
    Color Spaces: RGB (Gray for monochrome sensors)
    Compressions:
      NONE - whatever the camera used is decoded by LibRaw
    Only one image.
    Internally the components are always packed.
    Internally the lines are arranged from top down to bottom.
    Handle(0) returns libraw_data_t*

    Attributes, set after imFileOpen and before imFileReadImageInfo:
      RawLinear IM_INT (1) [0 or 1, default 0: 1 gives scene-linear output,
                            with no tone curve and no automatic brightening]
      RawHalfSize IM_INT (1) [0 or 1, default 0: 1 gives half the width and
                              height, one pixel per 2x2 sensor block, much faster]

    Attributes read (names as the JPEG driver gives the EXIF tags):
      Make, Model (string)
      ISOSpeedRatings, ExposureTime, FNumber, FocalLength IM_FLOAT (1)

    Comments:
      The default output is 16 bit sRGB with the standard gamma curve and the
      camera's own white balance -- a viewable picture, as dcraw would give.
      The camera's orientation is applied, so width and height may be
      exchanged relative to the sensor.
      Demosaicing happens in imFileReadImageInfo, so that call is the slow one.
\endverbatim
 * \ingroup format */

/** Register the Camera Raw Format. \n
 * In Lua, when using require"imlua_camraw" this function will be automatically called.
 * \ingroup camraw */
void imFormatRegisterCAMRAW(void);


#if defined(__cplusplus)
}
#endif

#endif
