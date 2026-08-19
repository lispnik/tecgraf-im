# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A modernized fork of the Tecgraf/PUC-Rio **IM** image processing library (C/C++ with Lua
bindings). The fork's substantive change is the build system: the legacy `tecmake`
makefiles were replaced with CMake, and all bundled third-party libraries were removed in
favor of system packages (except a 4-file `src/liblzf/` fallback used on Windows).

## Build

```sh
# macOS (Homebrew) — Lua 5.5 does not compile; pin 5.4 explicitly
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLUA_INCLUDE_DIR=/opt/homebrew/opt/lua@5.4/include/lua5.4 \
  -DLUA_LIBRARY=/opt/homebrew/opt/lua@5.4/lib/liblua5.4.dylib
cmake --build build -j

# Linux — libjasper was dropped from Debian/Ubuntu after 18.04 (unfixed CVEs)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIM_BUILD_JP2=OFF
cmake --build build -j

# Single target
cmake --build build --target im_process
```

Dependency install commands per platform are in `BUILDING.md`. Build options:
`IM_BUILD_PROCESS`, `IM_BUILD_PROCESS_OMP`, `IM_BUILD_JP2`, `IM_BUILD_FFTW3`,
`IM_BUILD_LUA` (all default ON), `IM_BUILD_HEIF` and `IM_BUILD_CAPTURE` (both
default OFF, see below), plus
`IM_BUNDLE_LZF` (default OFF) to force the vendored liblzf.

Everything lands in `build/lib/`.

## Tests

CTest drives everything; doctest 2.4.11 is vendored at `test/doctest/` (single header
plus upstream CMake scripts) so no CI job needs a new vcpkg port or network access.

```sh
cd build && ctest --output-on-failure          # all of it
ctest -R attrib --output-on-failure            # by name
ctest -LE known-bug                            # skip the known-bug probes
./lib/im_tests --test-case="*RemoveAll*"       # drive doctest directly
```

`-DBUILD_TESTING=OFF` drops the whole suite for consumers who only want the libraries.

Four conventions matter when adding tests:

- **A case that deliberately violates a documented precondition goes behind
  `#ifdef NDEBUG`.** The library's pattern is `assert(precondition)` followed by a
  real runtime guard, so the shipped builds cope and a debug build fails loudly at
  the point of the mistake. A case proving the *guard* therefore trips the assert
  when one is compiled in, and doctest cannot catch an abort. The `asserts` job in
  `ci-linux.yml` builds `Debug` specifically so the asserts are not dead code —
  without it, an assert whose condition disagreed with the guard beside it would
  never be noticed. Four regions are currently excluded this way; each says so and
  points at the note in `test_datatype.cpp`.

- **A bug you are not fixing yet gets a correct assertion, inverted.** Decorate the case
  `* doctest::should_fail()`: it states the behaviour that *should* hold, doctest reports
  "Failed as expected", and the suite stays green. When someone fixes the bug the case
  starts failing — the cue to delete the decorator, not to re-invert it. No cases are
  currently inverted; all six reviewed `im_attrib` bugs are fixed.
- **A bug that can only be seen by watching a process die needs its own process.** The
  pattern (since removed, see the note in `test/CMakeLists.txt`) was a small program that
  forks, with the parent treating a signal death *or* a non-zero exit as the expected
  outcome. Do not reach for CTest's `WILL_FAIL` — it does not invert a signal death, which
  is exactly how such cases fail. The non-zero-exit arm is what keeps it working under
  sanitizers, where ASan calls `exit(1)` instead of letting the signal through.
- **`test/smoke.lua` is the only check that reaches the bindings** and the `PREFIX ""`
  module-naming convention; a C++ binary cannot cover that. CMake passes it the library
  directory, the platform suffix, and only the add-on modules actually built.

`-DIM_ENABLE_SANITIZERS=ON` builds everything with ASan+UBSan (not supported for MSVC) and
runs in CI via `ci-sanitizers.yml`. Worth reaching for on this codebase — its
characteristic failure is a silent out-of-bounds read that returns plausible garbage
instead of crashing, which no assertion catches. One gotcha encoded in that workflow:
UBSan only *warns* unless `halt_on_error=1` is set, so without it findings sail past CI.

Leak detection is on in CI, but **do not copy `detect_leaks=1` to a local macOS run** —
Darwin has no LeakSanitizer and ASan aborts every binary with "detect_leaks is not
supported on this platform" before a single test runs. Locally use
`ASAN_OPTIONS=abort_on_error=1:strict_string_checks=1`; on macOS use `leaks --atExit --
./lib/im_tests` for the leak check instead.

`test/fixtures/` holds two small HEIC/AVIF files produced by libheif's `heif-enc`. They
exist because a round-trip through one driver passes just as happily when the read and
write paths are wrong in matching ways — decoding a file this tree did not write is the
only check that catches that.

Also available, but not run by CI and not assertions: `build/lib/document_enhance <in>
<out>` and `document_enhance_v2`, CLI demos with no expected output. Fixture images live
in `html/examples/` (`lena.jpg`, `rice.png`, `flower.jpg`); `imCalcRMSError`
(`include/im_process_ana.h:30`) lets the library diff images itself for golden-image
tests.

All four CI jobs run `ctest --test-dir build --output-on-failure`. On Windows the Lua
smoke test is not registered (vcpkg ships no `lua.exe`) and the `attrib.isolated.*` probes
exit 77 for lack of `fork()`, so CTest reports them as skipped rather than failed.

The other seven programs under `test/` are upstream sources not referenced by
`CMakeLists.txt`, but they split into two groups:

- `im_info.cpp`, `im_copy.c`, `im_copy.cpp` compile and link against `libim` **as-is**
  (verified). Their `im_format_{jp2,avi,wmv}.h` includes are declaration-only and the
  matching `imFormatRegister*()` calls are commented out. `im_info` is a handy
  file/format diagnostic — add it to CMake if you want it.
- `im_view.{c,cpp}`, `glut_capture.c`, `iupglview.c` genuinely need external toolkits
  (IUP, CD, GLUT/OpenGL, `libim_capture`) that this build neither produces nor depends on.

(`BUILDING.md` says these carry stale tecmake `.mak` files; those were deleted in
`f59cf54` and no `.mak` file remains in the tree.)

## Architecture

Layered, with each layer a separate shared library so consumers link only what they need:

- **`libim`** (`src/*.cpp`, `include/im*.h`) — core. Image representation (`imImage`),
  file I/O (`imFile`), color/data-type conversion, and one `src/im_format_*.cpp` per
  format. Formats self-register: `src/im_format_all.cpp` calls
  `imFormatRegister<NAME>()` for each built-in codec, so adding a format means a new
  `im_format_x.cpp`, a `imFormatRegisterX()` entry there, and a line in `IM_SOURCES`.
- **`libim_process`** (`src/process/`) — 100+ operations over `imImage`. Compiled twice:
  once plain, once as **`libim_process_omp`** from the identical source list with
  `OpenMP::OpenMP_CXX` linked in. Both share `src/im_process.def`. Note that
  `im_convertbitmap.cpp`, `im_convertcolor.cpp`, and `im_converttype.cpp` are compiled
  into *both* `libim` and `libim_process`.
- **`libim_jp2`** / **`libim_fftw3`** — optional single-file add-ons over jasper and
  fftw3.
- **`libim_heif`** (`src/im_format_heif.cpp`) — HEIC and AVIF over libheif, the newest
  add-on and the only one defaulting **off**: HEIC *encoding* links x265 (GPL-2.0), which
  would override IM's MIT terms for redistributors. `BUILDING.md` has the detail. It
  registers two drivers from one implementation and is the model to copy for a new format
  with a heavyweight dependency.
- **`libim_capture`** (`src/im_capture_avf.mm` on macOS, `src/im_capture_none.cpp`
  elsewhere) — live video capture, default **off**. Unlike the format drivers
  there is no registry and no base class: the contract is the 27 functions in
  `src/im_capture.def`, and each platform supplies one translation unit defining
  all of them plus its own `struct _imVideoCapture`. Only macOS has a real
  backend; the stub reports no devices so the symbol set is the same everywhere.
  `src/im_capture_dx.cpp` is the upstream DirectShow backend, behind
  `IM_CAPTURE_DIRECTSHOW` because it needs a 2008-era Windows SDK. **A program
  that captures on macOS must carry `NSCameraUsageDescription` and run from an
  app bundle, or TCC kills it uncatchably** — which is why no test may call
  `imVideoCaptureConnect`. See BUILDING.md.

  The camera attributes are the one part that is *not* AVFoundation: they go
  through **CoreMediaIO**, because AVFoundation exposes mode enums and no
  numeric ranges, and IM's attribute API is a percentage of a range. They also
  need no camera permission. How many a device has is hardware-dependent — a
  USB camera reports nine of twenty, a built-in Mac camera reports none — so an
  empty `imVideoCaptureGetAttributeList` is normal, not broken.
- **`imlua*`** (`src/lua5/`) — Lua bindings, one module per native library, built with
  `PREFIX ""` so they load as `imlua.so` not `libimlua.so`.

### Two things that bite

**Windows exports come from `.def` files, not `__declspec(dllexport)`.** Each library has
one (`src/im.def`, `src/im_process.def`, `src/lua5/imlua*.def`, …). A new public symbol
that isn't added to the matching `.def` will link fine on Unix and produce no import
library at all on MSVC.

**Lua helper scripts are embedded, not loaded at runtime.** `src/lua5/*.lua` are
precompiled into `src/lua5/lh/*.lh` C blobs, which the bindings `#include` under the
`IMLUA_USELH` define. Editing a `.lua` file changes nothing until the corresponding `.lh`
is regenerated with `src/bin2c.lua`; both are checked in.

### macOS dylib policy

`CMakeLists.txt` sets `CMAKE_INSTALL_NAME_DIR` to the build lib dir with
`CMAKE_BUILD_WITH_INSTALL_NAME_DIR=ON`, so in-tree dylibs carry absolute install_names and
are `dlopen`-able from runtimes with no rpath of their own (SBCL/CFFI, Python ctypes).
Don't replace this with an `@rpath` scheme — that consumer is the point of the fork.
`CMAKE_FIND_FRAMEWORK LAST` is also deliberate: framework-first search otherwise finds an
old libpng header inside third-party frameworks and yields a runtime ABI mismatch.

## Legacy code conventions

The core sources are decades-old C++ that uses `register` and other deprecated constructs;
`-Wno-deprecated-declarations -Wno-register` is set globally rather than modernizing them.
Match the surrounding style when editing — don't opportunistically modernize.

## CI

`.github/workflows/ci-{linux,macos,windows}.yml` build on Ubuntu 22.04/24.04,
macOS 14/15 (Apple Silicon only), and windows-2022 via vcpkg. Triggers are limited to
`master`, `cmake-build`, `macos-system-libs-and-fixes`, and `ci-*` branches.
