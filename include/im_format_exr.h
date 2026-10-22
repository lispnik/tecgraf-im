/** \file
 * \brief Register the OpenEXR Format
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_EXR_H
#define __IM_FORMAT_EXR_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup exr EXR - OpenEXR High Dynamic Range Image
 * \section Description
 *
 * \par
 * The Academy Software Foundation's OpenEXR, the standard interchange format
 * for floating point images in film and rendering.\n
 * https://openexr.com \n
 * \par
 * You must link the application with "im_exr.lib"
 * and you must call the function \ref imFormatRegisterEXR once
 * to register the format into the IM core library.
 * In Lua call require"imlua_exr". \n
 * \par
 * Access to the file format uses OpenEXR 3.1 or newer \n
 * https://github.com/AcademySoftwareFoundation/openexr \n
 * Copyright (c) Contributors to the OpenEXR Project, licensed BSD-3-Clause.
 *
 * \par
 * See \ref im_format_exr.h
 *
 * \section Features
 *
\verbatim
    Data Types: Float (half and uint channels are converted to float on read)
    Color Spaces: Gray and RGB
    Compressions:
      NONE  - no compression
      RLE   - run length
      ZIPS  - zlib, one scanline at a time
      ZIP   - zlib, 16 scanlines at a time  [default]
      PIZ   - wavelet + Huffman
      PXR24 - lossy for float, 24 bit
      B44   - lossy for half, fixed rate
      B44A  - lossy for half, flat areas smaller
      DWAA  - lossy DCT, 32 scanlines
      DWAB  - lossy DCT, 256 scanlines
    Only one image (a multi-part file reads as its first part).
    Can have an alpha channel.
    Internally the components are always packed (interleaved).
    Internally the lines are arranged from top down to bottom.
    Handle(0) returns Imf::InputFile* when reading, NULL when writing.

    Attributes:
      Half IM_INT (1) [write only, 0 or 1, default 0: write half instead of float channels]

    Comments:
      The channels read are, in order of preference: R G B; Y RY BY (luminance
      and subsampled chroma, converted to RGB); Y; the first layer, by name,
      with layer.R layer.G layer.B; or the only channel in the file. Each
      takes A (or layer.A) as alpha when present. Any other channel layout,
      and deep files, are refused.
      The size is the data window, not the display window.
      Pixel values are passed through as stored: EXR stores colour
      premultiplied by alpha, and so does IM after reading one.
      Writing is exact with the default float channels and the lossless
      compressions (NONE, RLE, ZIPS, ZIP, PIZ). Half=1 rounds to 11 bits of
      precision.
\endverbatim
 * \ingroup format */

/** Register the OpenEXR Format. \n
 * In Lua, when using require"imlua_exr" this function will be automatically called.
 * \ingroup exr */
void imFormatRegisterEXR(void);


#if defined(__cplusplus)
}
#endif

#endif
