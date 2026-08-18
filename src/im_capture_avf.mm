/** \file
 * \brief Video Capture - macOS, over AVFoundation
 *
 * See Copyright Notice in im_lib.h
 */

/* IM's capture library has no base class and no virtual dispatch. The contract
   is the 27 functions declared in include/im_capture.h and listed in
   src/im_capture.def, and each platform supplies exactly one translation unit
   that defines all of them plus its own struct _imVideoCapture. Backend
   selection happens in CMakeLists.txt. So this file is a peer of
   src/im_capture_dx.cpp, not a subclass of anything, and the sections below
   are in the same order as that file so the two can be read side by side.

   The pipeline is
   
     AVCaptureDevice -> AVCaptureDeviceInput -> AVCaptureSession
                     -> AVCaptureVideoDataOutput -> delegate on a serial queue

   which is the AVFoundation counterpart of the DirectShow graph
   "capture_filter -> grabber_filter -> null_filter" the reference builds.

   Three notes that apply to the whole file:

   ARC is on for this file (set in CMakeLists.txt), so the Objective-C members
   of struct _imVideoCapture are __strong and the handle is allocated with new
   and released with delete -- not the malloc/memset/free the reference uses,
   which would be undefined for a struct with non-trivial members.
   CVPixelBufferRef is a Core Foundation type and is NOT managed by ARC; the
   frame slot retains and releases it by hand.

   Every public entry point wraps its body in @autoreleasepool. A C caller has
   no pool on the stack -- this library is reached from plain C, from Lua, and
   from runtimes that dlopen it -- and without one the first Objective-C
   message logs "autoreleased with no pool in place" and leaks.

   Deliberate deviations from the DirectShow backend are marked "Deviation:"
   and name the line they diverge from, so nobody quietly "fixes" them back.
*/

#include <AVFoundation/AVFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <im.h>
#include <im_util.h>
#include <im_color.h>

#include "im_capture.h"


/*************************************************************************
                              Device List
*************************************************************************/

/* Same cap as the reference (im_capture_dx.cpp:236). A machine with more than
   thirty cameras is not a case this API was shaped for. */
#define VC_MAXVIDDEVICES 30

/* The strings are plain storage on purpose. imVideoCaptureDeviceDesc and its
   siblings hand out pointers into this array and the header documents them as
   owned by the library, so they have to be memory whose address does not move,
   not NSString bytes whose lifetime belongs to an autorelease pool. */
struct vcDevice
{
  char desc[160];
  char ex_desc[256];
  char path[256];
  char vendorinfo[128];
};

static vcDevice vc_DeviceList[VC_MAXVIDDEVICES];
static int vc_DeviceCount = 0;

/* Index-matched to vc_DeviceList. Held separately from the POD above so there
   is no array of C++ objects with ARC members at namespace scope, and so the
   only static destruction question is this one pointer -- which is why
   imVideoCaptureReleaseDevices exists. */
static NSArray<AVCaptureDevice*>* vc_DeviceObjects = nil;

/* One lock around the list. The reference has none; Create() is plausibly
   called from a worker thread, and enumerating from two at once would race on
   both vc_DeviceCount and the string buffers. */
static pthread_mutex_t vc_ListMutex = PTHREAD_MUTEX_INITIALIZER;


static void vc_EnumerateDevicesLocked(void)
{
  @autoreleasepool
  {
    NSMutableArray* types =
      [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];

    if (@available(macOS 14.0, *))
    {
      /* USB and other external cameras. Before macOS 14 this was
         AVCaptureDeviceTypeExternalUnknown, which still exists but is
         deprecated -- the tree already builds with
         -Wno-deprecated-declarations (CMakeLists.txt:12). */
      [types addObject:AVCaptureDeviceTypeExternal];

      /* An iPhone used as a webcam. The SDK header says an app opts in to
         this type through an Info.plist key, which suggested a bundle-less
         command line tool would never see one -- but it does: a plain C
         binary run from a terminal enumerates "Matthew's iPhone (2) Camera"
         alongside the built-in and USB cameras. The key evidently gates
         something narrower than discovery. */
      [types addObject:AVCaptureDeviceTypeContinuityCamera];
    }
    else
    {
      [types addObject:AVCaptureDeviceTypeExternalUnknown];
    }

    AVCaptureDeviceDiscoverySession* session =
      [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:types
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];

    NSArray<AVCaptureDevice*>* devices = session.devices;
    if (devices.count > VC_MAXVIDDEVICES)
      devices = [devices subarrayWithRange:NSMakeRange(0, VC_MAXVIDDEVICES)];

    memset(vc_DeviceList, 0, sizeof(vc_DeviceList));
    vc_DeviceObjects = devices;
    vc_DeviceCount = (int)devices.count;

    for (int i = 0; i < vc_DeviceCount; i++)
    {
      AVCaptureDevice* device = devices[i];
      vcDevice* entry = &vc_DeviceList[i];

      /* The index prefix matches the reference (im_capture_dx.cpp:253):
         callers such as test/glut_capture.c print this straight into a
         numbered menu and would otherwise have to build the number
         themselves.

         snprintf throughout, where the reference uses strcpy into fixed
         buffers -- localizedName is user-visible text of no bounded length. */
      snprintf(entry->desc, sizeof(entry->desc), "%d - %s", i,
               device.localizedName? device.localizedName.UTF8String: "camera");

      if (device.modelID)
        snprintf(entry->ex_desc, sizeof(entry->ex_desc), "%s",
                 device.modelID.UTF8String);
      if (device.uniqueID)
        snprintf(entry->path, sizeof(entry->path), "%s",
                 device.uniqueID.UTF8String);
      if (device.manufacturer)
        snprintf(entry->vendorinfo, sizeof(entry->vendorinfo), "%s",
                 device.manufacturer.UTF8String);
    }
  }
}

/* Returns non-zero when "device" names a device that exists, enumerating once
   if nothing has yet.

   Deviation from im_capture_dx.cpp:597: there, imVideoCaptureDeviceCount
   returns the raw counter, which is zero until something else has triggered
   enumeration -- so a caller who asks for the count first, as
   test/glut_capture.c:320 does, is told there are no cameras. That demo only
   works because imVideoCaptureCreate happens to run earlier. Enumerating
   lazily here makes the documented return value true whenever it is asked
   for. */
static int vc_CheckDeviceList(int device)
{
  if (vc_DeviceCount == 0)
  {
    vc_EnumerateDevicesLocked();
    if (vc_DeviceCount == 0)
      return 0;
  }

  if (device < 0 || device >= vc_DeviceCount)
    return 0;

  return 1;
}


/*************************************************************************
                          The capture handle
*************************************************************************/

/* The sample-buffer delegate. Its only job is to move the newest frame into
   the slot; the conversion happens on the caller's thread in Frame(), so a
   slow consumer cannot stall the capture queue.

   The back pointer is unowned -- the handle owns the delegate, not the other
   way round -- and Disconnect clears it, but only after the dispatch barrier,
   so no callback can be holding it. */
@interface imCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) imVideoCapture* vc;
@end


struct _imVideoCapture
{
  int device;   /* index into vc_DeviceList, -1 when not connected */
  int live;     /* 1 while the session is running */

  int width, height;   /* of the connected format; 0,0 when not connected */

  /* Our own retain of the device, independent of vc_DeviceObjects, so
     imVideoCaptureReleaseDevices cannot pull it out from under a running
     session. The reference borrows the pointer out of its device list and
     comments "do not release here" (im_capture_dx.cpp:1042). */
  AVCaptureDevice*          av_device;

  AVCaptureSession*         session;
  AVCaptureDeviceInput*     input;
  AVCaptureVideoDataOutput* output;
  imCaptureDelegate*        delegate;
  dispatch_queue_t          queue;    /* serial; also the teardown barrier */

  /* The frame slot. One buffer, newest wins -- the same model as the
     reference's single m_ImageData with its m_newImageFlag. slot_buffer is
     Core Foundation and is retained and released by hand even under ARC. */
  pthread_mutex_t  slot_mutex;
  pthread_cond_t   slot_cond;
  CVPixelBufferRef slot_buffer;
  int              slot_full;
  int              stopping;   /* set by Disconnect to release a blocked Frame */
};


/* Filled in with the frame slot in phase 3; the class has to exist now so the
   handle can hold one and Disconnect can clear its back pointer. */
@implementation imCaptureDelegate
@end


/*************************************************************************
                        Device List entry points
*************************************************************************/

int imVideoCaptureDeviceCount(void)
{
  int count;
  pthread_mutex_lock(&vc_ListMutex);
  vc_CheckDeviceList(0);
  count = vc_DeviceCount;
  pthread_mutex_unlock(&vc_ListMutex);
  return count;
}

const char* imVideoCaptureDeviceDesc(int device)
{
  const char* result = NULL;
  pthread_mutex_lock(&vc_ListMutex);
  if (vc_CheckDeviceList(device))
    result = vc_DeviceList[device].desc;
  pthread_mutex_unlock(&vc_ListMutex);
  return result;
}

const char* imVideoCaptureDeviceExDesc(int device)
{
  const char* result = NULL;
  pthread_mutex_lock(&vc_ListMutex);
  if (vc_CheckDeviceList(device))
    result = vc_DeviceList[device].ex_desc;
  pthread_mutex_unlock(&vc_ListMutex);
  return result;
}

const char* imVideoCaptureDevicePath(int device)
{
  const char* result = NULL;
  pthread_mutex_lock(&vc_ListMutex);
  if (vc_CheckDeviceList(device))
    result = vc_DeviceList[device].path;
  pthread_mutex_unlock(&vc_ListMutex);
  return result;
}

const char* imVideoCaptureDeviceVendorInfo(int device)
{
  const char* result = NULL;
  pthread_mutex_lock(&vc_ListMutex);
  if (vc_CheckDeviceList(device))
    result = vc_DeviceList[device].vendorinfo;
  pthread_mutex_unlock(&vc_ListMutex);
  return result;
}

int imVideoCaptureReloadDevices(void)
{
  int count;
  pthread_mutex_lock(&vc_ListMutex);
  vc_DeviceObjects = nil;
  vc_DeviceCount = 0;
  memset(vc_DeviceList, 0, sizeof(vc_DeviceList));
  vc_EnumerateDevicesLocked();
  count = vc_DeviceCount;
  pthread_mutex_unlock(&vc_ListMutex);
  return count;
}

void imVideoCaptureReleaseDevices(void)
{
  pthread_mutex_lock(&vc_ListMutex);
  vc_DeviceObjects = nil;
  vc_DeviceCount = 0;
  /* Cleared rather than merely counted down to zero, so a caller still holding
     a pointer this call invalidates reads an empty string rather than a stale
     name. It is a use-after-free either way, but a quiet one is worse. */
  memset(vc_DeviceList, 0, sizeof(vc_DeviceList));
  pthread_mutex_unlock(&vc_ListMutex);
}


/*************************************************************************
                          Create and Destroy
*************************************************************************/

imVideoCapture* imVideoCaptureCreate(void)
{
  @autoreleasepool
  {
    /* "Returns NULL if there is no capture device available." */
    if (imVideoCaptureDeviceCount() == 0)
      return NULL;

    /* new, not malloc: the ARC members below are non-trivially destructible,
       so the reference's malloc/memset/free (im_capture_dx.cpp:805) would be
       undefined here. Value-initialised, so every scalar starts at 0 and every
       object pointer at nil. */
    imVideoCapture* vc = new _imVideoCapture();
    if (!vc)
      return NULL;

    vc->device = -1;

    if (pthread_mutex_init(&vc->slot_mutex, NULL) != 0)
    {
      delete vc;
      return NULL;
    }
    if (pthread_cond_init(&vc->slot_cond, NULL) != 0)
    {
      pthread_mutex_destroy(&vc->slot_mutex);
      delete vc;
      return NULL;
    }

    return vc;
  }
}

void imVideoCaptureDestroy(imVideoCapture* vc)
{
  assert(vc);
  if (!vc)
    return;

  @autoreleasepool
  {
    imVideoCaptureDisconnect(vc);

    /* Safe only because Disconnect has already run the dispatch barrier, so
       no delegate callback is executing or pending. Destroying a mutex a
       callback is about to lock returns EBUSY on Darwin and is then a
       use-after-free with nothing to report it. */
    pthread_cond_destroy(&vc->slot_cond);
    pthread_mutex_destroy(&vc->slot_mutex);

    delete vc;
  }
}


/*************************************************************************
   Not supported on this platform.

   Each of these is a DirectShow concept with no AVFoundation counterpart.
   The API's documented convention for "this device does not support it" is a
   zero or NULL return -- imVideoCaptureGetAttribute's documentation names it
   as the way to probe -- so these are honest rather than silently wrong.

     Dialogs      DirectShow exposes each filter's property pages through
                  ISpecifyPropertyPages and OleCreatePropertyFrame. AVFoundation
                  has no equivalent; there is no system-provided camera settings
                  window to show.
     SetInOut     Routes an analog capture card's crossbar. No counterpart.
     Attributes   The names in im_capture.h:252-296 are a Windows list, down to
                  AnalogFormat enumerating DirectShow's AnalogVideoStandard
                  ordinals. AVFoundation exposes exposure, focus and white
                  balance as modes on AVCaptureDevice rather than as a
                  percentage of a range, so a partial mapping would be less
                  useful than an honest "unsupported".
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


/*************************************************************************
                            Authorization
*************************************************************************/

static void vc_FlushSlotLocked(imVideoCapture* vc);

/* How long to wait for the user to answer the camera prompt, in seconds. */
#define VC_AUTH_TIMEOUT 30

/* AVFoundation does NOT fail when camera permission is missing. From
   AVCaptureDevice.h: "Until access has been granted, any AVCaptureDevices for
   the media type will vend silent audio samples or black video frames." So
   without this check a caller would connect, capture, and get a plausible
   all-black image with nothing anywhere reporting a problem. Silently
   returning black is the worst of the available behaviours.

   Connect is the only function the API lets fail for this reason, so this is
   the only place it can be reported. Enumeration deliberately does not call
   it: device names read back with no authorisation and no prompt, so gating
   the list would turn "no permission" into the wrong diagnosis, "no camera".

   What this function CANNOT do is protect a process that has no camera usage
   description. TCC does not return a status in that case, it kills the
   process -- SIGABRT, with "attempted to access privacy-sensitive data
   without a usage description", from inside the request below. It is not
   catchable, and no amount of checking here avoids it, because asking is
   itself the access. Measured, not assumed: a plain command line binary run
   from a terminal dies here even with NSCameraUsageDescription embedded in
   its own __TEXT,__info_plist and covered by its signature, because TCC
   attributes the request to the responsible process -- the terminal -- and
   neither Terminal.app nor Emacs.app declares one.

   So the requirement lands on whoever links this library: the program must
   carry NSCameraUsageDescription and be attributed to itself, which in
   practice means an app bundle launched through LaunchServices. See the
   capture section of BUILDING.md. Nothing in im_tests may call Connect for
   the same reason -- it would abort the suite rather than fail a case. */
static int vc_CheckAuthorization(void)
{
  AVAuthorizationStatus status =
    [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];

  if (status == AVAuthorizationStatusAuthorized)
    return 1;

  if (status == AVAuthorizationStatusDenied ||
      status == AVAuthorizationStatusRestricted)
  {
    /* No prompt is possible from here -- the user has to change it in System
       Settings. Returning at once matters: asking anyway would come back NO
       after a round trip and read as a hang. */
    return 0;
  }

  /* NotDetermined: ask once. The completion handler runs on an arbitrary
     dispatch queue rather than the main one, so blocking here cannot deadlock
     even when the caller is the main thread and there is no run loop -- the
     prompt is drawn by another process. The wait is bounded so an ignored
     dialog does not wedge the caller for good; a grant that lands later is
     picked up by the next Connect. */
  __block int granted = 0;
  dispatch_semaphore_t answered = dispatch_semaphore_create(0);

  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL allowed) {
                             granted = allowed? 1: 0;
                             dispatch_semaphore_signal(answered);
                           }];

  if (dispatch_semaphore_wait(answered,
        dispatch_time(DISPATCH_TIME_NOW,
                      (int64_t)VC_AUTH_TIMEOUT * NSEC_PER_SEC)) != 0)
    return 0;

  return granted;
}


/*************************************************************************
                         Connection lifecycle
*************************************************************************/

/* Empties the frame slot. Caller holds slot_mutex. */
static void vc_FlushSlotLocked(imVideoCapture* vc)
{
  if (vc->slot_buffer)
  {
    CVPixelBufferRelease(vc->slot_buffer);
    vc->slot_buffer = NULL;
  }
  vc->slot_full = 0;
}

void imVideoCaptureDisconnect(imVideoCapture* vc)
{
  assert(vc);
  if (!vc || vc->device == -1)
    return;

  @autoreleasepool
  {
    /* The order below is the whole of the difficulty in this file.

       1. Release anyone blocked in Frame(timeout = -1). Done under the mutex,
          then unlocked -- the mutex must NOT be held across step 3, because
          the delegate takes it and the dispatch_sync would deadlock. */
    pthread_mutex_lock(&vc->slot_mutex);
    vc->stopping = 1;
    pthread_cond_broadcast(&vc->slot_cond);
    pthread_mutex_unlock(&vc->slot_mutex);

    /* 2. Stop producing. */
    if (vc->output)
      [vc->output setSampleBufferDelegate:nil queue:NULL];
    if (vc->session && vc->session.isRunning)
      [vc->session stopRunning];

    /* 3. The barrier. Neither call in step 2 promises that a callback is not
          running right now or already enqueued. Dispatching an empty block
          synchronously onto the same SERIAL queue returns only once every
          block submitted before it has finished, so after this line no
          delegate callback is running or pending. Everything below -- and
          pthread_mutex_destroy in Destroy -- depends on it. */
    if (vc->queue)
      dispatch_sync(vc->queue, ^{ });

    /* 4. Only now is it safe to drop the objects and the back pointer. */
    if (vc->delegate)
      vc->delegate.vc = NULL;

    if (vc->session)
    {
      if (vc->input)  [vc->session removeInput:vc->input];
      if (vc->output) [vc->session removeOutput:vc->output];
    }

    vc->delegate  = nil;
    vc->output    = nil;
    vc->input     = nil;
    vc->session   = nil;
    vc->av_device = nil;
    vc->queue     = nil;

    pthread_mutex_lock(&vc->slot_mutex);
    vc_FlushSlotLocked(vc);
    vc->stopping = 0;
    pthread_mutex_unlock(&vc->slot_mutex);

    vc->live = 0;
    vc->device = -1;
    vc->width = 0;
    vc->height = 0;
  }
}


/*************************************************************************
   Still to come. Defined now so the library links and the device half can
   be used and tested on its own; each is filled in by the phase named.

     Connect / Live                     session set-up and start/stop
     GetImageSize / SetImageSize        the active format
     Frame / OneFrame                   the delegate and the frame slot
     FormatCount / GetFormat / SetFormat  resolution enumeration

   Until then they report failure, which is a state every caller of this API
   already has to handle.
*************************************************************************/

int imVideoCaptureConnect(imVideoCapture* vc, int device)
{
  assert(vc);
  if (!vc)
    return 0;

  if (device == -1)
    return vc->device;      /* the documented query form */

  /* ">=", where the reference writes "device > vc_DeviceCount"
     (im_capture_dx.cpp:1008) and lets the index one past the end through to a
     zeroed slot. */
  pthread_mutex_lock(&vc_ListMutex);
  int known = vc_CheckDeviceList(device);
  AVCaptureDevice* av_device = known? vc_DeviceObjects[device]: nil;
  pthread_mutex_unlock(&vc_ListMutex);

  if (!known || !av_device)
    return 0;

  if (vc->device == device)
    return 1;               /* already there; the reference does the same */

  if (vc->device != -1)
    imVideoCaptureDisconnect(vc);

  if (!vc_CheckAuthorization())
    return 0;

  @autoreleasepool
  {
    NSError* error = nil;
    AVCaptureDeviceInput* input =
      [AVCaptureDeviceInput deviceInputWithDevice:av_device error:&error];
    if (!input)
      return 0;

    AVCaptureSession* session = [[AVCaptureSession alloc] init];
    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];

    /* Drop frames rather than queue them behind us. The slot holds one frame
       and the newest wins, so a backlog would only ever be discarded later. */
    output.alwaysDiscardsLateVideoFrames = YES;

    [session beginConfiguration];

    if (![session canAddInput:input] || ![session canAddOutput:output])
    {
      [session commitConfiguration];
      return 0;
    }
    [session addInput:input];
    [session addOutput:output];

    /* 32BGRA is asked for rather than assumed. availableVideoCVPixelFormatTypes
       is only populated once the output belongs to a session, which is why the
       check sits inside the configuration block. Getting this wrong would show
       up as garbled colour rather than an error, so it is worth the check:
       every macOS camera offers BGRA, and its bytes are B,G,R,A in memory,
       which is the same channel order the DirectShow backend consumes. */
    NSNumber* wanted = @(kCVPixelFormatType_32BGRA);
    if (![output.availableVideoCVPixelFormatTypes containsObject:wanted])
    {
      [session commitConfiguration];
      return 0;
    }
    output.videoSettings = @{ (id)kCVPixelBufferPixelFormatTypeKey : wanted };

    [session commitConfiguration];

    imCaptureDelegate* delegate = [[imCaptureDelegate alloc] init];
    delegate.vc = vc;

    /* Serial by construction: dispatch_queue_create with a NULL attribute.
       Both the newest-wins slot and the teardown barrier in Disconnect depend
       on it being serial. */
    dispatch_queue_t queue =
      dispatch_queue_create("br.puc-rio.tecgraf.im.capture", NULL);

    [output setSampleBufferDelegate:delegate queue:queue];

    /* The size the caller will be told about, and the size the delegate
       checks each arriving buffer against. */
    CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(
      (CMVideoFormatDescriptionRef)av_device.activeFormat.formatDescription);

    vc->av_device = av_device;
    vc->session   = session;
    vc->input     = input;
    vc->output    = output;
    vc->delegate  = delegate;
    vc->queue     = queue;
    vc->width     = dimensions.width;
    vc->height    = dimensions.height;
    vc->device    = device;

    return 1;
  }
}

int imVideoCaptureLive(imVideoCapture* vc, int live)
{
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1)
    return 0;

  if (live == -1)
    return vc->live;        /* the documented query form */

  if (live && vc->live)
    return 1;
  if (!live && !vc->live)
    return 1;

  @autoreleasepool
  {
    if (live)
    {
      /* startRunning blocks until the session starts or fails -- typically a
         few hundred milliseconds on a built-in camera, which is what the
         reference's Sleep(VC_CAMERADELAY) (im_capture_dx.cpp:879) was
         compensating for. A synchronous C API has nowhere else to put that
         wait, so Live(1) is slow by construction.

         Never called holding slot_mutex: the rule in this file is that no
         AVFoundation call happens under that lock. */
      [vc->session startRunning];

      /* isRunning is a second net under vc_CheckAuthorization. If access were
         revoked between the two, the session simply fails to start and posts
         a notification we deliberately do not observe -- testing the flag
         catches it without needing an observer, and so without needing a run
         loop. */
      if (!vc->session.isRunning)
        return 0;

      vc->live = 1;
      return 1;
    }
    else
    {
      [vc->session stopRunning];

      /* Anything already in the slot was captured before the stop and would be
         handed out as though it were current. */
      pthread_mutex_lock(&vc->slot_mutex);
      vc_FlushSlotLocked(vc);
      pthread_mutex_unlock(&vc->slot_mutex);

      vc->live = 0;
      return 1;
    }
  }
}

void imVideoCaptureGetImageSize(imVideoCapture* vc, int *width, int *height)
{
  assert(vc);
  if (!vc)
  {
    if (width)  *width = 0;
    if (height) *height = 0;
    return;
  }

  /* "width and height returns 0 if not connected", which is what the zeroed
     members hold until Connect sets them. */
  if (width)  *width  = vc->width;
  if (height) *height = vc->height;
}

int imVideoCaptureSetImageSize(imVideoCapture* vc, int width, int height)
{
  (void)width; (void)height;
  assert(vc);
  return 0;
}

int imVideoCaptureFrame(imVideoCapture* vc, unsigned char* data, int color_mode, int timeout)
{
  (void)data; (void)color_mode; (void)timeout;
  assert(vc);
  return 0;
}

int imVideoCaptureOneFrame(imVideoCapture* vc, unsigned char* data, int color_mode)
{
  (void)data; (void)color_mode;
  assert(vc);
  return 0;
}

int imVideoCaptureFormatCount(imVideoCapture* vc)
{
  assert(vc);
  return 0;
}

int imVideoCaptureGetFormat(imVideoCapture* vc, int format, int *width, int *height, char* desc)
{
  (void)format; (void)desc;
  assert(vc);
  if (width)  *width = 0;
  if (height) *height = 0;
  return 0;
}

int imVideoCaptureSetFormat(imVideoCapture* vc, int format)
{
  assert(vc);
  if (format == -1)
    return -1;
  return 0;
}
