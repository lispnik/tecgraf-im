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
#   min, max, open, close, range   exact over the whole image, borders included
#   median 3x3 and 5x5             exact in the interior; the borders differ
#                                  because the two libraries pad differently
#   mean 3x3                       within 1, which is rounding alone
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
PY

magick src.pgm -statistic Median  3x3 -depth 8 median3.pgm
magick src.pgm -statistic Median  5x5 -depth 8 median5.pgm
magick src.pgm -statistic Minimum 3x3 -depth 8 min3.pgm
magick src.pgm -statistic Maximum 3x3 -depth 8 max3.pgm
magick src.pgm -statistic Mean    3x3 -depth 8 mean3.pgm
magick src.pgm -morphology Open  Square:1 -depth 8 open3.pgm
magick src.pgm -morphology Close Square:1 -depth 8 close3.pgm

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
