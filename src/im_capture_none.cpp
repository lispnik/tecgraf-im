/** \file
 * \brief Video Capture - no backend for this platform
 *
 * See Copyright Notice in im_lib.h
 */

/* IM's capture library has no base class and no virtual dispatch: the contract
   is the 27 functions declared in include/im_capture.h and listed in
   src/im_capture.def, and each platform supplies exactly one translation unit
   that defines all of them plus its own struct _imVideoCapture. Backend
   selection happens in CMakeLists.txt, not at runtime.

   This is the backend for platforms that have none. It reports that no capture
   device exists, which is a state every caller already has to handle --
   imVideoCaptureCreate is documented to return NULL when there is no device,
   and imVideoCaptureDeviceCount to return a count that may be zero.

   It exists so the symbol set is identical on every platform. A consumer can
   then link libim_capture unconditionally and discover at runtime that there
   are no cameras, rather than failing to link on some platforms and not
   others. On MSVC it also keeps src/im_capture.def honest: the linker fails
   with LNK2001 if this file ever falls behind the export list, which is what
   keeps the two backends' symbol sets in step.

   Platforms currently landing here:

     Linux    -- no backend has ever been written. The im_capture_v4l.cpp line
                 in the old tecmake makefile was commented out and the file was
                 never committed.
     Windows  -- src/im_capture_dx.cpp exists but needs qedit.h, which Microsoft
                 removed from the Windows SDK after 6.1 (2008), plus DirectX
                 SDK 9.15. Build it with -DIM_CAPTURE_DIRECTSHOW=ON if you have
                 them.

   macOS uses src/im_capture_avf.mm, which is a real implementation. */

#include <im.h>
#include <im_util.h>

#include "im_capture.h"

#include <stdlib.h>


/* Never allocated: imVideoCaptureCreate always returns NULL here. Defined as a
   complete type anyway so the header's opaque pointer is well formed. */
struct _imVideoCapture
{
  int unused;
};


/*************************************************************************
                            Device List
*************************************************************************/

int imVideoCaptureDeviceCount(void)
{
  return 0;
}

const char* imVideoCaptureDeviceDesc(int device)
{
  (void)device;
  return NULL;
}

const char* imVideoCaptureDeviceExDesc(int device)
{
  (void)device;
  return NULL;
}

const char* imVideoCaptureDevicePath(int device)
{
  (void)device;
  return NULL;
}

const char* imVideoCaptureDeviceVendorInfo(int device)
{
  (void)device;
  return NULL;
}

int imVideoCaptureReloadDevices(void)
{
  return 0;
}

void imVideoCaptureReleaseDevices(void)
{
}


/*************************************************************************
                              Lifecycle
*************************************************************************/

imVideoCapture* imVideoCaptureCreate(void)
{
  /* "Returns NULL if there is no capture device available." */
  return NULL;
}

void imVideoCaptureDestroy(imVideoCapture* vc)
{
  (void)vc;
}

int imVideoCaptureConnect(imVideoCapture* vc, int device)
{
  (void)vc; (void)device;
  return 0;
}

void imVideoCaptureDisconnect(imVideoCapture* vc)
{
  (void)vc;
}


/*************************************************************************
                          Dialogs and Routing
*************************************************************************/

int imVideoCaptureDialogCount(imVideoCapture* vc)
{
  (void)vc;
  return 0;
}

int imVideoCaptureShowDialog(imVideoCapture* vc, int dialog, void* parent)
{
  (void)vc; (void)dialog; (void)parent;
  return 0;
}

const char* imVideoCaptureDialogDesc(imVideoCapture* vc, int dialog)
{
  (void)vc; (void)dialog;
  return NULL;
}

int imVideoCaptureSetInOut(imVideoCapture* vc, int input, int output, int cross)
{
  (void)vc; (void)input; (void)output; (void)cross;
  return 0;
}


/*************************************************************************
                           Formats and Size
*************************************************************************/

int imVideoCaptureFormatCount(imVideoCapture* vc)
{
  (void)vc;
  return 0;
}

int imVideoCaptureGetFormat(imVideoCapture* vc, int format, int *width, int *height, char* desc)
{
  (void)vc; (void)format; (void)desc;
  if (width)  *width = 0;
  if (height) *height = 0;
  return 0;
}

int imVideoCaptureSetFormat(imVideoCapture* vc, int format)
{
  (void)vc; (void)format;
  return 0;
}

void imVideoCaptureGetImageSize(imVideoCapture* vc, int *width, int *height)
{
  /* "width and height returns 0 if not connected." Nothing here is ever
     connected, so this is always the answer. */
  (void)vc;
  if (width)  *width = 0;
  if (height) *height = 0;
}

int imVideoCaptureSetImageSize(imVideoCapture* vc, int width, int height)
{
  (void)vc; (void)width; (void)height;
  return 0;
}


/*************************************************************************
                                Capture
*************************************************************************/

int imVideoCaptureFrame(imVideoCapture* vc, unsigned char* data, int color_mode, int timeout)
{
  /* "Returns zero if failed or timeout expired, the buffer is not changed."
     Returning without touching data is the whole of the contract here. */
  (void)vc; (void)data; (void)color_mode; (void)timeout;
  return 0;
}

int imVideoCaptureOneFrame(imVideoCapture* vc, unsigned char* data, int color_mode)
{
  (void)vc; (void)data; (void)color_mode;
  return 0;
}

int imVideoCaptureLive(imVideoCapture* vc, int live)
{
  (void)vc; (void)live;
  return 0;
}


/*************************************************************************
                              Attributes
*************************************************************************/

int imVideoCaptureResetAttribute(imVideoCapture* vc, const char* attrib, int fauto)
{
  (void)vc; (void)attrib; (void)fauto;
  return 0;
}

int imVideoCaptureGetAttribute(imVideoCapture* vc, const char* attrib, double *percent)
{
  (void)vc; (void)attrib;
  if (percent) *percent = 0;
  return 0;
}

int imVideoCaptureSetAttribute(imVideoCapture* vc, const char* attrib, double percent)
{
  (void)vc; (void)attrib; (void)percent;
  return 0;
}

const char** imVideoCaptureGetAttributeList(imVideoCapture* vc, int *num_attrib)
{
  (void)vc;
  if (num_attrib) *num_attrib = 0;
  return NULL;
}
