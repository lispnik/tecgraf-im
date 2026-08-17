#!/bin/sh
# Regenerates the golden images used by test/test_golden.cpp.
#
# These are reference outputs from ImageMagick, not from this library, which
# is the whole point: every other test in this suite compares IM against
# hand-computed values or against IM itself, and neither catches an operation
# that is self-consistently wrong. A second implementation does.
#
# Only run this if you mean to change what the tests compare against. The
# committed outputs are what the assertions are written from, and a newer
# ImageMagick that rounds differently would silently move the goalposts --
# regenerating and committing is a deliberate act, not routine maintenance.
#
# Verified against ImageMagick 7.1.2-29 Q16-HDRI.
#
# Agreement observed when these were made, on the 32x24 source:
#
#   min and max at 3x3 and 5x5,     exact over the whole image, borders included
#     open, close, range
#   binary dilate, nearest-neighbour
#     resize to an integer multiple,
#     Otsu threshold                exact over the whole image
#   median 3x3, 5x5 and 7x7,        exact in the interior; the borders differ
#     binary erode                  because the two libraries pad differently
#   mean 3x3, RGB to gray,          within 1, which is rounding alone
#     RGB to YCbCr and to XYZ,
#     the L of RGB to Lab,
#     convolution with an explicit
#     kernel at 3x3 and 5x5
#   the separable convolution       within 2: the same rounding, plus the
#                                   intermediate pass truncating once more
#
# The a and b of Lab need a scale applied before they can be compared. IM
# computes a = 2.5*(fX-fY) and packs -0.5..0.5 into a byte; the standard, and
# ImageMagick, use a = 500*(fX-fY) over -128..127. The two differ by a factor
# of 637.5/500 about the midpoint and agree once that is accounted for, so the
# difference is an encoding choice rather than a disagreement about colour.
#
# Downsampling is deliberately absent. IM and ImageMagick disagree completely
# about which source pixel a destination pixel samples when shrinking -- 223
# levels apart on this source -- so there is no useful reference to compare
# against, only two defensible conventions.
#
# src.pgm is generated here too rather than by ImageMagick, so the input is
# reproducible from this script alone. It mixes a gradient, a block pattern
# and isolated extreme values: a rank filter has to respond to the structure,
# and a median has to remove the spikes, so a filter that quietly did nothing
# could not pass.

set -e
cd "$(dirname "$0")"

command -v magick >/dev/null || { echo "ImageMagick 7 (magick) not found" >&2; exit 1; }

python3 - <<'PY'
W, H = 32, 24
px = bytearray()
for y in range(H):
    for x in range(W):
        v = (x*6 + y*3) % 200
        if (x//5 + y//4) % 2 == 0:
            v = (v + 55) % 256
        if (x*7 + y*11) % 47 == 0:
            v = 255 if (x + y) % 2 else 0
        px.append(v)
open('src.pgm', 'wb').write(b'P5\n%d %d\n255\n' % (W, H) + bytes(px))

# A bilevel source for the binary morphology, and an RGB one for the colour
# conversion. Both carry small features so an erode or dilate has something to
# remove or fill rather than sweeping a flat field.
bp = bytearray()
for y in range(H):
    for x in range(W):
        bp.append(255 if ((x//3 + y//2) % 2 == 0 or (x*y) % 13 == 0) else 0)
open('src_bin.pgm', 'wb').write(b'P5\n%d %d\n255\n' % (W, H) + bytes(bp))

cp = bytearray()
for y in range(H):
    for x in range(W):
        r = (x*8 + y*2) % 256
        g = (y*10 + x*3) % 256
        b = (x*5 + y*7 + 40) % 256
        if (x//4 + y//3) % 2 == 0:
            r = (r + 90) % 256
        cp += bytes((r, g, b))
open('src_rgb.ppm', 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + bytes(cp))
PY

magick src.pgm -statistic Median  3x3 -depth 8 median3.pgm
magick src.pgm -statistic Median  5x5 -depth 8 median5.pgm
magick src.pgm -statistic Median  7x7 -depth 8 median7.pgm
magick src.pgm -statistic Minimum 3x3 -depth 8 min3.pgm
magick src.pgm -statistic Maximum 3x3 -depth 8 max3.pgm
magick src.pgm -statistic Minimum 5x5 -depth 8 min5.pgm
magick src.pgm -statistic Maximum 5x5 -depth 8 max5.pgm
magick src.pgm -statistic Mean    3x3 -depth 8 mean3.pgm
magick src.pgm -morphology Open  Square:1 -depth 8 open3.pgm
magick src.pgm -morphology Close Square:1 -depth 8 close3.pgm

# Convolution with a kernel the caller writes out, which is the general case
# every named filter in im_convolve.cpp is a special case of. Three things
# about the reference here are not guessable and were each established by
# experiment before the assertions were written:
#
# 1. ImageMagick does not normalize a kernel given as a literal string --
#    without "-define convolve:scale=!" this kernel sums to 45 and every
#    output pixel saturates to white. imProcessConvolve always divides by the
#    kernel sum (iKernelTotal, treating a sum of zero as one), so normalizing
#    is what makes the two comparable rather than a stylistic choice.
#
# 2. Correlate, not Convolve, is the matching operator -- and the kernel below
#    is written with its ROWS REVERSED from the one the test builds. Both
#    follow from imImage storing its rows bottom-up. IM's inner loop applies
#    the kernel without rotating it (correlation) in memory order, so read in
#    file order, which is what ImageMagick works in, the kernel arrives upside
#    down. Getting either half of this wrong looks convincingly like a defect:
#    against -morphology Convolve the two disagree by 33 levels in the
#    interior, and against an unflipped Correlate by 47.
#
# 3. "-virtual-pixel mirror" is needed and the default is not enough. It makes
#    no difference at 3x3 -- edge replication and mirroring pick the same
#    pixel one step out -- so a 3x3 case alone would suggest the setting is
#    unnecessary, and the 5x5 case below then disagrees at the border by 7.
#
# With all three right the agreement is within 1 everywhere, borders included,
# and IM is never the higher of the two: it truncates the divided sum where
# ImageMagick rounds it. The tests assert that direction as well as the
# magnitude, since a tolerance of 1 in both directions would also admit a
# genuine off-by-one.
#
# The 3x3 kernel is deliberately asymmetric. A symmetric one cannot tell
# correlation from convolution, so it would have hidden point 2 entirely.
magick src.pgm -virtual-pixel mirror -define convolve:scale='!' \
  -morphology Correlate '3x3: 7,8,9  4,5,6  1,2,3' -depth 8 conv_asym3.pgm

# A 5x5 binomial, symmetric, so it exercises the larger kernel without
# depending on the orientation above. Used twice by the tests: once against
# imProcessConvolve with the full 25 weights, once against imProcessConvolveSep
# given the same weights as a row and a column.
magick src.pgm -virtual-pixel mirror -define convolve:scale='!' \
  -morphology Convolve '5x5: 1,4,6,4,1  4,16,24,16,4  6,24,36,24,6  4,16,24,16,4  1,4,6,4,1' \
  -depth 8 gauss5.pgm

# Nearest neighbour to exactly twice the size, where the mapping is
# unambiguous. See the note above about shrinking.
magick src.pgm -filter Point -resize 64x48! -depth 8 resize_near2x.pgm

# Otsu picks its own level from the histogram, so agreement here is agreement
# about the algorithm rather than about applying a level someone chose.
magick src.pgm -auto-threshold OTSU -depth 8 otsu.pgm

# Binary morphology, on a bilevel source of its own.
magick src_bin.pgm -morphology Erode  Square:1 -depth 8 bin_erode3.pgm
magick src_bin.pgm -morphology Dilate Square:1 -depth 8 bin_dilate3.pgm

# Rec601 luma, which is the transform imConvertColorSpace applies. ImageMagick
# 7 would otherwise work in linear light and give a quite different answer, so
# the -grayscale operator is named explicitly rather than -colorspace Gray.
magick src_rgb.ppm -grayscale Rec601Luma -depth 8 rgb2gray601.pgm

# The other colour spaces. "-set colorspace sRGB" after the conversion is
# essential and not obvious: without it ImageMagick converts the data back to
# sRGB on the way into a PPM, because PPM is an RGB format and version 7
# tracks what a file is in. The reference then disagrees with IM by up to 251
# levels and looks like a real defect rather than a round trip. The -set
# relabels without converting, so the channel values are written as they are.
for cs in YCbCr XYZ Lab; do
  lc=$(echo "$cs" | tr 'A-Z' 'a-z')
  magick src_rgb.ppm -colorspace "$cs" -set colorspace sRGB -depth 8 "rgb2$lc.ppm"
done

# The range filter is max minus min by definition, and ImageMagick has no
# single statistic for it -- so this one is derived from the two outputs above
# rather than produced directly. Still independent of IM; just note that it
# inherits whatever those two did at the border.
python3 - <<'PY'
def read(path):
    d = open(path, 'rb').read()
    i = d.index(b'255\n') + 4
    return d[:i], bytearray(d[i:])
head, mx = read('max3.pgm')
_,    mn = read('min3.pgm')
open('range3.pgm', 'wb').write(head + bytes(bytearray(a - b for a, b in zip(mx, mn))))
PY

echo "regenerated: $(ls *.pgm | tr '\n' ' ')"
