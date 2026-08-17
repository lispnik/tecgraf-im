# Changes in this fork

Changes made in this fork that a consumer of the library needs to know about.
Upstream Tecgraf's own history is in `html/en/history.html` and is not
duplicated here.

Everything below is user-visible: behaviour a caller can observe, or a header
that changed shape. Test coverage, CI and internal refactoring are in the git
log rather than here.

---

## Incompatible changes

These alter results or layout for code that already compiles. Read this
section before upgrading.

### `IM_BIT_XOR` now computes an exclusive or

`imProcessBitwiseOp` and `imProcessBitMask` with `IM_BIT_XOR` used to compute
`~(a | b)`, which is NOR. The enum documented that formula, immediately beside
a name saying otherwise, and no exclusive or was reachable at all.

`IM_BIT_XOR` is now `a ^ b`. **`IM_BIT_NOR` is new and carries the old
behaviour.**

- Code that deliberately wanted `~(a | b)`, having read the comment, must
  change to `IM_BIT_NOR`.
- Code that wrote `IM_BIT_XOR` meaning exclusive or is fixed by this change
  and needs nothing.

The Lua binding gains `im.BIT_NOR` alongside `im.BIT_XOR`.

`imProcessBitMask` also downgrades a binary target to gray more accurately: OR
and XOR can only push a sample above 1 when the mask does, but NOR always can,
so it no longer consults the mask for that case.

### A complex phase is measured from the real axis

`cpxphase` returned `atan2(C.real, C.imag)`. `atan2` takes `(y, x)`, so the
argument of a complex number is `atan2(imag, real)`; with the arguments
swapped it measured the angle from the *imaginary* axis, turning `(1,0)` into
pi/2 and `(0,1)` into 0. The complex logarithm carried the same swap.

Affects `imConvertDataType` with `IM_CPX_PHASE`, and `imProcessSplitComplex`
in polar mode. Any stored phase data produced by an older build is wrong by
this transformation.

### Polar complex merge reads radians

`imProcessMergeComplex` in polar mode read its phase as degrees, while
`imProcessSplitComplex` writes what `cpxphase` returns, which is radians. A
polar split and merge did not round-trip. Merge now reads radians, so the two
agree and the round trip is exact.

### `imFile::compression` is 16 bytes

Was `char[10]`, which is three short of `"ADOBEDEFLATE"` and one short of
`"THUNDERSCAN"` — both entries in the TIFF driver's own advertised
compression table, so passing either to `imFileSetInfo` overflowed the field
into `image_count` beside it. The copy is bounded as well now.

This changes the layout of `imFile`. Rebuild anything that includes
`im_file.h`. Two callers receive the field by `strcpy` into a local buffer;
both were checked and the one that was then too small, in `imFileFormat`, was
widened with it.

### Point operation callbacks receive correct coordinates

The `x`, `y` and `d` handed to callbacks of `imProcessUnaryPointOp`,
`imProcessUnaryPointColorOp`, `imProcessMultiPointOp` and
`imProcessMultiPointColorOp` were computed with the plane index and the row
index inverted, and identified no pixel in the image from the second sample
onwards. Sample *values* were never affected.

A callback written to compensate for the old coordinates will need its
workaround removed. One that ignored them, as everything in this tree did, is
unaffected.

### `imConvertDataType` reports success for a same-type conversion

It returned `IM_ERR_DATA` as soon as the source and target data types
matched — the same code it returns for a genuine colour space mismatch, so a
caller could not tell a no-op from a real error. `im_convert.h` says "if data
type is the same nothing is done", and `imConvertColorSpace` has always
returned `IM_ERR_NONE` for the equivalent case in its own dimension. It now
returns `IM_ERR_NONE`.

### The tone gamut rounds instead of truncating

`imProcessToneGamut` computes in double and casts back to the target type.
That cast truncated, biasing every normalized operation downward by up to one
unit. Results on integer images may differ by 1 from an older build.

The visible consequence was that `imProcessNegative` was not its own inverse:
`(1.0 - 254.0/255.0) * 255.0` is `0.9999999999999964`, so 254 inverted to 0
rather than 1. It is an involution now.

### `imFileFormat` returns the format the file actually is

The old-API `imFileFormat` reported the wrong format for every file in the
library. `FormatNew2Old` compares names with `imStrEqual`, which returns 1 for
equal, and all nine of its tests were negated — so each branch fired when the
name did **not** match. A BMP came back as `IM_GIF` and everything else as
`IM_BMP`.

`IM_BMP` being the value that came out for almost everything is what makes
this worth reading twice: code that only ever handled BMP saw correct results,
and code that switched on the return value has been taking the wrong branch
for every other format. Anything written to compensate must be undone.

`imImageInfo`, `imLoadRGB` and `imLoadMap` were unaffected — they open the
file rather than consulting this mapping.

### `im::Image`'s `as_bitmap` constructor selects the loader it names

`im::Image(file_name, index, error, as_bitmap)` had its branches inverted:
`as_bitmap` selected `imFileImageLoad` and clearing it selected
`imFileImageLoadBitmap`. `im::File::LoadFrame` takes the same flag and has
always dispatched it correctly. Code that passed the flag inverted to
compensate must stop.

---

## Fixed

Defects where the old behaviour was not something anything could reasonably
have depended on.

- **`imPaletteFindNearest` could not find anything.** Its running minimum was
  seeded with `(unsigned int)-1` into an `int`, so the comparison against a
  squared distance never held and every colour without an exact match returned
  `-1` — the one value a caller cannot use as an index.
- **`imProcessCompose` lost the alpha channel above 8 bits.** It cast to
  `unsigned char` when setting an opaque target, so an opaque `IM_USHORT`
  source composed to alpha 255 instead of 65535 while the colour planes came
  out correct.
- **`DoUnaryOpByte` wrote nothing for four operations.** `IM_UN_CONJ`,
  `IM_UN_CPXNORM`, `IM_UN_POSITIVES` and `IM_UN_NEGATIVES` fell through its
  switch, so a byte destination kept whatever it already held.
- **PNG crashed when opened and closed without reading.** Neither `png_ptr`
  nor `info_ptr` was initialised and `Open()` creates only the first, so
  `imFileGetInfo` followed by `imFileClose` handed libpng an uninitialised
  pointer.
- **GIF read past a global on every interlaced image**, on both the read and
  write paths.
- **`im::AttribTable`'s copy constructor** copied into an uninitialised
  pointer.
- **`im::Image::Duplicate()` leaked.** The wrapper counts references in an
  image attribute, and everything that copies attributes into a fresh image
  copied the count, so a duplicate started at 1, was incremented to 2, and
  never reached zero.
- **`imAnalyzeMeasureHoles` overflowed the heap whenever hole perimeters were
  asked for**, which through the C++ wrapper is every call. It allocated
  `holes_count * sizeof(int)` and passed the buffer to
  `imAnalyzeMeasurePerimeter`, which writes that many `double`s and memsets all
  of them first — exactly half the memory needed. The line above it allocates
  a genuinely `int`-sized buffer for `imAnalyzeMeasureArea`, and this was
  copied from it.
- **PCX read past the end of its line buffer on 24-bit images.** The buffer
  reserves `3*width` bytes of scratch, but the copy back out of it uses the
  file's line width padded to an even number, times three — so any odd width
  over-read. A 37-pixel line allocates 224 bytes and the copy wants 227.
- **PCX copied overlapping ranges with `memcpy`.** Three copies within one
  line buffer had overlapping source and destination, which is undefined even
  where the bytes happen to come out right. They use `memmove` now. This is
  also what was masking the over-read above: AddressSanitizer reports the
  overlap first and stops.
- **A codec's refusal is reported as `IM_ERR_COMPRESS`.** The HEIF driver
  mapped libheif's codec-plugin and encoding errors to `IM_ERR_ACCESS`, which
  made a codec limitation indistinguishable from an I/O problem.

---

## Hardened

Undefined behaviour that is now defined. Correct callers see no difference.

- **Integer division by zero.** `IM_UN_INV` and `IM_BIN_DIV` reached `1/0` and
  `a/0` for any zero sample — a black pixel. That is undefined, and undefined
  in the way that diverges between machines: SIGFPE on x86, a quiet 0 on ARM.
  Both yield 0 now, which is what the ARM builds always produced.
- **`imConvertMapToRGB` bounds its palette decode tables.** It decodes into
  three 256-entry arrays indexed by a byte, and both ends were unbounded. A
  palette longer than 256 filled past the end of them — a stack overflow
  reachable from a documented public function, since nothing states a limit.
  An index beyond a short palette read whatever the stack held, which is
  uninitialised rather than out of bounds, so no sanitizer sees it and the
  only symptom is output that differs between runs. Such an index now
  resolves to black; the value matters less than it being the same every time.
- **Type and size preconditions are enforced.** Fifty-two entry points across
  arithmetic, colour, geometry, convolution, morphology, thresholding, logic
  and tone gamut gained a guard, covering more again through the operations
  that delegate to them — the eighteen public functions in `im_convolve.cpp`
  are reached through seven, and the eight in `im_morphology_gray.cpp` through
  one. All of them write the destination through a pointer of the *source's*
  type, or loop over the *source's* dimensions, without consulting the
  destination. A narrower or smaller target overran its buffer. Each header
  stated the rule; none verified it. They now assert in a debug build and
  return without writing in a shipped one, matching what `imProcessCompose`
  has always done for a missing alpha channel.

  Documented misuse that used to corrupt memory now does nothing. Correct
  calls are unaffected — including the ones where a narrowing or differing
  target is legitimate, such as `IM_DOUBLE` to `IM_FLOAT` arithmetic, the
  binary result of a threshold, and the changed depth of
  `imProcessUnaryPointColorOp`.

---

## Documentation

Corrections where the code was right and the header was not, so nothing built
against this library changes behaviour.

- **`imKernelGradian3x3` was pictured upside down.** The matrices in
  `im_kernel.h` are pictures of each kernel, first row topmost, while the
  arrays in `im_kernel.cpp` are in memory order, which is bottom-up — so a
  picture is the vertical mirror of the literal beside it. Sixteen of the
  twenty-one kernels are symmetric about the horizontal axis and cannot tell
  the difference; of the five that can, four followed the convention and
  `imKernelGradian3x3` did not, which read as a picture inverted the sign of
  the gradient it computes. The convention is now stated where the group is
  introduced, since the trap is that reconciling the header against the source
  by flipping the array instead would silently invert the other four.
  `imKernelGradian3x3` also measures the vertical difference where
  `imKernelGradian7x7` measures the horizontal one; that is unchanged, and now
  said out loud.
- **The attribute and format list functions lend their strings.** `imFormatList`,
  `imFormatCompressions`, `imFileGetAttributeList` and
  `imImageGetAttributeList` fill the caller's array with pointers the library
  owns, not copies, and each has its own rule for how long they stay valid.
  Freeing them corrupts the heap; all four now say so.

---

## Build

- **libheif 1.17 or newer** is required for the optional HEIF/AVIF driver.
  The true minimum is 1.16, where `heif_brand2_avif` was added; 1.17 is what
  Ubuntu 24.04 ships and therefore what CI exercises. Ubuntu 22.04 carries
  1.12 and builds without the driver.
- **Windows takes pkg-config from vcpkg rather than Chocolatey.** The
  Chocolatey package ships no checksum and is fetched over plain HTTP;
  Chocolatey eventually stopped permitting that by default and broke the build
  outright. vcpkg was already in use for every other dependency and has a
  `pkgconf` port, so the second package manager and the unverified download
  are both gone.
- **A CI job builds with asserts enabled.** Every other job builds `Release`
  or `RelWithDebInfo`, which define `NDEBUG` and compile out every `assert` in
  the library, so they had never been compiled at all.
