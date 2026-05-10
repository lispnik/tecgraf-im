PROJNAME = im
LIBNAME = im_jp2
OPT = YES

DEPENDDIR = dep

# GeoJasper support only available for Jasper version 1
# GeoJasPer 1.4.0
# Copyright (c) 2003-2007 Dmitry V. Fedorov.
# http://www.dimin.net/software/geojasper/

ifdef USE_JASPER1
  JASPER = libjasper
else
  JASPER = libjasper2
  JASPERVER = JASPER_2
  # libjasper2 prefixes feature macros with JAS_
  JAS_PREFIX = JAS_
endif

SRCJP2 =  \
    base/jas_cm.c      base/jas_icc.c      base/jas_init.c    base/jas_stream.c  base/jas_version.c \
    base/jas_debug.c   base/jas_iccdata.c  base/jas_malloc.c  base/jas_string.c  base/jas_tmr.c \
    base/jas_getopt.c  base/jas_image.c    base/jas_seq.c     base/jas_tvp.c            \
    jp2/jp2_cod.c  jp2/jp2_dec.c  jp2/jp2_enc.c                                         \
    jpc/jpc_bs.c   jpc/jpc_math.c   jpc/jpc_mqenc.c  jpc/jpc_t1enc.c  jpc/jpc_tagtree.c \
    jpc/jpc_cs.c   jpc/jpc_mct.c    jpc/jpc_qmfb.c   jpc/jpc_t2cod.c  jpc/jpc_tsfb.c    \
    jpc/jpc_dec.c  jpc/jpc_mqcod.c  jpc/jpc_t1cod.c  jpc/jpc_t2dec.c  jpc/jpc_util.c    \
    jpc/jpc_enc.c  jpc/jpc_mqdec.c  jpc/jpc_t1dec.c  jpc/jpc_t2enc.c
SRCJP2  := $(addprefix $(JASPER)/, $(SRCJP2))

SRC = im_format_jp2.cpp

ifneq ($(findstring MacOS, $(TEC_UNAME)), )
  USE_SYSTEM_JASPER ?= Yes
endif

# Linux distros after ~22.04 dropped libjasper entirely (CVE history),
# so we don't auto-enable USE_SYSTEM_JASPER there. On Windows it's
# opt-in: vcpkg ships a 'jasper' port (vcpkg install jasper) that the
# user can enable via USE_SYSTEM_JASPER=Yes.
ifneq ($(findstring Win, $(TEC_SYSNAME)), )
  ifdef VCPKG_ROOT
    # Allow opt-in but don't force; user sets USE_SYSTEM_JASPER=Yes.
  endif
endif

ifdef USE_SYSTEM_JASPER
  # Link against system libjasper. jas_binfile.c is dropped because it
  # uses jas_stream_create/jas_stream_initbuf which are static in upstream.
  LIBS += jasper
  DEFINES = JASPER_SYSTEM_HEADER $(JASPERVER)
  # On Windows, system jasper is reached through the same dependency
  # roots used by libim. Mirror the include/lib search there.
  ifneq ($(findstring Win, $(TEC_SYSNAME)), )
    ifdef VCPKG_ROOT
      VCPKG_TRIPLET ?= x64-windows
      INCLUDES += $(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/include
      LDIR     += $(VCPKG_ROOT)/installed/$(VCPKG_TRIPLET)/lib
    else ifdef MSYS2_PREFIX
      INCLUDES += $(MSYS2_PREFIX)/include
      LDIR     += $(MSYS2_PREFIX)/lib
    else ifdef WINDEPS_ROOT
      INCLUDES += $(WINDEPS_ROOT)/include
      LDIR     += $(WINDEPS_ROOT)/lib
    endif
  endif
else
  SRC += jas_binfile.c $(SRCJP2)
  INCLUDES = $(JASPER)
  DEFINES = EXCLUDE_JPG_SUPPORT EXCLUDE_MIF_SUPPORT EXCLUDE_PNM_SUPPORT \
            EXCLUDE_BMP_SUPPORT EXCLUDE_PGX_SUPPORT EXCLUDE_RAS_SUPPORT \
            EXCLUDE_TIFF_SUPPORT JAS_GEO_OMIT_PRINTING_CODE JAS_BINFILE $(JASPERVER)
endif

ifneq ($(findstring Win, $(TEC_SYSNAME)), )
  ifneq ($(findstring owc1, $(TEC_UNAME)), )
    DEFINES += JAS_TYPES
  endif         
  ifneq ($(findstring dll, $(TEC_UNAME)), )
    DEFINES += JAS_WIN_MSVC_BUILD JAS_TYPES
  endif         
  ifneq ($(findstring vc, $(TEC_UNAME)), )
    DEFINES += JAS_WIN_MSVC_BUILD JAS_TYPES
  endif         
  ifneq ($(findstring bc, $(TEC_UNAME)), )
    DEFINES += JAS_TYPES
  endif         
  ifneq ($(findstring gcc, $(TEC_UNAME)), )
    DEFINES += $(JAS_PREFIX)HAVE_UNISTD_H JAS_TYPES
  endif         
  ifneq ($(findstring mingw, $(TEC_UNAME)), )
    DEFINES += HAVE_UNISTD_H $(JAS_PREFIX)HAVE_STDINT_H JAS_TYPES
  endif         
else
  DEFINES += $(JAS_PREFIX)HAVE_UNISTD_H JAS_TYPES
endif

ifneq ($(findstring MacOS, $(TEC_UNAME)), )
  ifneq ($(TEC_SYSVERSION).$(TEC_SYSMINOR), 10.4)
    BUILD_DYLIB=Yes
  endif
endif

USE_IM=Yes
IM = ..
