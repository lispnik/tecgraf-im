/** \file
 * \brief Register the HEIF and AVIF Formats
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_FORMAT_HEIF_H
#define __IM_FORMAT_HEIF_H

#if	defined(__cplusplus)
extern "C" {
#endif


/** \defgroup heif HEIF/AVIF - ISO Base Media File Format Images
 * \section Description
 *
 * \par
 * ISO/IEC 23008-12 (HEIF, the container used by Apple's .heic) and
 * the AV1 Image File Format (.avif).\n
 * https://nokiatech.github.io/heif/ \n
 * https://aomediacodec.github.io/av1-avif/
 * \par
 * You must link the application with "im_heif.lib"
 * and you must call the function \ref imFormatRegisterHEIF once
 * to register both formats into the IM core library.
 * In Lua call require"imlua_heif". \n
 * \par
 * Access to both file formats uses libheif 1.17 or newer \n
 * https://github.com/strukturag/libheif \n
 * Copyright (c) 2017-2024 Dirk Farin, licensed LGPL-3.0-or-later.
 *
 * \par
 * See \ref im_format_heif.h
 *
 * \section Licensing
 *
\verbatim
    libheif itself is LGPL and links no codec directly, but the codecs it
    loads do carry obligations:

      decode HEIC   libde265        LGPL-2.1
      encode HEIC   x265            GPL-2.0   <-- see below
      AVIF          aom / dav1d     BSD-2

    A binary whose link closure includes x265 must be distributed under the
    GPL, which would override IM's MIT terms for anyone redistributing it.
    If that matters for your distribution, either build libheif without x265
    and let it load an encoder plugin at run time (libheif 1.16+), or ship
    AVIF-only encoding, which has no such constraint.

    Independently of software licensing, HEVC is covered by patent pools
    (MPEG LA / Access Advance) that may require a licence for commercial
    distribution. AVIF is royalty free.
\endverbatim
 *
 * \section Features
 *
\verbatim
    Data Types: Byte and UShort
    Color Spaces: Gray and RGB
    Compressions:
      HEVC - ISO/IEC 23008-2, for the HEIF format  [default for HEIF]
      AV1  - AOMedia Video 1, for the AVIF format  [default for AVIF]
    Can have more than one image (HEIF bursts and Live Photos are read as
      a sequence; only the first image is written).
    Can have an alpha channel.
    Internally the components are always packed (interleaved).
    Internally the lines are arranged from top down to bottom.
    Handle(0) returns heif_context*
    Handle(1) returns heif_image_handle*
    Handle(2) returns heif_image*

    Attributes:
      Quality IM_INT (1) [write only, 0-100, default 80]
      Lossless IM_INT (1) [write only, 0 or 1, default 0]

    Comments:
      Images with more than 8 bits per sample are read as IM_USHORT and
      scaled up to the full 16 bit range, so a 10 bit file and a 16 bit
      file are comparable once loaded.
      Writing always produces a single image; multi-image containers are
      read-only.

      Lossless=1 also selects 4:4:4 chroma, without which the default 4:2:0
      subsampling loses colour before the encoder runs. Measured round-trip
      error with Lossless=1, per sample of 255:

        IM_GRAY  IM_BYTE     exact
        IM_RGB   IM_BYTE     1, the RGB/YCbCr conversion rounding floor
        IM_USHORT            15 of 65535, see below

      IM_USHORT is written at 12 bits, the most HEVC Main 12 and AV1 carry,
      so the low 4 bits do not survive. It is scaled rather than refused,
      unlike the JPEG driver which rejects anything but IM_BYTE.

      Without Lossless the error is far larger -- around 75 of 255 at
      Quality=100 on saturated colour -- because 4:2:0 discards three
      quarters of the chroma samples.
\endverbatim
 * \ingroup format */

/** Register the HEIF and AVIF Formats. \n
 * Registers two format drivers, "HEIF" (*.heic;*.heif) and "AVIF" (*.avif),
 * which share one implementation. \n
 * In Lua, when using require"imlua_heif" this function will be automatically called.
 * \ingroup heif */
void imFormatRegisterHEIF(void);


#if defined(__cplusplus)
}
#endif

#endif
