cmake_minimum_required(VERSION 3.20)
project(im LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Old IM code uses the 'register' keyword and various other constructs
# whose deprecation warnings clang elevates to errors in -std=c++17.
# Silence those so we can keep the legacy sources without touching them.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-Wno-deprecated-declarations -Wno-register)
endif()

# Much of this tree predates any bounds checking, and its worst failure mode is
# a silent out-of-bounds read that returns plausible garbage rather than
# crashing (see the count-overflow path in src/im_attrib.cpp). A sanitizer run
# turns those into hard failures. Off by default; intended for a dedicated CI
# job and local debugging. Must be set before any target is defined so the
# libraries are instrumented too, not just the tests.
option(IM_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

if(IM_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "IM_ENABLE_SANITIZERS is not supported for MSVC builds.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# Default to a Release build when nothing is requested.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Single-source layout: all shared libraries go to lib/, archives same.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# macOS: install_name is an absolute path embedded in the dylib so it
# can be dlopen'd from runtimes without an embedded -rpath (e.g. SBCL).
# Pair with INSTALL_RPATH so consumers built via this tree resolve
# transitive deps without a separate fix-install-names pass.
if(APPLE)
    # By default CMake on macOS searches *.framework before normal
    # include paths, which means find_package(PNG) etc. can pick up
    # an old libpng inside a third-party framework (e.g. Mono's
    # libpng-1.4.12 framework header against Homebrew's libpng-1.6.x
    # dylib) and produce a hard-to-debug ABI mismatch at runtime.
    # Make frameworks the last resort.
    set(CMAKE_FIND_FRAMEWORK LAST)

    set(CMAKE_MACOSX_RPATH ON)
    # Embed the absolute build path as the install_name so the dylibs
    # are loadable from runtimes without their own rpath (e.g. SBCL via
    # CFFI). BUILD_WITH_INSTALL_NAME_DIR=ON applies INSTALL_NAME_DIR
    # during the build, not just at 'cmake --install' time, so the
    # in-tree dylibs are already loadable without a fix-up pass.
    set(CMAKE_INSTALL_NAME_DIR "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)
    set(CMAKE_INSTALL_RPATH
        "${CMAKE_LIBRARY_OUTPUT_DIRECTORY};/opt/homebrew/lib")
endif()

option(IM_BUILD_PROCESS     "Build libim_process (image processing add-on)"      ON)
option(IM_BUILD_PROCESS_OMP "Build libim_process_omp (OpenMP-enabled parity build)" ON)
option(IM_BUILD_JP2         "Build libim_jp2 (JPEG 2000 add-on via libjasper)"   ON)
option(IM_BUILD_FFTW3       "Build libim_fftw3 (FFTW3 backend for FFT)"          ON)
option(IM_BUILD_LUA         "Build Lua 5.x bindings (imlua, imlua_process, ...)" ON)

# Build-time dependencies (all required for libim).
find_package(PkgConfig REQUIRED)
find_package(TIFF REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)

# Use pkg-config for libexif (CMake's FindEXIF doesn't ship in the base
# distribution) and for liblz4 (likewise).
pkg_check_modules(LIBEXIF REQUIRED IMPORTED_TARGET libexif)
pkg_check_modules(LIBLZ4  REQUIRED IMPORTED_TARGET liblz4)

# IM source uses unqualified '#include "exif-data.h"', but libexif's
# headers live under <prefix>/include/libexif/. pkg-config returns
# only the parent, so add the subdirectory explicitly.
set(LIBEXIF_SUBDIR_INCLUDES "")
foreach(_d ${LIBEXIF_INCLUDE_DIRS})
    if(IS_DIRECTORY "${_d}/libexif")
        list(APPEND LIBEXIF_SUBDIR_INCLUDES "${_d}/libexif")
    endif()
endforeach()

# liblzf: Debian / Ubuntu ship a pkg-config file (header under
# <prefix>/include/liblzf/); Homebrew installs lzf.h at the include
# root with no pkg-config; vcpkg and MSYS2 don't ship liblzf at all.
# Try pkg-config first, then a manual probe, then fall back to the
# vendored 4-file copy under src/liblzf/ (only relevant for Windows
# in practice). The bundled fallback can be forced for testing with
# -DIM_BUNDLE_LZF=ON.
option(IM_BUNDLE_LZF "Always compile liblzf from src/liblzf/ instead of using a system copy" OFF)

if(NOT IM_BUNDLE_LZF)
    pkg_check_modules(LIBLZF QUIET IMPORTED_TARGET liblzf)
    if(NOT LIBLZF_FOUND)
        find_path(LIBLZF_INCLUDE_DIR lzf.h
            HINTS /opt/homebrew/include /usr/include /usr/local/include
                  /usr/include/liblzf)
        find_library(LIBLZF_LIBRARY NAMES lzf
            HINTS /opt/homebrew/lib /usr/lib /usr/local/lib)
    endif()
endif()

if(IM_BUNDLE_LZF OR (NOT LIBLZF_FOUND AND NOT LIBLZF_INCLUDE_DIR))
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/liblzf/lzf.h")
        message(STATUS "liblzf: compiling from bundled src/liblzf/")
        add_library(lzf_bundled STATIC
            src/liblzf/lzf_c.c
            src/liblzf/lzf_d.c)
        target_include_directories(lzf_bundled
            PUBLIC ${CMAKE_SOURCE_DIR}/src/liblzf)
        set_target_properties(lzf_bundled PROPERTIES
            POSITION_INDEPENDENT_CODE ON)
        set(IM_LZF_TARGET lzf_bundled)
    else()
        message(FATAL_ERROR "liblzf not found and no bundled tree at src/liblzf/.")
    endif()
endif()

# ----------------------------------------------------------------------
# libim (core)
# ----------------------------------------------------------------------

# On Windows, MSVC's link.exe only emits an .lib import library if
# the DLL actually exports symbols. IM relies on per-library .def
# files (rather than __declspec(dllexport) annotations) for that.
# Add each target's .def file to its source list on WIN32.
function(im_add_def_file target def_path)
    if(WIN32)
        target_sources(${target} PRIVATE "${def_path}")
    endif()
endfunction()

set(IM_SOURCES
    src/im_oldcolor.c        src/im_oldresize.c     src/im_converttype.cpp
    src/im_attrib.cpp        src/im_format.cpp      src/im_format_tga.cpp
    src/im_filebuffer.cpp    src/im_bin.cpp         src/im_format_all.cpp
    src/im_format_raw.cpp    src/im_convertopengl.cpp
    src/im_binfile.cpp       src/im_format_sgi.cpp  src/im_datatype.cpp
    src/im_format_pcx.cpp    src/im_colorhsi.cpp    src/im_format_bmp.cpp
    src/im_image.cpp         src/im_rgb2map.cpp     src/im_colormode.cpp
    src/im_format_gif.cpp    src/im_lib.cpp         src/im_format_pnm.cpp
    src/im_colorutil.cpp     src/im_format_ico.cpp  src/im_palette.cpp
    src/im_format_ras.cpp    src/im_convertbitmap.cpp src/im_format_led.cpp
    src/im_counter.cpp       src/im_str.cpp         src/im_convertcolor.cpp
    src/im_fileraw.cpp       src/im_format_krn.cpp  src/im_compress.cpp
    src/im_file.cpp          src/im_old.cpp         src/im_format_pfm.cpp
    src/im_format_tiff.cpp   src/im_format_png.cpp  src/im_format_jpeg.cpp
)

if(WIN32)
    list(APPEND IM_SOURCES
        src/im_sysfile_win32.cpp src/im_dib.cpp src/im_dibxbitmap.cpp)
else()
    list(APPEND IM_SOURCES src/im_sysfile_unix.cpp)
endif()

add_library(im SHARED ${IM_SOURCES})
im_add_def_file(im "${CMAKE_SOURCE_DIR}/src/im.def")

target_include_directories(im
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${LIBEXIF_SUBDIR_INCLUDES})

target_compile_definitions(im PRIVATE USE_EXIF)
target_link_libraries(im
    PUBLIC  TIFF::TIFF JPEG::JPEG PNG::PNG ZLIB::ZLIB
            PkgConfig::LIBEXIF
            PkgConfig::LIBLZ4)

if(IM_LZF_TARGET)
    target_link_libraries(im PRIVATE ${IM_LZF_TARGET})
elseif(LIBLZF_FOUND)
    target_link_libraries(im PRIVATE PkgConfig::LIBLZF)
else()
    target_include_directories(im PRIVATE ${LIBLZF_INCLUDE_DIR})
    target_link_libraries(im PRIVATE ${LIBLZF_LIBRARY})
endif()

# Library naming: libim.{so,dylib,dll}
set_target_properties(im PROPERTIES OUTPUT_NAME im)

# ----------------------------------------------------------------------
# libim_process (and libim_process_omp variant from the same sources)
# ----------------------------------------------------------------------

set(IM_PROCESS_SOURCES
    src/process/im_arithmetic_bin.cpp  src/process/im_morphology_gray.cpp
    src/process/im_quantize.cpp        src/process/im_arithmetic_un.cpp
    src/process/im_geometric.cpp       src/process/im_render.cpp
    src/process/im_color.cpp           src/process/im_histogram.cpp
    src/process/im_resize.cpp          src/process/im_convolve.cpp
    src/process/im_houghline.cpp       src/process/im_statistics.cpp
    src/process/im_convolve_rank.cpp   src/process/im_logic.cpp
    src/process/im_threshold.cpp       src/process/im_effects.cpp
    src/process/im_morphology_bin.cpp  src/process/im_tonegamut.cpp
    src/process/im_canny.cpp           src/process/im_distance.cpp
    src/process/im_analyze.cpp         src/process/im_kernel.cpp
    src/process/im_remotesens.cpp      src/process/im_point.cpp
    src/process/im_process_counter.cpp src/process/im_binary.cpp
    src/im_convertbitmap.cpp           src/im_convertcolor.cpp
    src/im_converttype.cpp)

if(IM_BUILD_PROCESS)
    add_library(im_process SHARED ${IM_PROCESS_SOURCES})
    im_add_def_file(im_process "${CMAKE_SOURCE_DIR}/src/im_process.def")
    target_include_directories(im_process
        PRIVATE ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/src/process)
    target_compile_definitions(im_process PRIVATE IM_PROCESS)
    target_link_libraries(im_process PRIVATE im)
endif()

if(IM_BUILD_PROCESS_OMP)
    # Homebrew's libomp isn't detected by CMake's stock FindOpenMP on
    # Apple Silicon clang. Probe Homebrew's libomp directly and seed
    # the variables FindOpenMP expects before calling it.
    if(APPLE AND NOT DEFINED OpenMP_CXX_FLAGS)
        find_path(LIBOMP_INCLUDE_DIR omp.h
            HINTS /opt/homebrew/opt/libomp/include /usr/local/opt/libomp/include)
        find_library(LIBOMP_LIBRARY omp
            HINTS /opt/homebrew/opt/libomp/lib /usr/local/opt/libomp/lib)
        if(LIBOMP_INCLUDE_DIR AND LIBOMP_LIBRARY)
            set(OpenMP_C_FLAGS         "-Xpreprocessor -fopenmp -I${LIBOMP_INCLUDE_DIR}")
            set(OpenMP_CXX_FLAGS       "-Xpreprocessor -fopenmp -I${LIBOMP_INCLUDE_DIR}")
            set(OpenMP_C_LIB_NAMES     "omp")
            set(OpenMP_CXX_LIB_NAMES   "omp")
            set(OpenMP_omp_LIBRARY     ${LIBOMP_LIBRARY})
        endif()
    endif()

    find_package(OpenMP REQUIRED COMPONENTS CXX)
    add_library(im_process_omp SHARED ${IM_PROCESS_SOURCES})
    # Same export list as im_process; tecmake shared the .def file too.
    im_add_def_file(im_process_omp "${CMAKE_SOURCE_DIR}/src/im_process.def")
    target_include_directories(im_process_omp
        PRIVATE ${CMAKE_SOURCE_DIR}/include
                ${CMAKE_SOURCE_DIR}/src/process)
    target_compile_definitions(im_process_omp PRIVATE IM_PROCESS)
    target_link_libraries(im_process_omp
        PRIVATE im OpenMP::OpenMP_CXX)
endif()

# ----------------------------------------------------------------------
# libim_jp2 (JPEG 2000 via libjasper)
# ----------------------------------------------------------------------

if(IM_BUILD_JP2)
    # Jasper has no upstream FindJasper / pkgconfig module; probe both.
    pkg_check_modules(JASPER QUIET IMPORTED_TARGET jasper)
    if(NOT JASPER_FOUND)
        find_path(JASPER_INCLUDE_DIR jasper/jasper.h
            HINTS /opt/homebrew/include /usr/include /usr/local/include)
        find_library(JASPER_LIBRARY NAMES jasper
            HINTS /opt/homebrew/lib /usr/lib /usr/local/lib)
        if(NOT JASPER_INCLUDE_DIR OR NOT JASPER_LIBRARY)
            message(FATAL_ERROR
                "libjasper not found. Install it (brew install jasper / "
                "vcpkg install jasper) or pass -DIM_BUILD_JP2=OFF.")
        endif()
    endif()

    add_library(im_jp2 SHARED src/im_format_jp2.cpp)
    im_add_def_file(im_jp2 "${CMAKE_SOURCE_DIR}/src/im_jp2.def")
    target_include_directories(im_jp2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(im_jp2 PRIVATE JASPER_SYSTEM_HEADER JASPER_2)
    if(JASPER_FOUND)
        target_link_libraries(im_jp2 PRIVATE im PkgConfig::JASPER)
    else()
        target_include_directories(im_jp2 PRIVATE ${JASPER_INCLUDE_DIR})
        target_link_libraries(im_jp2 PRIVATE im ${JASPER_LIBRARY})
    endif()
endif()

# ----------------------------------------------------------------------
# libim_fftw3 (FFTW3 backend)
# ----------------------------------------------------------------------

if(IM_BUILD_FFTW3)
    pkg_check_modules(FFTW3  REQUIRED IMPORTED_TARGET fftw3)
    pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
    add_library(im_fftw3 SHARED src/process/im_fft.cpp)
    im_add_def_file(im_fftw3 "${CMAKE_SOURCE_DIR}/src/im_fftw3.def")
    target_include_directories(im_fftw3 PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(im_fftw3 PRIVATE USE_FFTW3)
    target_link_libraries(im_fftw3 PRIVATE im im_process
        PkgConfig::FFTW3 PkgConfig::FFTW3F)
endif()

# ----------------------------------------------------------------------
# Lua bindings (imlua, imlua_process, imlua_jp2, imlua_fftw3)
# ----------------------------------------------------------------------

if(IM_BUILD_LUA)
    find_package(Lua REQUIRED)
    message(STATUS "Lua: ${LUA_VERSION_STRING} (${LUA_LIBRARIES})")

    # All four Lua add-ons share the same convention: <prefix>/include
    # for headers, lua5/imlua_*.c for sources, and the precomputed .lh
    # bytecode-blob headers in lua5/lh/ for embedded helper scripts.
    set(IMLUA_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/lua5
        ${CMAKE_SOURCE_DIR}/src/lua5/lh
        ${LUA_INCLUDE_DIR})

    # IMLUA_USELH selects the precompiled .lh blob headers (in
    # src/lua5/lh/) over a runtime luaL_dofile call against .lua files
    # in $PWD; the tecmake build did the same via USE_BIN2C_LUA=Yes.
    set(IMLUA_DEFS IMLUA_USELH)

    # libimlua (core)
    add_library(imlua SHARED
        src/lua5/imlua.c
        src/lua5/imlua_aux.c
        src/lua5/imlua_convert.c
        src/lua5/imlua_file.c
        src/lua5/imlua_image.c
        src/lua5/imlua_palette.c
        src/lua5/imlua_util.c)
    im_add_def_file(imlua "${CMAKE_SOURCE_DIR}/src/lua5/imlua.def")
    target_include_directories(imlua PRIVATE ${IMLUA_INCLUDE_DIRS})
    target_compile_definitions(imlua PRIVATE ${IMLUA_DEFS})
    target_link_libraries(imlua PRIVATE im ${LUA_LIBRARIES})
    set_target_properties(imlua PROPERTIES PREFIX "")  # Lua convention: imlua.so, no 'lib'

    if(IM_BUILD_PROCESS)
        add_library(imlua_process SHARED
            src/lua5/imlua_process.c
            src/lua5/imlua_kernel.c
            src/lua5/imlua_convert.c)
        im_add_def_file(imlua_process "${CMAKE_SOURCE_DIR}/src/lua5/imlua_process.def")
        target_include_directories(imlua_process PRIVATE ${IMLUA_INCLUDE_DIRS})
        # IM_PROCESS toggles imlua_convert.c into providing
        # imlua_open_processconvert (the variant used by imlua_process)
        # instead of the plain imlua_open_convert used by core imlua.
        target_compile_definitions(imlua_process PRIVATE IM_PROCESS ${IMLUA_DEFS})
        target_link_libraries(imlua_process PRIVATE im im_process imlua ${LUA_LIBRARIES})
        set_target_properties(imlua_process PROPERTIES PREFIX "")
    endif()

    if(IM_BUILD_JP2)
        add_library(imlua_jp2 SHARED src/lua5/imlua_jp2.c)
        im_add_def_file(imlua_jp2 "${CMAKE_SOURCE_DIR}/src/lua5/imlua_jp2.def")
        target_include_directories(imlua_jp2 PRIVATE ${IMLUA_INCLUDE_DIRS})
        target_compile_definitions(imlua_jp2 PRIVATE ${IMLUA_DEFS})
        target_link_libraries(imlua_jp2 PRIVATE im im_jp2 imlua ${LUA_LIBRARIES})
        set_target_properties(imlua_jp2 PROPERTIES PREFIX "")
    endif()

    if(IM_BUILD_FFTW3)
        add_library(imlua_fftw3 SHARED src/lua5/imlua_fftw.c)
        im_add_def_file(imlua_fftw3 "${CMAKE_SOURCE_DIR}/src/lua5/imlua_fftw.def")
        target_include_directories(imlua_fftw3 PRIVATE ${IMLUA_INCLUDE_DIRS})
        target_compile_definitions(imlua_fftw3 PRIVATE ${IMLUA_DEFS})
        target_link_libraries(imlua_fftw3 PRIVATE im im_process im_fftw3 imlua ${LUA_LIBRARIES})
        set_target_properties(imlua_fftw3 PROPERTIES PREFIX "")
    endif()
endif()

# ----------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------

include(GNUInstallDirs)
set(_im_targets im)
foreach(t IN ITEMS im_process im_process_omp im_jp2 im_fftw3
                   imlua imlua_process imlua_jp2 imlua_fftw3)
    if(TARGET ${t})
        list(APPEND _im_targets ${t})
    endif()
endforeach()

install(TARGETS ${_im_targets}
    EXPORT      IMTargets
    LIBRARY     DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE     DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME     DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")

# ----------------------------------------------------------------------
# Test executables
# ----------------------------------------------------------------------

# Document enhancement tests
if(IM_BUILD_PROCESS)
    add_executable(document_enhance test/document_enhance.c)
    target_include_directories(document_enhance PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(document_enhance PRIVATE im im_process)

    add_executable(document_enhance_v2 test/document_enhance_v2.c)
    target_include_directories(document_enhance_v2 PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(document_enhance_v2 PRIVATE im im_process)
endif()

install(EXPORT IMTargets
    NAMESPACE   IM::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/IM)

# ----------------------------------------------------------------------
# Test suite
# ----------------------------------------------------------------------

# Defines BUILD_TESTING (ON by default) and calls enable_testing(). Consumers
# who only want the libraries can configure with -DBUILD_TESTING=OFF.
include(CTest)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# Print a small summary so users can confirm what's being built.
message(STATUS "")
message(STATUS "Configured IM build:")
message(STATUS "  libim          ON")
message(STATUS "  libim_process       ${IM_BUILD_PROCESS}")
message(STATUS "  libim_process_omp   ${IM_BUILD_PROCESS_OMP}")
message(STATUS "  libim_jp2           ${IM_BUILD_JP2}")
message(STATUS "  libim_fftw3         ${IM_BUILD_FFTW3}")
message(STATUS "  Lua bindings        ${IM_BUILD_LUA}")
message(STATUS "")
