PROJNAME = im
LIBNAME = im
OPT = YES

LDIR = ../lib/$(TEC_UNAME)
USE_ZLIB = Yes
DEPENDDIR = dep

# Default to system image libs on macOS and Linux. The bundled
# libtiff/libjpeg sources in the IM tree don't build against modern
# toolchains so the bundled fallback is essentially a no-op there.
ifneq ($(findstring MacOS, $(TEC_UNAME)), )
  USE_SYSTEM_IMAGE_LIBS ?= Yes
endif
ifneq ($(findstring Linux, $(TEC_UNAME)), )
  USE_SYSTEM_IMAGE_LIBS ?= Yes
endif

# Windows: opt-in. Set USE_SYSTEM_IMAGE_LIBS=Yes plus one of
#   - VCPKG_ROOT (Microsoft vcpkg, recommended for MSVC)
#   - MSYS2_PREFIX (e.g. C:/msys64/mingw64, for mingw builds)
#   - WINDEPS_ROOT (a flat layout with include/, lib/, bin/)
# When none of the three is supplied the bundled fallback is used.
ifneq ($(findstring Win, $(TEC_SYSNAME)), )
  ifdef VCPKG_ROOT
    USE_SYSTEM_IMAGE_LIBS ?= Yes
  endif
  ifdef MSYS2_PREFIX
    USE_SYSTEM_IMAGE_LIBS ?= Yes
  endif
  ifdef WINDEPS_ROOT
    USE_SYSTEM_IMAGE_LIBS ?= Yes
  endif
endif

ifdef USE_SYSTEM_IMAGE_LIBS
  # Put the system base include path BEFORE "." so transitive <libexif/...>
  # includes resolved by the system libexif headers don't get shadowed by
  # the bundled libexif/ dir still present in the source tree.
  ifneq ($(findstring Win, $(TEC_SYSNAME)), )
    # Windows: pick the first env var that's set. Tools are expected to
    # produce a layout with include/ and lib/ underneath.
    ifdef VCPKG_ROOT
      # vcpkg's per-triplet layout. VCPKG_TRIPLET defaults to
      # x64-windows; user can override (e.g. x64-windows-static).
      VCPKG_TRIPLET ?= x64-windows
      INCLUDES = $(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/include . ../include
      LDIR    += $(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/lib
    else ifdef MSYS2_PREFIX
      INCLUDES = $(MSYS2_PREFIX)/include . ../include
      LDIR    += $(MSYS2_PREFIX)/lib
    else ifdef WINDEPS_ROOT
      INCLUDES = $(WINDEPS_ROOT)/include . ../include
      LDIR    += $(WINDEPS_ROOT)/lib
    endif
  else
    ifneq ($(wildcard /opt/homebrew/include),)
      INCLUDES = /opt/homebrew/include . ../include
    else ifneq ($(wildcard /usr/local/include/libexif),)
      INCLUDES = /usr/local/include . ../include
    else
      # Linux distros typically install libexif under /usr/include/libexif/.
      # Note: '.' (= src/) is intentionally omitted here so that the
      # bundled src/libexif/ subdirectory does not shadow the system
      # libexif headers via transitive '<libexif/...>' includes; that
      # produced struct redefinitions against modern libexif.
      INCLUDES = /usr/include ../include
    endif
  endif
else
  INCLUDES = . ../include
endif

# WORDS_BIGENDIAN used by libTIFF
ifeq ($(TEC_SYSARCH), ppc)
  DEFINES = WORDS_BIGENDIAN
endif
ifeq ($(TEC_SYSARCH), mips)
  DEFINES = WORDS_BIGENDIAN
endif
ifeq ($(TEC_SYSARCH), sparc)
  DEFINES = WORDS_BIGENDIAN
endif

SRCTIFF = \
    tif_aux.c       tif_dirwrite.c   tif_jpeg.c      tif_print.c    \
    tif_close.c     tif_dumpmode.c   tif_luv.c       tif_read.c     \
    tif_codec.c     tif_error.c      tif_lzw.c       tif_strip.c    \
    tif_color.c     tif_extension.c  tif_next.c      tif_swab.c     \
    tif_compress.c  tif_fax3.c       tif_open.c      tif_thunder.c  \
    tif_dir.c       tif_fax3sm.c     tif_packbits.c  tif_tile.c     \
    tif_dirinfo.c   tif_flush.c      tif_pixarlog.c  tif_zip.c      \
    tif_dirread.c   tif_getimage.c   tif_predict.c   tif_version.c  \
    tif_write.c     tif_warning.c    tif_ojpeg.c     tif_lzma.c     \
    tif_jbig.c
SRCTIFF := $(addprefix libtiff/, $(SRCTIFF))

SRCPNG = \
    png.c       pngget.c    pngread.c   pngrutil.c  pngwtran.c  \
    pngerror.c  pngmem.c    pngrio.c    pngset.c    pngwio.c    \
    pngpread.c  pngrtran.c  pngtrans.c  pngwrite.c  pngwutil.c
SRCPNG := $(addprefix libpng/, $(SRCPNG))

SRCJPEG = \
    jcapimin.c  jcmarker.c  jdapimin.c  jdinput.c   jdtrans.c   \
    jcapistd.c  jcmaster.c  jdapistd.c  jdmainct.c  jerror.c    jmemmgr.c  \
    jccoefct.c  jcomapi.c   jdatadst.c  jdmarker.c  jfdctflt.c  jmemnobs.c \
    jccolor.c   jcparam.c   jdatasrc.c  jdmaster.c  jfdctfst.c  jquant1.c  \
    jcdctmgr.c  jdcoefct.c  jdmerge.c   jfdctint.c  jquant2.c  \
    jchuff.c    jcprepct.c  jdcolor.c   jidctflt.c  jutils.c    jdarith.c \
    jcinit.c    jcsample.c  jddctmgr.c  jdpostct.c  jidctfst.c  jaricom.c  \
    jcmainct.c  jctrans.c   jdhuff.c    jdsample.c  jidctint.c  jcarith.c
SRCJPEG := $(addprefix libjpeg/, $(SRCJPEG))

SRCEXIF = \
    fuji/exif-mnote-data-fuji.c  fuji/mnote-fuji-entry.c  fuji/mnote-fuji-tag.c                    \
    canon/exif-mnote-data-canon.c  canon/mnote-canon-entry.c  canon/mnote-canon-tag.c              \
    olympus/exif-mnote-data-olympus.c  olympus/mnote-olympus-entry.c  olympus/mnote-olympus-tag.c  \
    pentax/exif-mnote-data-pentax.c  pentax/mnote-pentax-entry.c  pentax/mnote-pentax-tag.c        \
    exif-byte-order.c  exif-entry.c  exif-utils.c    exif-format.c  exif-mnote-data.c              \
    exif-content.c  exif-ifd.c  exif-tag.c exif-data.c  exif-loader.c exif-log.c exif-mem.c
SRCEXIF  := $(addprefix libexif/, $(SRCEXIF))

SRCLZF = \
    lzf_c.c lzf_d.c
SRCLZF  := $(addprefix liblzf/, $(SRCLZF))
INCLUDES += liblzf

SRCLZ4 = \
    lz4.c
SRCLZ4  := $(addprefix lz4/, $(SRCLZ4))

SRC = \
    im_oldcolor.c         im_oldresize.c      im_converttype.cpp   \
    im_attrib.cpp         im_format.cpp       im_format_tga.cpp    im_filebuffer.cpp    \
    im_bin.cpp            im_format_all.cpp   im_format_raw.cpp    im_convertopengl.cpp \
    im_binfile.cpp        im_format_sgi.cpp   im_datatype.cpp      im_format_pcx.cpp    \
    im_colorhsi.cpp       im_format_bmp.cpp   im_image.cpp         im_rgb2map.cpp       \
    im_colormode.cpp      im_format_gif.cpp   im_lib.cpp           im_format_pnm.cpp    \
    im_colorutil.cpp      im_format_ico.cpp   im_palette.cpp       im_format_ras.cpp    \
    im_convertbitmap.cpp  im_format_led.cpp   im_counter.cpp       im_str.cpp           \
    im_convertcolor.cpp   im_fileraw.cpp      im_format_krn.cpp    im_compress.cpp      \
    im_file.cpp           im_old.cpp          im_format_pfm.cpp                         \
    im_format_tiff.cpp    im_format_png.cpp   im_format_jpeg.cpp

ifdef USE_SYSTEM_IMAGE_LIBS
  # Link against system libs instead of compiling bundled sources.
  # Note: drops imBinFile-backed I/O for TIFF (tiff_binfile.c uses libtiff
  # private headers that aren't shipped with system installs).
  LIBS += tiff jpeg lz4
  # liblzf has no Windows system-package presence (not in vcpkg or MSYS2),
  # so on Windows we always compile it from the bundled tree even when
  # USE_SYSTEM_IMAGE_LIBS is on.
  ifneq ($(findstring Win, $(TEC_SYSNAME)), )
    SRC += $(SRCLZF)
    INCLUDES += liblzf
  else
    LIBS += lzf
  endif
else
  SRC += $(SRCLZF)
  INCLUDES += liblzf
  SRC += $(SRCLZ4)
  INCLUDES += lz4
  SRC += $(SRCTIFF) tiff_binfile.c
  INCLUDES += libtiff
  SRC += $(SRCJPEG)
  INCLUDES += libjpeg
endif

# libpng: system on UNIX/macOS, system-or-bundled on Windows.
ifneq ($(findstring Win, $(TEC_SYSNAME)), )
  ifdef USE_SYSTEM_IMAGE_LIBS
    LIBS += libpng16
  else
    SRC += $(SRCPNG)
    INCLUDES += libpng
  endif
else
  # The IM build was historically missing -lpng (relying on
  # -undefined dynamic_lookup to leave libpng symbols dangling), so
  # libim.dylib was unloadable in isolation. Link it explicitly.
  INCLUDES += /usr/include/libpng
  LIBS += png
endif
    
ifneq ($(findstring Win, $(TEC_SYSNAME)), )
  SRC += im_sysfile_win32.cpp im_dib.cpp im_dibxbitmap.cpp
  
  ifneq ($(findstring dll, $(TEC_UNAME)), )
    SRC += im.rc
  endif
  
  # force the definition of math functions using float
  # Watcom does not define them
  ifneq ($(findstring owc, $(TEC_UNAME)), )
    DEFINES += IM_DEFMATHFLOAT
  endif         
  
  ifneq ($(findstring bc, $(TEC_UNAME)), )
    DEFINES += IM_DEFMATHFLOAT
  else
    USE_EXIF = Yes
  endif
else
  USE_EXIF = Yes
  SRC += im_sysfile_unix.cpp
endif

ifdef USE_EXIF
  ifdef USE_SYSTEM_IMAGE_LIBS
    # IM source uses unqualified "exif-data.h"; system installs put it
    # under <prefix>/include/libexif/, which we add here. The base
    # prefix is already first in INCLUDES so transitive <libexif/...>
    # resolves to the system copy, not the bundled tree.
    ifneq ($(findstring Win, $(TEC_SYSNAME)), )
      ifdef VCPKG_ROOT
        INCLUDES += $(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/include/libexif
      else ifdef MSYS2_PREFIX
        INCLUDES += $(MSYS2_PREFIX)/include/libexif
      else ifdef WINDEPS_ROOT
        INCLUDES += $(WINDEPS_ROOT)/include/libexif
      endif
    else ifneq ($(wildcard /opt/homebrew/include/libexif),)
      INCLUDES += /opt/homebrew/include/libexif
    else ifneq ($(wildcard /usr/local/include/libexif),)
      INCLUDES += /usr/local/include/libexif
    else ifneq ($(wildcard /usr/include/libexif),)
      INCLUDES += /usr/include/libexif
    endif
    LIBS += exif
  else
    INCLUDES += libexif
    SRC += $(SRCEXIF)
  endif
  DEFINES += USE_EXIF
endif

ifneq ($(findstring AIX, $(TEC_UNAME)), )
  DEFINES += IM_DEFMATHFLOAT
endif

ifneq ($(findstring SunOS, $(TEC_UNAME)), )
  DEFINES += IM_DEFMATHFLOAT
endif
      
ifneq ($(findstring HP-UX, $(TEC_UNAME)), )
  DEFINES += IM_DEFMATHFLOAT
endif

ifneq ($(findstring MacOS, $(TEC_UNAME)), )
  # Build as MH_DYLIB so other libs can link against it.
  # Old MacOS X 10.4 (TEC_SYSVERSION=10, TEC_SYSMINOR=4) didn't support dylibs.
  ifneq ($(TEC_SYSVERSION).$(TEC_SYSMINOR), 10.4)
    BUILD_DYLIB=Yes
  endif
endif
