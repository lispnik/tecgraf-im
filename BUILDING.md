# Building IM from source

IM links against system image libraries — there is no bundled
fallback. The build is driven by CMake (3.20 or newer).

## macOS (Homebrew)

```sh
brew install cmake ninja pkg-config \
             libtiff jpeg-turbo libpng libexif liblzf lz4 \
             jasper fftw libomp

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `build/lib/libim*.dylib` with absolute install_names — no
post-build fix-up step needed. The dylibs are loadable from any
runtime (SBCL/CFFI, Python ctypes, etc.) without an embedded rpath.

## Linux (Debian/Ubuntu)

```sh
sudo apt-get install cmake ninja-build pkg-config \
                     libtiff-dev libjpeg-dev libpng-dev \
                     libexif-dev liblzf-dev liblz4-dev \
                     libfftw3-dev libomp-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIM_BUILD_JP2=OFF
cmake --build build -j
```

`libjasper-dev` was dropped from Debian/Ubuntu after 18.04 due to
unfixed CVEs, so the JP2 add-on is disabled with `-DIM_BUILD_JP2=OFF`
on modern Linux distros. Install jasper from source if you need it.

## Windows (vcpkg)

```cmd
vcpkg install tiff jpeg-turbo libpng libexif liblzf lz4 jasper fftw3

cmake -S . -B build -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
cmake --build build -j
```

vcpkg's CMake toolchain handles all `find_package` and pkg-config
lookups; no other configuration is required.

## Build options

| Option                 | Default | Notes                                |
|------------------------|---------|--------------------------------------|
| `IM_BUILD_PROCESS`     | ON      | Image processing operations          |
| `IM_BUILD_PROCESS_OMP` | ON      | OpenMP-enabled parity build          |
| `IM_BUILD_JP2`         | ON      | JPEG 2000 support (needs libjasper)  |
| `IM_BUILD_FFTW3`       | ON      | FFTW3-backed FFT add-on              |
| `IM_BUILD_LUA`         | ON      | Lua 5.x bindings (imlua + add-ons)   |
| `IM_BUILD_HEIF`        | OFF     | HEIC/AVIF support (needs libheif)    |
| `IM_BUILD_CAPTURE`     | OFF     | Video capture (real only on macOS)   |

Disable any of them with e.g. `-DIM_BUILD_JP2=OFF`.

## HEIC / AVIF, and why it is off by default

`IM_BUILD_HEIF=ON` builds `libim_heif`, adding the `HEIF` (*.heic, *.heif) and
`AVIF` (*.avif) drivers. It needs libheif 1.17 or newer:

```sh
brew install libheif           # macOS
sudo apt install libheif-dev   # Debian/Ubuntu
vcpkg install libheif          # Windows
```

It is the only add-on defaulting to OFF, because of what libheif links rather
than libheif itself:

| Codec    | Used for      | Licence  |
|----------|---------------|----------|
| libheif  | the container | LGPL-3.0 |
| libde265 | HEIC decode   | LGPL-2.1 |
| **x265** | HEIC encode   | **GPL-2.0** |
| aom      | AVIF          | BSD-2    |

IM is MIT. Dynamically linking the LGPL parts leaves that intact, but a binary
whose link closure includes **x265 must be distributed under the GPL**, which
would carry over to anyone redistributing your build. Homebrew's libheif links
x265 today, so on macOS you get it by default -- check with:

```sh
otool -L $(brew --prefix libheif)/lib/libheif.dylib | grep x265
```

To keep IM's MIT terms intact, either build libheif without x265 and let it
load an encoder plugin at run time (libheif 1.16+), or use AVIF for writing,
which has no such constraint. Decoding HEIC needs only the LGPL libde265.

Separately, HEVC is covered by patent pools (MPEG LA / Access Advance) that may
require a licence for commercial distribution regardless of software licence.
AVIF is royalty free.

## Video capture, and the camera permission it needs

`-DIM_BUILD_CAPTURE=ON` builds `libim_capture`. What you get depends on the
platform, because the backend is chosen at build time -- there is no base class
and no runtime selection, just one translation unit per platform implementing
the 27 functions in `include/im_capture.h`.

| Platform | Backend | Result |
|----------|---------|--------|
| macOS | `src/im_capture_avf.mm`, AVFoundation | real capture |
| Windows | `src/im_capture_none.cpp` | reports no devices |
| Windows + `-DIM_CAPTURE_DIRECTSHOW=ON` | `src/im_capture_dx.cpp` | needs SDKs from 2008, see below |
| Linux and everything else | `src/im_capture_none.cpp` | reports no devices |

The stub is not a placeholder to be embarrassed about: `imVideoCaptureCreate`
is documented to return `NULL` when there is no camera and
`imVideoCaptureDeviceCount` to return a count that may be zero, so every caller
already handles that state. Having it means the exported symbol set is the same
everywhere and you can link `libim_capture` unconditionally.

The macOS backend links `AVFoundation`, `CoreMedia`, `CoreVideo` and
`Foundation`, all system frameworks, and compiles one Objective-C++ file -- the
only one in the tree, which is why `OBJCXX` is enabled lazily rather than in
the top-level `project()`.

The DirectShow backend needs `qedit.h`, which Microsoft removed from the
Windows SDK after 6.1 (2008), plus DirectX SDK 9.15. It has never been compiled
by this tree. It is behind its own option so that turning capture on does not
break a Windows build that has no way to satisfy it.

### The camera permission is not optional, and not catchable

**A program that uses `libim_capture` on macOS must declare
`NSCameraUsageDescription`, and must be attributed to itself.** If it does not,
TCC does not return an error -- it kills the process with `SIGABRT`, from inside
the first call that touches the camera, and nothing in the library can catch it
or report it:

```
This app has crashed because it attempted to access privacy-sensitive data
without a usage description.
```

Enumerating devices is safe and raises no prompt: `imVideoCaptureDeviceCount`
and the `imVideoCaptureDevice*` functions can be called from anything. It is
`imVideoCaptureConnect` that touches the camera.

Embedding the key in your own binary is **not** enough on its own. TCC
attributes the request to the *responsible* process, which for anything started
from a shell is the terminal, and neither Terminal.app nor iTerm nor Emacs
declares a camera usage string. Measured: a command line binary dies even with
`NSCameraUsageDescription` in its own `__TEXT,__info_plist` section and covered
by its code signature.

What works is an app bundle launched through LaunchServices:

```sh
mkdir -p Grab.app/Contents/MacOS
cp your_program Grab.app/Contents/MacOS/Grab
cat > Grab.app/Contents/Info.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>Grab</string>
  <key>CFBundleIdentifier</key><string>com.example.grab</string>
  <key>NSCameraUsageDescription</key><string>Captures video frames.</string>
</dict></plist>
PLIST
codesign --force --deep -s - Grab.app
open -W --stdout /dev/stdout -a "$PWD/Grab.app"
```

The first run raises the permission dialog. Until it is answered, AVFoundation
does not fail: it **delivers black frames**, which is why `imVideoCaptureConnect`
checks the authorization status itself and returns 0 rather than handing back a
plausible all-black image.

Alternatively, grant camera access to your terminal in System Settings ->
Privacy & Security -> Camera, and every command line program it launches
inherits it.

### Changing the capture size often does not work

`imVideoCaptureGetFormat` lists the sizes a device's session accepts and
`imVideoCaptureSetImageSize` sets one, but on macOS accepting is not honouring.
Measured against AVFoundation directly, on a FaceTime HD camera: every
size-named preset is accepted, reads back as set, and the session goes on
delivering the sensor's native 1920x1080 — including when a matching
`AVCaptureDevice.activeFormat` is set alongside it. That is the framework's
behaviour with an `AVCaptureVideoDataOutput` on that hardware, not something
the library can work around.

So treat `GetFormat` as candidates and `SetImageSize`'s return value as the
answer. It starts the session to find out what actually arrives and returns 0
for anything else, rather than reporting a success the caller would only
discover was false when the frames came back the wrong size. Expect it to
refuse — on some cameras, everything except the size it is already at.

### Testing capture

`ctest` covers what can be covered without hardware, which is more than it
sounds: the device-list contract runs everywhere, and the pixel conversion --
the part where a mistake produces a *plausible* image rather than an obvious
failure -- is exercised on synthetic frames with known contents, so the
bottom-up flip, the row stride, the channel order and the luma are all pinned
with no camera at all.

Nothing in the suite calls `imVideoCaptureConnect`, deliberately: `im_tests` is
a bundle-less binary, so a case that connected would abort the whole suite
rather than fail itself.

## Lua bindings

`IM_BUILD_LUA=ON` adds four shared libraries with the Lua-loader
prefix convention (`imlua.so`, no `lib` prefix):

| Module          | Brings in                         |
|-----------------|-----------------------------------|
| `imlua`         | core IM API                       |
| `imlua_process` | image processing operations       |
| `imlua_jp2`     | JPEG 2000 (if `IM_BUILD_JP2`)     |
| `imlua_fftw3`   | FFTW3-backed FFT (if `IM_BUILD_FFTW3`) |

CMake's stock `find_package(Lua)` picks whatever Lua is on the
system. Force a specific version (e.g. Lua 5.4 instead of 5.5) by
passing the include and library paths:

```sh
cmake -S . -B build \
  -DLUA_INCLUDE_DIR=/opt/homebrew/opt/lua@5.4/include/lua5.4 \
  -DLUA_LIBRARY=/opt/homebrew/opt/lua@5.4/lib/liblua5.4.dylib
```

The bindings embed their helper `.lua` scripts as `IMLUA_USELH` blob
headers (precomputed under `src/lua5/lh/`), so the runtime does not
need to find any `.lua` file at load time.

## Installing

```sh
cmake --install build --prefix /usr/local
```

Headers go to `<prefix>/include`, libraries to `<prefix>/lib`, and a
CMake export package is written to `<prefix>/lib/cmake/IM/` so
consumers can:

```cmake
find_package(IM REQUIRED)
target_link_libraries(my_app PRIVATE IM::im IM::im_process)
```

## Tecmake (legacy)

The pre-CMake build used `tecmake` makefiles; those are no longer
present in this tree. The Lua bindings, AVI/WMV/Capture/ECW format
add-ons, and the test programs under `test/` still have stale
`*.mak` files for tecmake — they are not currently built. Port to
CMake as needed.
