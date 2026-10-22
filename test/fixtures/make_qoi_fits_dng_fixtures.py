#!/usr/bin/env python3
"""Regenerates the QOI, FITS and DNG fixtures beside this script.

As with make_float_fixtures.py, running this is a deliberate act: test_qoi.cpp,
test_fits.cpp and test_camraw.cpp were written from what it produces. None of
these files is written by this library -- QOI by ImageMagick, FITS by astropy,
the DNG by tifffile from tags assembled here per the DNG specification -- so a
reader wrong in the same way as its own writer still fails.

The pixel formulas below are recomputed by the tests; keep them in step.

Needs: python3 with numpy, astropy and tifffile; ImageMagick 7 (`magick`).
"""

import os
import subprocess
import tempfile

import numpy as np
import tifffile
from astropy.io import fits

HERE = os.path.dirname(os.path.abspath(__file__))
EXAMPLES = os.path.join(HERE, "..", "..", "html", "examples")


def magick(*args):
    subprocess.check_call(["magick"] + list(args))


def qoi_fixtures():
    # Gray PNG to a 3 channel QOI: compared against rice.png, every channel.
    magick(os.path.join(EXAMPLES, "rice.png"), "-type", "TrueColor",
           os.path.join(HERE, "qoi_rgb.qoi"))

    # RGBA, with alpha varying and colour under transparent pixels, from a PNG
    # committed beside it so the comparison is exact and self-contained.
    # Small and busy, so every QOI chunk type turns up: runs in the flat
    # band, index hits from the repeating pattern, diffs and lumas on the
    # gradients, full RGB/RGBA on the jumps.
    png = os.path.join(HERE, "qoi_rgba_source.png")
    magick("-size", "33x21", "gradient:red-blue",
           "(", "-size", "33x21", "pattern:checkerboard", ")",
           "-compose", "multiply", "-composite",
           "(", "-size", "21x33", "gradient:white-black", "-rotate", "90", ")",
           "-alpha", "off", "-compose", "copy_opacity", "-composite",
           "-fill", "#336699", "-draw", "rectangle 0,0 32,3",
           "-depth", "8", "-define", "png:color-type=6", png)
    magick(png, os.path.join(HERE, "qoi_rgba.qoi"))


def fits_value(x, y, p=0):
    return (x * 1000 + y * 37 + p * 5000) % 65536


def fits_fixtures():
    h, w = 30, 40
    y, x = np.mgrid[0:h, 0:w]

    # Unsigned 16 bit: astropy stores it the FITS way, as int16 with
    # BZERO = 32768. Row 0 of the array is the first row in the file, which
    # FITS defines as the bottom. Also carries header keywords.
    hdu = fits.PrimaryHDU(fits_value(x, y).astype(np.uint16))
    hdu.header["OBJECT"] = "M31"
    hdu.header["TELESCOP"] = "Synthetic"
    hdu.header["EXPTIME"] = 30.5
    hdu.writeto(os.path.join(HERE, "fits_uint16.fits"), overwrite=True)

    # int16 with a non-integer BSCALE: the reader must apply it and come back
    # with floats. value = raw * 0.5 + 100.
    raw = ((x * 7 + y * 3) % 200 - 100).astype(np.int16)
    hdu = fits.PrimaryHDU(raw)
    hdu.header["BSCALE"] = 0.5
    hdu.header["BZERO"] = 100.0
    hdu.writeto(os.path.join(HERE, "fits_scaled.fits"), overwrite=True)

    # An empty primary HDU, then a float cube of 4 planes in an extension:
    # the image is not in the first HDU, and the cube is a sequence.
    cube = np.stack([fits_value(x, y, p) / 7.0 for p in range(4)]).astype(np.float32)
    fits.HDUList([fits.PrimaryHDU(), fits.ImageHDU(cube)]).writeto(
        os.path.join(HERE, "fits_float_cube.fits"), overwrite=True)

    # A 3 plane byte cube: one RGB image.
    rgb = np.stack([(x * 6 + c * 40 + y) % 256 for c in range(3)]).astype(np.uint8)
    fits.PrimaryHDU(rgb).writeto(os.path.join(HERE, "fits_rgb.fits"), overwrite=True)

    # Tile-compressed int32 with Rice: a binary table cfitsio presents as an
    # image.
    big = (fits_value(x, y).astype(np.int32) * 1000 - 20000000)
    fits.HDUList([fits.PrimaryHDU(),
                  fits.CompImageHDU(big, compression_type="RICE_1")]).writeto(
        os.path.join(HERE, "fits_rice.fits"), overwrite=True)


def dng_fixture():
    """A 64x48 RGGB Bayer DNG with three vertical bands, red, green and blue.

    At each site the sensor value is high if the site's filter matches the
    band's colour and low otherwise, so a correct decode gives a red band on
    the left, green in the middle and blue on the right. A wrong CFA phase or
    a swapped channel order makes the bands come out in the wrong colours,
    which is exactly what the test checks.

    The colour matrix is XYZ (D65) to linear sRGB, so the "camera" is sRGB
    itself and LibRaw's conversion to sRGB output is the identity."""
    h, w = 48, 64
    cfa_colour = np.array([[0, 1], [1, 2]])          # R G / G B
    band = np.minimum(np.arange(w) * 3 // w, 2)      # 0, 1, 2 by thirds
    data = np.empty((h, w), np.uint16)
    for yy in range(h):
        for xx in range(w):
            data[yy, xx] = 50000 if cfa_colour[yy % 2, xx % 2] == band[xx] else 2000

    def srational(values, scale=10000):
        out = []
        for v in values:
            out += [int(round(v * scale)), scale]
        return out

    xyz_to_srgb = [3.2406, -1.5372, -0.4986,
                   -0.9689, 1.8758, 0.0415,
                   0.0557, -0.2040, 1.0570]

    tags = [
        (33421, "H", 2, (2, 2), True),                 # CFARepeatPatternDim
        (33422, "B", 4, (0, 1, 1, 2), True),           # CFAPattern: RGGB
        (50706, "B", 4, (1, 4, 0, 0), True),           # DNGVersion
        (50707, "B", 4, (1, 1, 0, 0), True),           # DNGBackwardVersion
        (50708, "s", 0, "IM Synthetic", True),         # UniqueCameraModel
        (50717, "I", 1, 65535, True),                  # WhiteLevel
        (50714, "I", 1, 0, True),                      # BlackLevel
        (50721, "2i", 9, srational(xyz_to_srgb), True),  # ColorMatrix1
        (50778, "H", 1, 21, True),                     # CalibrationIlluminant1: D65
        (50728, "2I", 3, (1, 1, 1, 1, 1, 1), True),    # AsShotNeutral
        (271, "s", 0, "IM", True),                     # Make
        (272, "s", 0, "Synthetic", True),              # Model
    ]
    tifffile.imwrite(os.path.join(HERE, "camraw_bands.dng"), data,
                     photometric=32803, subfiletype=0, metadata=None,
                     extratags=tags)


if __name__ == "__main__":
    qoi_fixtures()
    fits_fixtures()
    dng_fixture()
