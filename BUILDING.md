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

Disable any of them with e.g. `-DIM_BUILD_JP2=OFF`.

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
