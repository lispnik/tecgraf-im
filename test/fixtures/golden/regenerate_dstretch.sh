#!/bin/sh
# Regenerates the decorrelation-stretch golden images used by
# test/test_golden.cpp.
#
# Companion to regenerate.sh beside this file, and separate from it for one
# reason: that script's references come from ImageMagick and its header says
# so, down to the version it was verified against. These do not. ImageMagick
# has no decorrelation stretch, so the second implementation here is numpy on
# LAPACK.
#
# Everything regenerate.sh says about when to run it applies here too. The
# committed outputs are what the assertions were written from; regenerating is
# a deliberate act, not maintenance.
#
# Verified against numpy 2.5.2.
#
# WHY THIS IS A REAL CHECK AND NOT A TRANSCRIPTION
#
# The worry with a hand-written reference is that it ends up being the same
# code twice, in which case it confirms nothing. That is not the case here,
# and the reason is worth stating because it is also why the comparison is
# stable at all.
#
# The transform is
#
#     T = diag(target) . Sigma^(-1/2)
#
# and Sigma^(-1/2) is the unique symmetric positive semi-definite matrix whose
# square is Sigma^(-1). It is a function of the covariance alone. In
# particular:
#
#   - flipping the sign of an eigenvector leaves v.v^T unchanged;
#   - reordering the eigenvalues reorders the terms of a sum;
#   - and where two eigenvalues coincide, and the individual eigenvectors are
#     not even well defined, their gains are equal, so what the pair
#     contributes is the projector onto the eigenspace, which is well defined.
#
# So numpy.linalg.eigh -- LAPACK's divide-and-conquer dsyevd -- and the cyclic
# Jacobi in src/process/im_decorrelate.cpp can disagree completely about the
# eigenvectors and still agree about T to round-off. The two implementations
# share the formula and share nothing else: not the eigen-solver, not the
# accumulation order, not the language. Measured, the largest disagreement
# over random sign and order scrambles was 4.4e-16.
#
# THREE CONVENTIONS THAT HAVE TO MATCH
#
# 1. Rounding, not truncation, and away from zero. The library rounds because
#    truncating biases every band half a level toward zero, which is exactly
#    the size of error a one-level tolerance would hide. numpy's np.round is
#    NOT the same thing -- it rounds halves to even, and exact halves do occur
#    here -- so the reference adds +/-0.5 and truncates, matching
#    iDecorrStore in im_decorrelate.cpp.
#
# 2. The covariance divisor. The library uses n-1. It does not actually matter
#    -- scaling Sigma by a constant scales the eigenvalues and the target
#    standard deviations by amounts that cancel -- but np.cov defaults to n-1
#    as well, so there is nothing to remember.
#
# 3. The Y'CbCr matrix. ITU-R BT.601 with no headroom, the same coefficients
#    imColorRGB2YCbCr uses. Published, so both sides can follow it
#    independently rather than one copying the other.
#
# WHAT IS DELIBERATELY NOT HERE
#
# No L*a*b* golden. IM's L*a*b* is its own encoding -- a = 2.5*(fX-fY) and
# b = (fY-fZ), against the standard's 500 and 200 -- so a numpy reference for
# it would have to restate IM's constants from im_color.h, and a reference
# copied out of the code under test checks nothing. The L*a*b* spaces are
# covered structurally in test/test_decorrelate.cpp instead.
#
# The source is generated here rather than reusing src_rgb.ppm, whose channel
# covariance has a condition number of 2.5. A decorrelation stretch on colours
# that are already nearly independent barely moves them, so it would be a
# fixture that passes whatever the code does. This one has a condition number
# near 90: strongly correlated, which is both the case the operation exists
# for and the numerically interesting one, but nowhere near the 1e-12 at which
# the degenerate-direction guard starts firing.

set -e
cd "$(dirname "$0")"

python3 <<'PY'
import numpy as np

W, H = 32, 24
SCALE = 1.0

# ---- the source ---------------------------------------------------------
# Colours strung out along a single axis, the way faded pigment sits against
# rock, plus a mild spatial gradient so the image is not pure noise and a
# transform that shuffled pixels could not pass.
rng = np.random.default_rng(20240915)
L = np.linalg.cholesky(np.array([[1.0, 0.97, 0.94],
                                 [0.97, 1.0, 0.96],
                                 [0.94, 0.96, 1.0]]))
u = rng.standard_normal((H*W, 3)) @ L.T
gy, gx = np.mgrid[0:H, 0:W]
ramp = (gx/(W-1.0) * 18.0 + gy/(H-1.0) * 10.0).reshape(-1, 1)
src = np.clip(np.array([126.0, 116.0, 104.0]) + 16.0*u + ramp, 0, 255).round()
src = src.astype(np.uint8)

def write_ppm(name, a):
    with open(name, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (W, H))
        f.write(a.reshape(H, W, 3).astype(np.uint8).tobytes())

write_ppm("src_dstretch.ppm", src)

cond = np.linalg.cond(np.cov(src.astype(float), rowvar=False))
print("src_dstretch.ppm  covariance condition number %.1f" % cond)

# ---- the stretch --------------------------------------------------------
YCBCR = np.array([[ 0.299,  0.587,  0.114],
                  [-0.169, -0.331,  0.500],
                  [ 0.500, -0.419, -0.081]])

def decorrelate(x, M, scale):
    """T = M^-1 . diag(target) . Sigma_w^(-1/2) . M, applied about the mean."""
    w = x @ M.T
    mu = w.mean(axis=0)
    S = np.cov(w, rowvar=False)                      # n-1, see note 2

    lam, V = np.linalg.eigh(S)
    keep = lam > 1e-12 * lam.max()
    gain = np.where(keep, 1.0/np.sqrt(np.where(keep, lam, 1.0)), 1.0)

    target = scale * np.sqrt(np.diag(S))
    Tw = np.diag(target) @ (V * gain) @ V.T          # diag(target) . Sigma^-1/2

    out_w = (w - mu) @ Tw.T + mu
    return out_w @ np.linalg.inv(M).T

def store(y):
    """+/-0.5 then truncate then clip -- iDecorrStore, see note 1."""
    y = np.where(y < 0, y - 0.5, y + 0.5)
    return np.clip(np.trunc(y), 0, 255).astype(np.uint8)

x = src.astype(np.float64)
for name, M in (("dstretch_rgb.ppm", np.eye(3)),
                ("dstretch_yuv.ppm", YCBCR)):
    y = decorrelate(x, M, SCALE)
    write_ppm(name, store(y))
    print("%-20s worst excursion before clipping %7.2f .. %7.2f"
          % (name, y.min(), y.max()))
PY

echo "done -- remember .gitattributes and the fixtures[] array in test_golden.cpp"
