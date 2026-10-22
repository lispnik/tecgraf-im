#!/usr/bin/env python3
"""Regenerates the EXR and Radiance HDR fixtures beside this script.

Like regenerate.sh in golden/, running this is a deliberate act: the tests in
test_exr.cpp and test_hdr.cpp were written from what it produces. The point of
the files is that this library did not write them -- the EXRs come from
OpenEXR's own Python bindings (and, for the luminance/chroma one, from
OpenEXR's C++ RgbaOutputFile), the HDRs from ImageMagick -- so a read path that
is wrong in the same way as the matching write path still fails.

Every pixel follows value() below, which the tests recompute:

    value(x, y, c) = 2 ** ((x - 16) / 4) * (1 + c / 4) + y / 100

so a row spans roughly 2^-4 to 2^6 -- well outside 0..1 on both sides, which
is what a float format is for -- and each channel and row is distinct.

Needs: python3 with numpy and OpenEXR (pip install OpenEXR numpy), ImageMagick
7 (`magick`), and a C++ compiler plus pkg-config's OpenEXR for exr_yc.exr.
"""

import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
import OpenEXR

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 40, 30


def value(x, y, c):
    return 2.0 ** ((x - 16) / 4.0) * (1.0 + c / 4.0) + y / 100.0


def plane(c, w=W, h=H):
    return np.array([[value(x, y, c) for x in range(w)] for y in range(h)],
                    dtype=np.float32)


def alpha(w=W, h=H):
    return np.array([[y / (h - 1.0) for _ in range(w)] for y in range(h)],
                    dtype=np.float32)


def tiles(x_size, y_size):
    description = OpenEXR.TileDescription()
    description.xSize = x_size
    description.ySize = y_size
    description.mode = OpenEXR.ONE_LEVEL
    return description


def write_exr(name, header, channels):
    OpenEXR.File(header, channels).write(os.path.join(HERE, name))


def exr_fixtures():
    # RGBA, half, PIZ, scanline: the commonest file a renderer produces.
    write_exr("exr_rgba_half_piz.exr",
              {"compression": OpenEXR.PIZ_COMPRESSION,
               "type": OpenEXR.scanlineimage},
              {"R": plane(0).astype(np.float16),
               "G": plane(1).astype(np.float16),
               "B": plane(2).astype(np.float16),
               "A": alpha().astype(np.float16)})

    # Luminance only, float, tiled: the tiled reader, and the Y channel.
    write_exr("exr_y_float_tiled.exr",
              {"compression": OpenEXR.ZIP_COMPRESSION,
               "type": OpenEXR.tiledimage,
               "tiles": tiles(16, 16)},
              {"Y": plane(0)})

    # A layer, an unrelated extra channel, and a data window that is offset
    # from and smaller than the display window -- a cropped render pass.
    # value() is relative to the data window's corner.
    window_origin = (10, 5)
    header = {"compression": OpenEXR.ZIP_COMPRESSION,
              "type": OpenEXR.scanlineimage,
              "dataWindow": (np.array(window_origin, dtype=np.int32),
                             np.array((window_origin[0] + W - 1,
                                       window_origin[1] + H - 1), dtype=np.int32)),
              "displayWindow": (np.array((0, 0), dtype=np.int32),
                                np.array((63, 47), dtype=np.int32))}
    write_exr("exr_layer_window.exr", header,
              {"diffuse.R": plane(0), "diffuse.G": plane(1),
               "diffuse.B": plane(2), "Z": np.full((H, W), 7.0, np.float32)})

    # Two unrelated channels and no colour: there is no honest way to make
    # this one image, so the driver must refuse it rather than pick one.
    write_exr("exr_unmappable.exr",
              {"compression": OpenEXR.NO_COMPRESSION,
               "type": OpenEXR.scanlineimage},
              {"Z": np.full((4, 4), 1.0, np.float32),
               "N.x": np.full((4, 4), 0.5, np.float32)})


YC_SOURCE = r"""
#include <ImfRgbaFile.h>
#include <ImfArray.h>
#include <cmath>
int main(int, char** argv)
{
  const int w = %d, h = %d;
  Imf::Array2D<Imf::Rgba> px(h, w);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
    {
      /* Gray, so the subsampled chroma is zero and the reconstruction
         should give back three equal channels. */
      float v = std::pow(2.0f, (x - 16) / 4.0f) + y / 100.0f;
      px[y][x] = Imf::Rgba(v, v, v, 1.0f);
    }
  Imf::RgbaOutputFile file(argv[1], w, h, Imf::WRITE_YC);
  file.setFrameBuffer(&px[0][0], 1, w);
  file.writePixels(h);
  return 0;
}
""" % (W, H)


def exr_yc_fixture():
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "OpenEXR"],
                                    text=True).split()
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "yc.cpp")
        binary = os.path.join(tmp, "yc")
        with open(source, "w") as f:
            f.write(YC_SOURCE)
        subprocess.check_call(["c++", "-std=c++17", source, "-o", binary] + flags)
        subprocess.check_call([binary, os.path.join(HERE, "exr_yc.exr")])


def write_pfm(path, planes):
    """PFM is the hand-off to ImageMagick: trivial enough to write here, and
    stored bottom row first, which is the one thing to get right."""
    h, w = planes[0].shape
    with open(path, "wb") as f:
        f.write(b"PF\n%d %d\n-1.0\n" % (w, h))
        for y in reversed(range(h)):
            for x in range(w):
                f.write(struct.pack("<3f", *(p[y, x] for p in planes)))


def magick(source, target):
    """ImageMagick takes a PFM to be sRGB and HDR to be linear, so left to
    itself it applies the sRGB curve on the way through -- 0.0625 arrives as
    0.0052, and the fixture no longer matches value(). Declaring the input
    already linear stops that. Getting this wrong looks exactly like a decoder
    applying a gamma it should not."""
    subprocess.check_call(["magick", source, "-set", "colorspace", "RGB", target])


def hdr_fixtures():
    with tempfile.TemporaryDirectory() as tmp:
        # Wide enough for the new per-component RLE, which ImageMagick uses
        # from width 8 up.
        pfm = os.path.join(tmp, "wide.pfm")
        write_pfm(pfm, [plane(0), plane(1), plane(2)])
        magick(pfm, os.path.join(HERE, "hdr_rle.hdr"))

        # Narrower than 8, so the scanlines are necessarily flat.
        pfm = os.path.join(tmp, "narrow.pfm")
        write_pfm(pfm, [plane(c, 5, 4) for c in range(3)])
        magick(pfm, os.path.join(HERE, "hdr_flat.hdr"))

    # No current tool writes old-style runs, so this one is assembled by hand:
    # 12x2, row 0 one literal pixel then a run of 11 repeating it, row 1 two
    # literals then a run of 10. The pixel bytes are RGBE with exponent 129
    # (scale 2^-7), so mantissa m decodes to (m + 0.5) / 128.
    with open(os.path.join(HERE, "hdr_old_rle.hdr"), "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\nEXPOSURE=2.0\n"
                b"EXPOSURE=1.5\n\n-Y 2 +X 12\n")
        f.write(bytes([64, 128, 192, 129]) + bytes([1, 1, 1, 11]))
        f.write(bytes([10, 20, 30, 129]) + bytes([40, 50, 60, 129]) +
                bytes([1, 1, 1, 10]))


if __name__ == "__main__":
    exr_fixtures()
    exr_yc_fixture()
    hdr_fixtures()
