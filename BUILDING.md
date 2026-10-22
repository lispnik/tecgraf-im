# Building IM from source

IM links against system image libraries — there is no bundled
fallback. The build is driven by CMake (3.20 or newer).

## macOS (Homebrew)

```sh
brew install cmake ninja pkg-config \
             libtiff jpeg-turbo libpng libexif liblzf lz4 \
             jasper fftw libomp webp jpeg-xl openexr cfitsio

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
                     libfftw3-dev libomp-dev \
                     libwebp-dev libjxl-dev libopenexr-dev libcfitsio-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIM_BUILD_JP2=OFF
cmake --build build -j
```

`libjasper-dev` was dropped from Debian/Ubuntu after 18.04 due to
unfixed CVEs. The JP2 add-on defaults to OFF for that reason, so nothing
needs to be passed
on modern Linux distros. Install jasper from source if you need it.

Ubuntu 22.04 and older do not package libjxl at all, and their OpenEXR is 2.x
where the driver needs 3.1; add `-DIM_BUILD_JXL=OFF -DIM_BUILD_EXR=OFF` there.

## Windows (vcpkg)

```cmd
vcpkg install tiff jpeg-turbo libpng libexif liblzf lz4 jasper fftw3 libwebp libjxl openexr cfitsio

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
| `IM_BUILD_JP2`         | OFF     | JPEG 2000 support (needs libjasper)  |
| `IM_BUILD_FFTW3`       | ON      | FFTW3-backed FFT add-on              |
| `IM_BUILD_LUA`         | ON      | Lua 5.x bindings (imlua + add-ons)   |
| `IM_BUILD_HEIF`        | OFF     | HEIC/AVIF support (needs libheif)    |
| `IM_BUILD_WEBP`        | ON      | WebP support (needs libwebp)         |
| `IM_BUILD_JXL`         | ON      | JPEG XL support (needs libjxl)       |
| `IM_BUILD_EXR`         | ON      | OpenEXR support (needs OpenEXR 3.1+) |
| `IM_BUILD_FITS`        | ON      | FITS support (needs cfitsio)         |
| `IM_BUILD_CAMRAW`      | OFF     | Camera raw, read only (needs LibRaw) |
| `IM_BUILD_CAPTURE`     | OFF     | Video capture (real only on macOS)   |
| `IM_BUILD_AVI`         | OFF     | AVI driver (Windows + `vfw.h`)       |
| `IM_BUILD_WMV`         | OFF     | WMV driver (Windows + `wmsdk.h`)     |

Disable any of them with e.g. `-DIM_BUILD_FFTW3=OFF`.

## JPEG 2000, and why it is off by default

The only JP2 backend here is jasper. Debian and Ubuntu dropped it after 18.04
over unfixed CVEs, and it has been maintenance-only upstream for years, so on
most platforms the dependency is either absent or unwelcome.

It is also noisy in a way that reaches end users: `imFormatRegisterJP2` calls
`jas_init`, which writes a deprecation notice and a memory-limit warning to
stderr in every process that loads the driver. A consumer's command-line tool
prints those on each invocation, having asked for nothing but a PNG.

Turn it on with `-DIM_BUILD_JP2=ON` where jasper is available and wanted.

No CI job builds it, so `libim_jp2` appears in no published artifact and the
driver is not exercised by CI. The format tests skip cleanly when the module
is absent, which is what the Linux jobs -- which have never had jasper -- have
been demonstrating all along.

## AVI and WMV

Both drivers are Windows-only: they are built on the `imDib*` family, which is
itself compiled into `libim` only on Windows. Turning either on elsewhere logs a
status line and builds nothing.

Even on Windows each one is gated on a header probe, because the two SDKs are
not equally obtainable:

| Driver | Needs | Availability |
|---|---|---|
| `libim_avi` | `<vfw.h>`, `vfw32.lib` | Ships in the current Windows SDK. Builds on a stock runner. |
| `libim_wmv` | `<wmsdk.h>`, `wmvcore.lib` | Windows Media Format 11 SDK — a **separate download**, documented by Microsoft as superseded by Source Reader / Sink Writer. Not present on CI runners. |

So `-DIM_BUILD_WMV=ON` on a machine without that SDK is not an error; the
configure step reports

```
IM_BUILD_WMV ignored: <wmsdk.h> not found. It ships in the Windows Media
Format 11 SDK, which is a separate download from the Windows SDK.
```

and carries on. The configure summary at the end reports whether each target
was actually created, rather than what was requested — an `ON` next to a
library that was skipped would be the more misleading answer.

These targets exist because `imFormatRegisterAVI` and `imFormatRegisterWMV`
were declared in `include/`, listed in `src/im_avi.def` and `src/im_wmv.def`,
and documented — but had no target, so nothing built them and every consumer
had two dead entry points.

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

## WebP and JPEG XL

`libim_webp` adds the `WEBP` driver (*.webp) over libwebp 1.2+, and
`libim_jxl` the `JXL` driver (*.jxl) over libjxl 0.7+. Both libraries are
BSD-licensed with no GPL codec behind them, which is why these default ON
where HEIF defaults OFF. Register them with `imFormatRegisterWebP()` and
`imFormatRegisterJXL()`, or `require"imlua_webp"` / `require"imlua_jxl"`.

What each carries:

| | WebP | JPEG XL |
|---|---|---|
| Data types | byte | byte, ushort, float |
| Colour | RGB (gray is stored as RGB) | gray, RGB |
| Alpha | yes | yes |
| Lossless | `VP8L` compression, or `Lossless=1` | `Lossless=1`, exact for every type |
| Multiple images | animated files read frame by frame | first frame only |

`Quality` (0-100) sets the lossy level for both: cwebp's scale for WebP
(default 75), and cjxl's `-q` scale for JPEG XL (default 90, Butteraugli
distance 1.0). The headers, `include/im_format_webp.h` and
`include/im_format_jxl.h`, have the detail.

## OpenEXR and Radiance HDR

Two floating point formats, for images whose values do not stop at 1.

`libim_exr` adds the `EXR` driver (*.exr) over OpenEXR 3.1+, found through
its CMake package. Register it with `imFormatRegisterEXR()` or
`require"imlua_exr"`. It reads half, float and uint channels into `IM_FLOAT`
-- RGB, luminance, luminance/chroma, a named layer, or a lone channel, plus
alpha -- over the data window, tiled or scanline. It writes float by default,
which is exact under the lossless compressions, or half with `Half=1`.

`HDR` (*.hdr) is built into `libim` with no dependency: Radiance RGBE, the
format light probes and environment maps usually come in. It reads and writes
`IM_FLOAT` RGB and XYZ, run-length coded or flat, at RGBE's precision of about
1 part in 256 of each pixel's brightest channel.

## QOI

`QOI` (*.qoi) is built into `libim`, written from the one-page specification
with no dependency: lossless 8 bit RGB and RGBA, gray and binary written as
RGB.

## FITS

`libim_fits` adds the `FITS` driver (*.fits, *.fit, *.fts) over NASA's
cfitsio 4.0+. Register it with `imFormatRegisterFITS()` or
`require"imlua_fits"`. Every 2D plane of every image HDU is one image (a three
plane cube is one RGB image instead), BZERO/BSCALE are applied, and
tile-compressed files are read transparently. It writes byte, short, ushort,
int, float and double, uncompressed or with GZIP or (integers only) Rice tile
compression -- floating point is never quantized.

## Camera raw, and why it is off by default

`-DIM_BUILD_CAMRAW=ON` builds `libim_camraw`, a read-only `CAMRAW` driver over
LibRaw 0.20+ (the reentrant `libraw_r`) for DNG and the several hundred
proprietary formats LibRaw knows:

```sh
brew install libraw            # macOS
sudo apt install libraw-dev    # Debian/Ubuntu
vcpkg install libraw           # Windows
```

LibRaw is LGPL-2.1 or CDDL-1.0. Linking it dynamically leaves IM's MIT terms
alone, but whoever redistributes the result takes on the LGPL's obligations
for LibRaw itself, so it is opted into rather than shipped by default -- the
same reasoning as HEIF, with a lighter consequence. CI builds it everywhere;
the release archives do not.

It is called `CAMRAW` because IM already has a `RAW` driver for headerless
sample data. Output is 16 bit sRGB with the camera's white balance; set
`RawLinear` for scene-linear data or `RawHalfSize` for a fast quarter-area
preview, between `imFileOpen` and `imFileReadImageInfo`.

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

### Camera controls, and why your camera probably has none

`imVideoCaptureGetAttribute` and its three companions are implemented on macOS,
over **CoreMediaIO** rather than AVFoundation. That is not an arbitrary choice:
IM's model is DirectShow's — ask the device for a range, then get and set a
value inside it, which is what a percentage needs — and AVFoundation on macOS
never exposes a range. It gives mode enums, every property carrying an actual
value is iOS-only, and brightness, contrast, hue, saturation, sharpness, gamma,
gain, iris, pan and tilt are absent from it on every platform. CoreMediaIO's
`NativeRange`, `NativeValue` and `AutomaticManual` are `GetRange`, `Get`/`Set`
and `Flags_Auto` under other names.

**How much you get depends entirely on the camera, and a Mac's own camera gives
nothing.** Measured here:

| Camera | Controls |
|---|---|
| Logitech BRIO (USB/UVC) | brightness, contrast, saturation, sharpness, gain, backlight compensation, exposure, zoom, focus — 9 of the 20 documented names |
| Built-in FaceTime HD camera | **none** — the device reports zero owned objects |
| iPhone over Continuity | **none** |

So an empty `imVideoCaptureGetAttributeList` is the normal answer on a laptop,
not a fault. The API is built for this: a zero from `imVideoCaptureGetAttribute`
is the documented way to ask whether an attribute is supported, and the list
only ever contains controls the device really has.

**Nothing you can call will change that**, and it is worth knowing why before
going looking. Each camera is serviced by a CoreMediaIO system extension —
`com.apple.cmio.uvcassistantextension` for USB cameras, and separate Apple
extensions for the built-in camera and for Continuity — and whether controls
exist is that extension's choice, not the client's. Apple's UVC extension
republishes a camera's UVC processing-unit controls as CoreMediaIO feature
controls; Apple's extensions for its own cameras publish none, because the
exposure and white balance decisions live inside the image pipeline rather than
being exposed as adjustable values. This is not the legacy-versus-modern
plug-in split it looks like: the USB camera is served by a modern extension
too.

The practical test for "will this camera have controls" is the transport type —
a `usb` camera very likely does, a `bltn` one does not — but the only honest
answer is to ask, which is what `imVideoCaptureGetAttributeList` is for.

Measured with the laptop lid both open and closed, because a closed lid
suspends a built-in camera and it was worth ruling out as the cause: the
built-in camera reports zero controls either way.

```sh
open -W --stdout /dev/stdout -a build/lib/im_capture_grab.app --args 0 attrs
```

prints every documented name and what the connected device says about it.

Two behaviours worth knowing:

- **The attributes need no camera permission.** Ranges and values read back from
  a plain command line binary with no bundle and no TCC prompt — the opposite of
  `imVideoCaptureConnect`. Only opening a stream needs authorisation.
- **`imVideoCaptureResetAttribute` only honours its `fauto` argument.** The
  DirectShow backend also writes the driver's advertised default value;
  CoreMediaIO has no default-value property at all, so with `fauto` clear there
  is nothing truthful to write and the call returns 0 rather than inventing a
  value and reporting success.

Controls are a property of the device, not of your connection: another
application changing the brightness changes it for you too. That is how UVC
works, not a defect here.

### A closed lid, and other suspended cameras

A built-in camera on a laptop with the lid closed is *suspended*: it enumerates,
it connects, it reports its formats, it negotiates a size — and it delivers no
frames at all. CoreMediaIO documents exactly this, on
`kCMIODevicePropertySuspendedByUser`:

> the user might close the FireWire iSight's privacy iris or close the clamshell
> on a Mac Book or Mac Book Pro. While suspended the device still responds to all
> requests just as if it was active, but the stream(s) will not provide/accept
> any data.

Measured here, with the lid shut: `imVideoCaptureConnect` succeeds, the format
list is complete, a size is negotiated, and `imVideoCaptureFrame` times out
every time. Since every visible sign says the camera is working, the library
says so itself rather than leaving you to guess:

```
im_capture: device 1 is suspended -- it is connected and answering, but it will
deliver no frames until it is resumed. On a built-in camera this is a closed
laptop lid; on some cameras it is a privacy shutter.
```

Only some devices publish the property — a built-in camera and a USB camera do,
an iPhone over Continuity does not — so its absence means "no information", not
"not suspended".

### When the camera goes away

Unplug a camera mid-capture and `imVideoCaptureFrame` starts returning 0 and
`imVideoCaptureLive(vc, -1)` returns 0, with one line on stderr saying so. The
detection is a pair of synchronous property reads rather than a notification,
deliberately: this library is reached from plain C, from Lua, and from runtimes
that `dlopen` it, none of which is guaranteed to have a run loop for a
notification to arrive on.

Recovery needs a fresh connection, and a fresh *device list*:

```c
imVideoCaptureDisconnect(vc);
imVideoCaptureReloadDevices();      /* the device index may have changed */
imVideoCaptureConnect(vc, device);
```

`imVideoCaptureReloadDevices` is not optional. AVFoundation never revives an
`AVCaptureDevice` once it reports itself disconnected — when the same camera is
plugged back in it publishes a new one.

```sh
open -W --stdout /dev/stdout -a build/lib/im_capture_grab.app --args 0 watch
```

captures in a loop so you can pull the cable and watch it happen.

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
