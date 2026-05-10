# Building IM from source

The IM build can either compile its own bundled copies of the image
libraries (libtiff, libjpeg, libpng, libexif, liblzf, libjasper, lz4)
or link against system-installed copies. The preferred path on every
platform is **system libs** — the bundled tree is kept only as a
fallback for environments without a package manager.

The toggle is `USE_SYSTEM_IMAGE_LIBS`. It defaults to `Yes` on macOS
and Linux; on Windows it must be opted in.

---

## macOS (Homebrew)

```sh
brew install libtiff jpeg-turbo libpng libexif liblzf jasper fftw libomp lz4
cd im/src
make
# (one-time post-build) rewrite dylib install_names so non-rpath
# loaders (e.g. CFFI/SBCL) can dlopen them.
cd ..
./fix-install-names.sh
```

Anything in Homebrew's `prefix/lib` is found automatically.

---

## Linux (Debian/Ubuntu)

```sh
sudo apt-get install build-essential pkg-config \
  libtiff-dev libjpeg-dev libpng-dev libexif-dev liblzf-dev \
  libfftw3-dev libomp-dev liblz4-dev
# libjasper-dev was dropped from Debian/Ubuntu after 18.04 due to
# unfixed CVEs. The im_jp2 add-on will fail to build unless you
# either install libjasper from source or skip it.
cd im/src
make im im_process im_process_omp im_fftw3
```

The `Makefile` builds each add-on as a separate target; `make im_jp2`
requires libjasper.

---

## Windows

The Windows build is supported through three dependency-source paths.
Pick whichever fits your toolchain:

### 1. vcpkg (recommended for MSVC)

```cmd
git clone https://github.com/microsoft/vcpkg
cd vcpkg && bootstrap-vcpkg.bat
vcpkg install tiff jpeg-turbo libpng libexif jasper fftw lz4
:: liblzf is not in vcpkg; the bundled copy is used automatically.

set VCPKG_ROOT=C:\path\to\vcpkg
set USE_SYSTEM_IMAGE_LIBS=Yes
cd im\src
nmake -f ..\tecmakewin.mak
```

The makefile picks `$(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/include`
and `.../lib`. `VCPKG_TRIPLET` defaults to `x64-windows`; override
for static builds (`x64-windows-static`) or 32-bit (`x86-windows`).

To enable JP2 support, install jasper and set
`USE_SYSTEM_JASPER=Yes`. For the headless `im_jp2.dll` the user is
responsible for placing `jasper.dll` and dependencies on `PATH`.

### 2. MSYS2 / mingw64 (GCC on Windows)

```sh
pacman -S mingw-w64-x86_64-libtiff mingw-w64-x86_64-libjpeg-turbo \
          mingw-w64-x86_64-libpng mingw-w64-x86_64-libexif \
          mingw-w64-x86_64-jasper mingw-w64-x86_64-fftw \
          mingw-w64-x86_64-lz4 mingw-w64-x86_64-libomp
# liblzf has no MSYS2 package; the bundled copy is used.

export MSYS2_PREFIX=/mingw64
export USE_SYSTEM_IMAGE_LIBS=Yes
cd im/src
make
```

### 3. Pre-built dependency drop (`WINDEPS_ROOT`)

If a maintainer ships a flat `windeps.zip` containing `include/`,
`lib/`, and (for runtime) `bin/`, point at it:

```cmd
set WINDEPS_ROOT=C:\path\to\windeps
set USE_SYSTEM_IMAGE_LIBS=Yes
nmake -f ..\tecmakewin.mak
```

This is what `vcpkg export` produces, repackaged.

### 4. Bundled fallback (no system libs)

Don't set any of `VCPKG_ROOT` / `MSYS2_PREFIX` / `WINDEPS_ROOT` and
don't set `USE_SYSTEM_IMAGE_LIBS`. The build compiles libtiff,
libjpeg, libpng, libexif, liblzf, lz4 (and libjasper for `im_jp2`)
from the bundled sources. **liblzf is always built from the bundled
tree on Windows** because no Windows package manager ships it.

---

## What's bundled

| Library    | Bundled path           | Win system?               | Unix system?         |
|------------|------------------------|---------------------------|----------------------|
| libtiff    | `src/libtiff/`         | vcpkg / MSYS2             | always               |
| libjpeg    | `src/libjpeg/`         | vcpkg / MSYS2             | always               |
| libpng     | `src/libpng/`          | vcpkg / MSYS2             | always               |
| libexif    | `src/libexif/`         | vcpkg / MSYS2             | always               |
| liblzf     | `src/liblzf/`          | always bundled            | always system        |
| libjasper  | `src/libjasper/` (v1)  | vcpkg                     | macOS Homebrew only  |
| libjasper2 | `src/libjasper2/` (v2) | (use system instead)      | (use system instead) |
| lz4        | `src/lz4/`             | vcpkg / MSYS2             | always               |

The bundled libtiff/libjpeg etc. are old (mid-2000s vintage) and do
not build cleanly against modern toolchains; the bundled fallback is
practically Windows-only at this point.

---

## Cross-compilation, special builds

`tecmakewin.mak` accepts the standard tecmake env vars
(`USE_OPENMP=Yes`, `USE_LUA51=Yes`, etc.). The vcpkg/MSYS2/WINDEPS
selection is orthogonal and stacks with those.
