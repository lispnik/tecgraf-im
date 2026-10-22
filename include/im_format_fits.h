/** \file
 * \brief Register the FITS Format
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_FITS_H
#define __IM_FORMAT_FITS_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup fits FITS - Flexible Image Transport System
 * \section Description
 *
 * \par
 * The standard data format of astronomy, maintained by the IAU FITS Working
 * Group.\n
 * https://fits.gsfc.nasa.gov \n
 * \par
 * You must link the application with "im_fits.lib"
 * and you must call the function \ref imFormatRegisterFITS once
 * to register the format into the IM core library.
 * In Lua call require"imlua_fits". \n
 * \par
 * Access to the file format uses NASA's cfitsio \n
 * https://heasarc.gsfc.nasa.gov/fitsio/ \n
 * Copyright (Unpublished--all rights reserved under the copyright laws of the
 * United States), U.S. Government as represented by the Administrator of the
 * National Aeronautics and Space Administration; permissive licence.
 *
 * \par
 * See \ref im_format_fits.h
 *
 * \section Features
 *
\verbatim
    Data Types: Byte, Short, UShort, Int, Float and Double
    Color Spaces: Gray and RGB
    Compressions:
      NONE      - no compression [default]
      GZIP      - tile compressed, lossless for every type
      RICE      - tile compressed, integer types only
      HCOMPRESS - read only
      PLIO      - read only
    Can have more than one image: every 2D plane of every image HDU is one
      image (a 3 plane cube is one RGB image instead). Only one is written.
    Internally the components are never packed (FITS cubes are planar).
    Internally the lines are arranged from bottom up, as FITS stores them.
    Handle(0) returns fitsfile*

    Attributes read, when the header has them:
      OBJECT, TELESCOP, INSTRUME, OBSERVER, DATE-OBS, FILTER, BUNIT (string)
      EXPTIME, EQUINOX, AIRMASS IM_FLOAT (1)

    Comments:
      BZERO and BSCALE are applied on read: 16 bit data with BZERO = 32768
      reads as IM_USHORT, and non-integer scaling reads as IM_FLOAT.
      Unsigned 32 bit and 64 bit integer data read as IM_DOUBLE, IM having
      no integer type that holds them.
      IM_USHORT is written the FITS way, as 16 bit with BZERO = 32768.
      Floating point is never quantized: GZIP keeps every bit.
      Alpha has no FITS meaning and is not written.
\endverbatim
 * \ingroup format */

/** Register the FITS Format. \n
 * In Lua, when using require"imlua_fits" this function will be automatically called.
 * \ingroup fits */
void imFormatRegisterFITS(void);


#if defined(__cplusplus)
}
#endif

#endif
