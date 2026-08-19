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
#include <CoreMediaIO/CMIOHardware.h>
#include <CoreMediaIO/CMIOHardware.h>

#include <pthread.h>
#include <errno.h>
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
                          Sizes and formats
*************************************************************************/

/* Resolution on macOS is decided by the session's preset, NOT by
   AVCaptureDevice.activeFormat.

   Worth stating plainly, because the opposite is the natural assumption, the
   device happily accepts the format, and the disagreement is silent. Measured
   on a Logitech BRIO: after setting activeFormat to 176x144 the device
   reported activeFormat as 176x144 while the session's input port -- which is
   what the camera actually delivers -- still reported 1920x1080, with the
   preset sitting at AVCaptureSessionPresetHigh, and frames kept arriving at
   1920x1080. iOS resolves this with AVCaptureSessionPresetInputPriority, which
   tells the session to defer to the device; that constant does not exist on
   macOS.

   So the settable sizes are the size-named presets, and this table is all of
   them. imVideoCaptureGetFormat lists the subset a device's session accepts.

   Accepting is not honouring, and the gap is wider than it ought to be. On a
   FaceTime HD camera, canSetSessionPreset agrees to every entry below, the
   preset reads back as the one that was set, and the session delivers
   1920x1080 regardless -- with a matching AVCaptureDevice.activeFormat set at
   the same time as well, which is the only combination left to try. Measured
   directly against AVFoundation rather than through this file, so it is the
   framework's behaviour on this hardware, not a bug here: an
   AVCaptureVideoDataOutput on that camera hands back the sensor's native size
   whatever it is asked for.

   Which leaves GetFormat listing candidates rather than guarantees, and
   imVideoCaptureSetImageSize as the only authority -- it starts the session to
   find out what actually arrives, and refuses anything else. A GetFormat that
   only ever offered guarantees would have to probe every preset at connect
   time, which is a second apiece with the camera light coming on for each, and
   that is too high a price for a list. */

struct vcPreset
{
  __unsafe_unretained AVCaptureSessionPreset name;
  int width, height;
};

static vcPreset vc_PresetTable[] =
{
  { AVCaptureSessionPreset320x240,    320,  240 },
  { AVCaptureSessionPreset352x288,    352,  288 },
  { AVCaptureSessionPreset640x480,    640,  480 },
  { AVCaptureSessionPreset960x540,    960,  540 },
  { AVCaptureSessionPreset1280x720,  1280,  720 },
  { AVCaptureSessionPreset1920x1080, 1920, 1080 },
  { AVCaptureSessionPreset3840x2160, 3840, 2160 },
};

#define VC_PRESET_COUNT ((int)(sizeof(vc_PresetTable)/sizeof(vc_PresetTable[0])))

/* The size a preset names, or 0x0 for one not in the table -- which includes
   the defaults, Photo/High/Medium/Low, whose dimensions are deliberately
   unspecified and device-dependent. */
static void vc_PresetSize(AVCaptureSessionPreset preset, int* width, int* height)
{
  *width = 0;
  *height = 0;
  for (int i = 0; i < VC_PRESET_COUNT; i++)
  {
    if ([preset isEqualToString:vc_PresetTable[i].name])
    {
      *width  = vc_PresetTable[i].width;
      *height = vc_PresetTable[i].height;
      return;
    }
  }
}


/*************************************************************************
                              Attributes
*************************************************************************/

/* IM's attribute model is DirectShow's: ask the driver for a range, then get
   and set a value inside it, which is exactly what a percentage needs.
   AVFoundation on macOS cannot answer that. It gives mode enums --
   exposureMode, focusMode, whiteBalanceMode -- and every property carrying an
   actual value is iOS only: exposureDuration, ISO, lensPosition,
   deviceWhiteBalanceGains, videoZoomFactor and the AVCaptureDeviceFormat
   min/max accessors are all API_UNAVAILABLE(macos). Brightness, contrast, hue,
   saturation, sharpness, gamma, gain, iris, pan and tilt are absent from
   AVFoundation on every platform.

   CoreMediaIO is the structural match, and close to an exact one:

     kCMIOFeatureControlPropertyNativeRange       IAMVideoProcAmp::GetRange
     kCMIOFeatureControlPropertyNativeValue       Get / Set
     kCMIOFeatureControlPropertyAutomaticManual   VideoProcAmp_Flags_Auto

   Measured, because none of it is guessable from the headers. On a Logitech
   BRIO the device's owned objects include nine controls with real ranges --
   brightness, contrast, saturation and sharpness at 0..255, gain at 0..255,
   exposure at 3..2047, zoom at 100..500, focus at 0..255, backlight
   compensation at 0..1 -- every one of them settable. On the built-in FaceTime
   HD camera, and on an iPhone over Continuity, the same enumeration returns
   ZERO owned objects.

   So this is worth having and not worth promising: a USB camera gets most of
   the list and a Mac's own camera gets none of it. That is the API working as
   documented rather than failing -- imVideoCaptureGetAttribute returning zero
   is the documented way to ask whether an attribute is supported.

   Also measured: none of this needs camera authorisation. Every range and
   value above was read from a plain command line binary with no bundle and no
   TCC prompt, which is the opposite of imVideoCaptureConnect. */

struct vcAttrib
{
  const char* name;
  CMIOClassID control_class;      /* 0 when macOS has no counterpart */
  CMIOClassID alternate_class;    /* second candidate, or 0 */
};

#define VC_ATTRIB_COUNT 20

/* In the order include/im_capture.h documents them, so GetAttributeList reports
   them in that order too -- which is what the reference does. */
static const vcAttrib vc_AttribTable[VC_ATTRIB_COUNT] =
{
  { "VideoBrightness",            kCMIOBrightnessControlClassID,            0 },
  { "VideoContrast",              kCMIOContrastControlClassID,              0 },
  { "VideoHue",                   kCMIOHueControlClassID,                   0 },
  { "VideoSaturation",            kCMIOSaturationControlClassID,            0 },
  { "VideoSharpness",             kCMIOSharpnessControlClassID,             0 },
  { "VideoGamma",                 kCMIOGammaControlClassID,                 0 },

  /* DirectShow's colour-kill switch. Nothing in CoreMediaIO. */
  { "VideoColorEnable",           0,                                        0 },

  /* The header calls this "a color temperature in degrees Kelvin", which is
     literally kCMIOTemperatureControlClassID, while
     kCMIOWhiteBalanceControlClassID is the generic control. Generic first,
     temperature as the fallback. */
  { "VideoWhiteBalance",          kCMIOWhiteBalanceControlClassID,
                                  kCMIOTemperatureControlClassID },

  { "VideoBacklightCompensation", kCMIOBacklightCompensationControlClassID, 0 },
  { "VideoGain",                  kCMIOGainControlClassID,                  0 },

  /* kCMIOPanControlClassID rather than kCMIOPanTiltAbsoluteControlClassID: the
     absolute and relative pan/tilt/zoom controls carry NativeData, a packed
     structure, instead of a NativeValue, so they cannot be one percentage.
     Observed on a BRIO, whose 'ptab' reports a NativeRange of [nan..7.6e-310]
     -- garbage the range check below rejects anyway. */
  { "CameraPanAngle",             kCMIOPanControlClassID,                   0 },
  { "CameraTiltAngle",            kCMIOTiltControlClassID,                  0 },
  { "CameraRollAngle",            kCMIORollAbsoluteControlClassID,          0 },
  { "CameraLensZoom",             kCMIOZoomControlClassID,                  0 },

  /* DirectShow's Exposure is a shutter time; CoreMediaIO splits the two. */
  { "CameraExposure",             kCMIOExposureControlClassID,
                                  kCMIOShutterControlClassID },

  { "CameraIris",                 kCMIOIrisControlClassID,                  0 },
  { "CameraFocus",                kCMIOFocusControlClassID,                 0 },

  /* IAMVideoControl mode bits. No CoreMediaIO counterpart. */
  { "FlipHorizontal",             0,                                        0 },
  { "FlipVertical",               0,                                        0 },

  /* AnalogVideoStandard ordinals for an analogue capture card. Nothing on
     macOS has any idea what NTSC_M is. */
  { "AnalogFormat",               0,                                        0 },
};

/* CoreMediaIO also defines BlackLevel, WhiteLevel, WhiteBalanceU/V,
   PowerLineFrequency, NoiseReduction and OpticalFilter, and a BRIO does expose
   PowerLineFrequency among others. They are not surfaced because the name set
   is fixed by include/im_capture.h and shared with the DirectShow backend --
   adding one is a public API change, not a macOS detail. */

/* Fixed-size read of one CoreMediaIO property. Everything this file wants is
   fixed size and global scope. */
static int vc_CMIOGet(CMIOObjectID object, CMIOObjectPropertySelector selector,
                      UInt32 size, void* out)
{
  CMIOObjectPropertyAddress address =
    { selector, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
  UInt32 used = 0;

  return CMIOObjectGetPropertyData(object, &address, 0, NULL, size, &used, out)
           == kCMIOHardwareNoError && used == size;
}

static int vc_CMIOSet(CMIOObjectID object, CMIOObjectPropertySelector selector,
                      UInt32 size, const void* value)
{
  CMIOObjectPropertyAddress address =
    { selector, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };

  return CMIOObjectSetPropertyData(object, &address, 0, NULL, size, (void*)value)
           == kCMIOHardwareNoError;
}

static int vc_CMIOSettable(CMIOObjectID object, CMIOObjectPropertySelector selector)
{
  CMIOObjectPropertyAddress address =
    { selector, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
  Boolean settable = false;

  return CMIOObjectIsPropertySettable(object, &address, &settable)
           == kCMIOHardwareNoError && settable;
}

/* The percentage conversions, ported from im_capture_dx.cpp:1817 and :1822.

   The formula is the reference's; the types are not. DirectShow's GetRange
   yields long Min/Max/Step, CoreMediaIO's NativeRange is a pair of Float64 with
   no step at all, so the step-rounding arm of vc_Percent2Value has no input
   here and is gone.

   Two guards the reference does not have. A degenerate range would divide by
   zero and hand the caller a NaN through a double* -- a BRIO reports 0..0 for
   its temperature and power-line-frequency controls and [nan..7.6e-310] for
   pan-tilt-absolute -- so it is refused as unsupported instead. And the
   percentage is clamped rather than extrapolated: SetAttribute(vc, "VideoGain",
   150) drives the reference straight out of range and leaves the driver to
   cope.

   External linkage, and deliberately not in src/im_capture.def: this is the
   part of the attribute code whose correctness cannot be shown from a camera,
   so test_capture.cpp declares it and drives it with known ranges. The same
   arrangement as imVideoCaptureConvertBGRA, and for the same reason. */
int imVideoCaptureValue2Percent(double minimum, double maximum,
                                double value, double* percent)
{
  if (!percent)
    return 0;

  *percent = 0;

  /* Written as a negated comparison so that a NaN bound fails it: every
     comparison against NaN is false. */
  if (!(maximum > minimum))
    return 0;

  if (value < minimum) value = minimum;
  if (value > maximum) value = maximum;

  *percent = ((value - minimum) * 100.0) / (maximum - minimum);
  return 1;
}

int imVideoCapturePercent2Value(double minimum, double maximum,
                                double percent, double* value)
{
  if (!value)
    return 0;

  *value = 0;

  if (!(maximum > minimum))
    return 0;

  if (percent < 0.0)   percent = 0.0;
  if (percent > 100.0) percent = 100.0;

  *value = (percent / 100.0) * (maximum - minimum) + minimum;
  return 1;
}

/* The CMIODeviceID whose UID matches this AVCaptureDevice's uniqueID.

   Enumerating and comparing rather than using kCMIOHardwarePropertyDeviceForUID,
   because "the two identifier spaces agree" is an assumption worth being able
   to watch fail. Measured: they agree exactly on a USB camera, on the built-in
   camera and on an iPhone over Continuity. */
static CMIODeviceID vc_ResolveCMIODevice(NSString* unique_id)
{
  if (!unique_id)
    return 0;

  CMIOObjectPropertyAddress address = { kCMIOHardwarePropertyDevices,
                                        kCMIOObjectPropertyScopeGlobal,
                                        kCMIOObjectPropertyElementMain };
  UInt32 bytes = 0;
  if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, NULL, &bytes)
        != kCMIOHardwareNoError || bytes == 0)
    return 0;

  CMIODeviceID* devices = (CMIODeviceID*)malloc(bytes);
  if (!devices)
    return 0;

  UInt32 used = 0;
  CMIODeviceID found = 0;
  if (CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, NULL,
                                bytes, &used, devices) == kCMIOHardwareNoError)
  {
    int count = (int)(used / sizeof(CMIODeviceID));
    for (int i = 0; i < count && found == 0; i++)
    {
      CFStringRef device_uid = NULL;
      if (vc_CMIOGet(devices[i], kCMIODevicePropertyDeviceUID,
                     sizeof(device_uid), &device_uid) && device_uid)
      {
        /* Core Foundation, so released by hand even under ARC -- the same
           discipline the frame slot uses for its CVPixelBufferRef. */
        if ([(__bridge NSString*)device_uid isEqualToString:unique_id])
          found = devices[i];
        CFRelease(device_uid);
      }
    }
  }

  free(devices);
  return found;
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

  /* Set when the camera has gone away or the session has failed. Kept separate
     from "live" on purpose: imVideoCaptureFrame asserts live, and a preview
     loop is Live(1) followed by Frame in a loop, so zeroing live from
     underneath it would abort a debug build for a caller that did nothing
     wrong. live still means "the caller asked for it and startRunning
     succeeded"; imVideoCaptureLive(vc,-1) reports live && !failed, which is
     the answer a caller actually wants. */
  int              failed;
  int              failure_reported;   /* per handle, not per process */

  /* Set once a delivered frame has told us the real size. Until then the
     delegate accepts whatever arrives instead of checking it, because there is
     nothing trustworthy to check against yet -- see vc_LearnDeliveredSize. */
  int              size_known;

  /* The public format numbering: the session presets this device accepts, in
     increasing size. The same job the reference's format_map[] does for
     DirectShow's enumeration. */
  NSArray<AVCaptureSessionPreset>* format_list;

  /* CoreMediaIO's view of the same camera, resolved from av_device.uniqueID at
     Connect. The attributes live there rather than in AVFoundation, which on
     macOS exposes exposure and focus as mode enums and has no brightness,
     contrast, saturation, sharpness or gain at all -- see vc_AttribTable. */
  CMIODeviceID cmio_device;                     /* 0 when unresolved */
  CMIOObjectID cmio_control[VC_ATTRIB_COUNT];   /* index-matched to vc_AttribTable */
  const char*  attrib_list[VC_ATTRIB_COUNT];    /* the subset actually present */
  int          attrib_count;
};


/* Records which of the table's controls this device actually has.

   Every owned object is examined once and matched against the table, rather
   than searching for each attribute in turn, which would walk the same list
   twenty times. The qualifier that kCMIOObjectPropertyOwnedObjects accepts is
   not used: it is documented to match subclasses, but on a BRIO it reported
   twelve matches against a list that holds more feature controls than that, so
   the properties are tested for directly instead.

   A control is recorded only if it has a usable NativeRange. One that exists
   and reports 0..0, as a BRIO's temperature control does, would otherwise be
   listed by GetAttributeList and then refused by GetAttribute -- which the
   header permits, but which is less use than not listing it. */
static void vc_RefreshControls(imVideoCapture* vc)
{
  memset(vc->cmio_control, 0, sizeof(vc->cmio_control));
  vc->attrib_count = 0;

  if (vc->cmio_device == 0)
    return;

  CMIOObjectPropertyAddress owned = { kCMIOObjectPropertyOwnedObjects,
                                      kCMIOObjectPropertyScopeGlobal,
                                      kCMIOObjectPropertyElementMain };
  UInt32 bytes = 0;
  if (CMIOObjectGetPropertyDataSize(vc->cmio_device, &owned, 0, NULL, &bytes)
        != kCMIOHardwareNoError || bytes == 0)
    return;

  CMIOObjectID* objects = (CMIOObjectID*)malloc(bytes);
  if (!objects)
    return;

  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(vc->cmio_device, &owned, 0, NULL,
                                bytes, &used, objects) == kCMIOHardwareNoError)
  {
    int count = (int)(used / sizeof(CMIOObjectID));
    for (int i = 0; i < count; i++)
    {
      CMIOClassID control_class = 0;
      if (!vc_CMIOGet(objects[i], kCMIOObjectPropertyClass,
                      sizeof(control_class), &control_class))
        continue;

      AudioValueRange range = { 0, 0 };
      if (!vc_CMIOGet(objects[i], kCMIOFeatureControlPropertyNativeRange,
                      sizeof(range), &range))
        continue;
      if (!(range.mMaximum > range.mMinimum))
        continue;                     /* 0..0, or nan, is not a range */

      for (int a = 0; a < VC_ATTRIB_COUNT; a++)
      {
        if (vc->cmio_control[a])
          continue;                   /* first match wins, so a primary class
                                         beats an alternate */
        if ((vc_AttribTable[a].control_class &&
             vc_AttribTable[a].control_class == control_class) ||
            (vc_AttribTable[a].alternate_class &&
             vc_AttribTable[a].alternate_class == control_class))
        {
          vc->cmio_control[a] = objects[i];
          break;
        }
      }
    }
  }

  free(objects);

  for (int a = 0; a < VC_ATTRIB_COUNT; a++)
    if (vc->cmio_control[a])
      vc->attrib_list[vc->attrib_count++] = vc_AttribTable[a].name;
}

/* The table row for a name, or -1.

   A linear scan of twenty strcmp, where the reference builds a 101-bucket hash
   (im_capture_dx.cpp:2105) with no collision handling and a size chosen
   empirically so that exactly those twenty names happen not to collide. Note
   also that it returns 0 for "not found" while its callers test for -1, so an
   unknown name there silently becomes VideoProcAmp_Brightness. -1 here is
   unambiguous. */
static int vc_AttribIndex(const char* attrib)
{
  if (!attrib)
    return -1;

  for (int i = 0; i < VC_ATTRIB_COUNT; i++)
    if (strcmp(attrib, vc_AttribTable[i].name) == 0)
      return i;

  return -1;
}

/* The control backing an attribute, or 0 when this device does not have it. */
static CMIOObjectID vc_ControlFor(imVideoCapture* vc, const char* attrib)
{
  int index = vc_AttribIndex(attrib);
  if (index < 0)
    return 0;

  return vc->cmio_control[index];
}

@implementation imCaptureDelegate

/* Runs on the serial queue the handle owns. Its only job is to move the newest
   frame into the slot: the conversion happens on the caller's thread in
   imVideoCaptureFrame, after the lock is dropped, so a slow consumer cannot
   stall capture. The reference does the opposite -- a full-frame CopyMemory
   inside SampleCB (im_capture_dx.cpp:153) -- because DirectShow hands it a
   buffer it may not keep, whereas a CVPixelBuffer can simply be retained. */
- (void)captureOutput:(AVCaptureOutput*)output
  didOutputSampleBuffer:(CMSampleBufferRef)sample
         fromConnection:(AVCaptureConnection*)connection
{
  (void)output; (void)connection;

  imVideoCapture* vc = self.vc;
  if (!vc)
    return;

  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer)
    return;

  pthread_mutex_lock(&vc->slot_mutex);

  /* The dimension test is a bounds check, not a nicety. The caller sized its
     buffer from imVideoCaptureGetImageSize, so a frame of any other size would
     be converted straight past the end of it. AVFoundation can deliver one
     after a format change, or when a device renegotiates. Dropping it loses a
     frame; converting it corrupts the heap. */
  int buffer_width  = (int)CVPixelBufferGetWidth(buffer);
  int buffer_height = (int)CVPixelBufferGetHeight(buffer);

  if (!vc->stopping && !vc->size_known)
  {
    /* The first frame is the authority on the size. Adopt it rather than
       measure it against a guess: everything AVFoundation offers beforehand --
       the device's activeFormat, the session's preset, the input port's format
       description -- has been observed to disagree with what is actually
       delivered. */
    vc->width  = buffer_width;
    vc->height = buffer_height;
    vc->size_known = 1;
  }

  if (!vc->stopping && buffer_width == vc->width && buffer_height == vc->height)
  {
    if (vc->slot_buffer)
      CVPixelBufferRelease(vc->slot_buffer);   /* newest wins */

    vc->slot_buffer = buffer;
    CVPixelBufferRetain(vc->slot_buffer);      /* CF, so retained by hand under ARC */
    vc->slot_full = 1;

    pthread_cond_signal(&vc->slot_cond);
  }
  else if (!vc->stopping)
  {
    /* Every frame is being dropped, so imVideoCaptureFrame will return 0 for
       ever and there is nothing in the API to say why. Reported once, on
       stderr, because a library that goes permanently silent is worse than one
       that prints a line: the sizes are the whole diagnosis and the caller
       cannot obtain them any other way. */
    static int reported = 0;
    if (!reported)
    {
      reported = 1;
      fprintf(stderr,
        "im_capture: dropping every frame -- the device is delivering %dx%d "
        "but imVideoCaptureGetImageSize reports %dx%d, so converting would "
        "write past the caller's buffer.\n",
        buffer_width, buffer_height, vc->width, vc->height);
    }
  }

  pthread_mutex_unlock(&vc->slot_mutex);
}

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
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1)
    return 0;

  CMIOObjectID control = vc_ControlFor(vc, attrib);
  if (!control)
    return 0;

  /* The reference writes the driver's advertised Default value and, when fauto
     is set and the driver says it can, switches to automatic as well
     (im_capture_dx.cpp:1866). CoreMediaIO has no counterpart to Default. The
     feature control properties are OnOff, AutomaticManual, AbsoluteNative,
     Tune, NativeValue, AbsoluteValue, NativeRange, AbsoluteRange, the two
     converters, AbsoluteUnitName, NativeData and NativeDataRange -- and that is
     the whole list. There is no value to reset to.

     So fauto is the only half that can be honoured, and it is honoured exactly.
     With fauto clear there is nothing truthful to do and this returns 0.

     Rejected: writing the midpoint of NativeRange, or the value read when the
     device was connected, and calling either "the default". Both are
     fabrications that would return 1, and a caller cannot tell a fabricated
     success from a real one -- which is the failure this file goes out of its
     way to avoid elsewhere. */
  if (!fauto)
    return 0;

  if (!vc_CMIOSettable(control, kCMIOFeatureControlPropertyAutomaticManual))
    return 0;

  UInt32 automatic = 1;
  return vc_CMIOSet(control, kCMIOFeatureControlPropertyAutomaticManual,
                    sizeof(automatic), &automatic)? 1: 0;
}

int imVideoCaptureGetAttribute(imVideoCapture* vc, const char* attrib, double *percent)
{
  assert(vc);
  assert(vc->device != -1);

  /* Written before anything can fail, so a caller that ignores the return
     value does not read a stale double. */
  if (percent)
    *percent = 0;

  if (!vc || vc->device == -1 || !percent)
    return 0;

  CMIOObjectID control = vc_ControlFor(vc, attrib);
  if (!control)
    return 0;      /* unknown name, or a device without this control */

  AudioValueRange range = { 0, 0 };
  Float32 value = 0;
  if (!vc_CMIOGet(control, kCMIOFeatureControlPropertyNativeRange, sizeof(range), &range) ||
      !vc_CMIOGet(control, kCMIOFeatureControlPropertyNativeValue, sizeof(value), &value))
    return 0;

  return imVideoCaptureValue2Percent(range.mMinimum, range.mMaximum,
                                     (double)value, percent);
}

int imVideoCaptureSetAttribute(imVideoCapture* vc, const char* attrib, double percent)
{
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1)
    return 0;

  CMIOObjectID control = vc_ControlFor(vc, attrib);
  if (!control)
    return 0;

  /* Refused before the write rather than after it. Attempting a write to a
     read-only control would produce an OSStatus we would report the same way,
     but only after a round trip through the DAL. */
  if (!vc_CMIOSettable(control, kCMIOFeatureControlPropertyNativeValue))
    return 0;

  AudioValueRange range = { 0, 0 };
  if (!vc_CMIOGet(control, kCMIOFeatureControlPropertyNativeRange, sizeof(range), &range))
    return 0;

  double value = 0;
  if (!imVideoCapturePercent2Value(range.mMinimum, range.mMaximum, percent, &value))
    return 0;

  /* The reference passes VideoProcAmp_Flags_Manual on every Set
     (im_capture_dx.cpp:1849), and that is not decoration: a control left under
     automatic control ignores a manual write, or overwrites it on the next
     frame. CoreMediaIO expresses the same thing as a separate property, so it
     is set separately -- and only when the control has it and will take it,
     since a control with no automatic mode is already manual. */
  if (vc_CMIOSettable(control, kCMIOFeatureControlPropertyAutomaticManual))
  {
    UInt32 automatic = 0;
    vc_CMIOSet(control, kCMIOFeatureControlPropertyAutomaticManual,
               sizeof(automatic), &automatic);
  }

  Float32 native = (Float32)value;
  return vc_CMIOSet(control, kCMIOFeatureControlPropertyNativeValue,
                    sizeof(native), &native)? 1: 0;
}

const char** imVideoCaptureGetAttributeList(imVideoCapture* vc, int *num_attrib)
{
  assert(vc);
  assert(vc->device != -1);

  if (num_attrib)
    *num_attrib = 0;

  if (!vc || vc->device == -1 || vc->attrib_count == 0)
    return NULL;

  if (num_attrib)
    *num_attrib = vc->attrib_count;

  /* The array is the handle's and is valid until Disconnect; the strings in it
     are the literals in vc_AttribTable and outlive everything.

     Deviation from im_capture_dx.cpp:2196, whose list is a function-static
     array shared by every handle in the process -- two handles alternating
     GetAttributeList calls there overwrite each other's list with no way for
     either to notice.

     This list is also narrower than the reference's, and deliberately: that one
     returns a whole interface's block of names as soon as the device supports
     the interface at all, leaving the caller to discover which of them fail.
     This one returns the controls the device actually has, so every name in it
     answers imVideoCaptureGetAttribute. */
  return vc->attrib_list;
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

/* Runs the session until a frame arrives, so the delegate can record the size
   that is really being delivered. Returns non-zero if it learned one.

   This exists because nothing AVFoundation will tell you in advance is
   reliable. Measured on one camera, in one session: AVCaptureDevice
   .activeFormat said 640x480, the input port said 1920x1080, and the delivery
   was 1920x1080; later, after the device's activeFormat had been changed by
   another process, the port said 1280x720 and the delivery was still
   1920x1080. Each of those was tried as the source of truth and each was wrong
   in some configuration. A frame that has actually arrived cannot be.

   The cost is that Connect runs the camera for a moment. That buys a
   GetImageSize which is correct however the caller sequences its calls, and it
   removes the failure this replaces -- every frame silently dropped for a size
   mismatch, with imVideoCaptureFrame returning 0 for ever. */
static int vc_LearnDeliveredSize(imVideoCapture* vc)
{
  [vc->session startRunning];
  if (!vc->session.isRunning)
    return 0;

  uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC)
                    + 2000ull * 1000000ull;      /* 2s: generous for a cold camera */

  pthread_mutex_lock(&vc->slot_mutex);
  while (!vc->size_known && !vc->stopping)
  {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (now >= deadline)
      break;

    uint64_t remaining = deadline - now;
    struct timespec ts;
    ts.tv_sec  = (time_t)(remaining / 1000000000ull);
    ts.tv_nsec = (long)  (remaining % 1000000000ull);
    if (pthread_cond_timedwait_relative_np(&vc->slot_cond, &vc->slot_mutex, &ts) != 0)
      break;
  }
  int learned = vc->size_known;
  vc_FlushSlotLocked(vc);      /* the frame was for measuring, not for keeping */
  pthread_mutex_unlock(&vc->slot_mutex);

  [vc->session stopRunning];
  return learned;
}

#define VC_FAIL_DISCONNECTED 1
#define VC_FAIL_RUNTIME      2

/* Records that the camera is gone, wakes anyone waiting for a frame, and says
   so once.

   The API has no error channel -- every function returns 0 or a count -- so
   without this a vanished camera is indistinguishable from a slow one:
   imVideoCaptureFrame returns 0 for ever and imVideoCaptureLive(vc,-1) goes on
   claiming the capture is running. The stderr line is the same argument as the
   delegate's size-mismatch warning: a condition that makes every later call
   fail, with nothing in the API able to explain why, is worth one line.

   Reported once per handle rather than once per process, unlike that older
   warning, whose "static int reported" swallows the second handle's message. */
static void vc_ReportFailure(imVideoCapture* vc, int reason, NSError* error)
{
  int report;

  pthread_mutex_lock(&vc->slot_mutex);
  report = !vc->failure_reported;
  vc->failed = 1;
  vc->failure_reported = 1;
  pthread_cond_broadcast(&vc->slot_cond);      /* release a blocked Frame(-1) */
  pthread_mutex_unlock(&vc->slot_mutex);

  /* Printed after the lock is dropped: fprintf can block on a pipe, and the
     delegate must never wait behind it. */
  if (!report)
    return;

  fprintf(stderr,
    "im_capture: device %d %s -- imVideoCaptureFrame will return 0 from now on "
    "and imVideoCaptureLive(vc,-1) reports 0. To recover: disconnect this "
    "handle, call imVideoCaptureReloadDevices (the device index may change), "
    "then connect again. AVFoundation never revives an AVCaptureDevice once it "
    "reports disconnected, it publishes a new one.%s%s\n",
    vc->device,
    reason == VC_FAIL_DISCONNECTED? "has been disconnected": "stopped with an error",
    error? " Reported: ": "",
    error? error.localizedDescription.UTF8String: "");
}

/* Whether the camera is still there, asked synchronously.

   Both questions AVFoundation answers without a run loop and without an
   observer, which matters because this library is reached from plain C, from
   Lua, and from runtimes that dlopen it -- none of which is guaranteed to have
   one. AVCaptureDevice.h on -connected: "When the value of this property
   becomes NO for a given instance, it will not become YES again", so this is a
   latch rather than something that flickers.

   Never called holding slot_mutex, per the rule that no AVFoundation call
   happens under that lock. */
static int vc_CheckAlive(imVideoCapture* vc)
{
  if (vc->device == -1)
    return 1;
  if (vc->failed)
    return 0;

  int reason = 0;

  @autoreleasepool
  {
    if (vc->av_device && !vc->av_device.connected)
      reason = VC_FAIL_DISCONNECTED;
    else if (vc->live && vc->session && !vc->session.isRunning)
      reason = VC_FAIL_RUNTIME;
  }

  if (reason)
  {
    vc_ReportFailure(vc, reason, nil);
    return 0;
  }

  return 1;
}

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
    vc->session     = nil;
    vc->av_device   = nil;
    vc->queue       = nil;
    vc->format_list = nil;

    pthread_mutex_lock(&vc->slot_mutex);
    vc_FlushSlotLocked(vc);
    vc->stopping = 0;
    vc->failed = 0;
    vc->failure_reported = 0;
    pthread_mutex_unlock(&vc->slot_mutex);

    vc->live = 0;
    vc->device = -1;
    vc->width = 0;
    vc->height = 0;
    vc->size_known = 0;

    vc->cmio_device = 0;
    vc->attrib_count = 0;
    memset(vc->cmio_control, 0, sizeof(vc->cmio_control));
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

    /* The size the caller will be told about, and the size the delegate checks
       each arriving buffer against -- so it has to be what the camera will
       actually deliver, not what it could deliver.

       AVCaptureDevice.activeFormat is NOT that. The session applies its own
       preset, which on macOS overrides the device's format, and the two
       disagree: measured on a Logitech BRIO, activeFormat said 640x480 while
       the session delivered 1920x1080, so every frame failed the delegate's
       dimension check and imVideoCaptureFrame returned 0 for ever with nothing
       to say why. (iOS solves this with AVCaptureSessionPresetInputPriority,
       which tells the session to stand aside; that constant is unavailable on
       macOS.)

       Neither is the input's port, which is what this block reads. That was
       the second guess and it was wrong too: the port agreed with delivery at
       first, then reported 1280x720 against the same 1920x1080 once another
       process had changed the device's activeFormat, and the delegate dropped
       every frame again.

       So what is read here is a provisional figure and nothing more. It only
       survives if vc_LearnDeliveredSize, immediately below, fails to see a
       frame within two seconds -- and a frame that has actually arrived is the
       one authority that cannot disagree with the delivery. activeFormat is
       kept as a second fallback for a port that has published nothing. */
    CMVideoDimensions dimensions = { 0, 0 };

    AVCaptureInputPort* port = input.ports.firstObject;
    if (port && port.formatDescription)
      dimensions = CMVideoFormatDescriptionGetDimensions(
        (CMVideoFormatDescriptionRef)port.formatDescription);

    if (dimensions.width == 0 || dimensions.height == 0)
      dimensions = CMVideoFormatDescriptionGetDimensions(
        (CMVideoFormatDescriptionRef)av_device.activeFormat.formatDescription);

    if (dimensions.width == 0 || dimensions.height == 0)
      return 0;


    /* The presets this device accepts: exactly the sizes SetImageSize can
       deliver. */
    NSMutableArray<AVCaptureSessionPreset>* presets = [NSMutableArray array];
    for (int i = 0; i < VC_PRESET_COUNT; i++)
    {
      if ([session canSetSessionPreset:vc_PresetTable[i].name])
        [presets addObject:vc_PresetTable[i].name];
    }
    vc->format_list = presets;

    vc->av_device = av_device;
    vc->session   = session;
    vc->input     = input;
    vc->output    = output;
    vc->delegate  = delegate;
    vc->queue     = queue;
    vc->width      = dimensions.width;    /* provisional, see below */
    vc->height     = dimensions.height;
    vc->size_known = 0;
    vc->device     = device;

    /* Resolved eagerly rather than on first use. Authorisation has already
       been checked by this point, Connect already runs the camera for up to
       two seconds below, and a handful of property reads is noise against
       that -- while a lazy resolve would put the first CoreMediaIO call at an
       arbitrary later moment and leave a "not tried yet" state to reason
       about. Failing to resolve is not a Connect failure: capture works
       perfectly well with no attributes. */
    vc->cmio_device = vc_ResolveCMIODevice(av_device.uniqueID);
    vc_RefreshControls(vc);

    /* Replace the guess with the truth, by running the camera until a frame
       arrives and taking its dimensions. If nothing arrives the port's figure
       stands, which is the best guess available and no worse than before. */
    vc_LearnDeliveredSize(vc);

    return 1;
  }
}

int imVideoCaptureLive(imVideoCapture* vc, int live)
{
  assert(vc);
  if (!vc)
    return 0;

  /* The query form is answered before the connection is asserted. It is
     documented as "use -1 to return the current state", and the state of a
     handle that is not connected is a perfectly good answer -- zero. The
     reference asserts a connection for every form alike
     (im_capture_dx.cpp:1233), which makes asking a disconnected handle
     whether it is live abort a debug build rather than say "no". */
  if (live == -1)
  {
    /* Asks the camera rather than repeating what was asked for. This is the
       one place a caller that never blocks in Frame can find out the device
       has gone, and it costs two property reads. */
    if (vc->live && !vc_CheckAlive(vc))
      return 0;
    return vc->live && !vc->failed;
  }

  assert(vc->device != -1);
  if (vc->device == -1)
    return 0;

  if (live && vc->live)
    return 1;
  if (!live && !vc->live)
    return 1;

  @autoreleasepool
  {
    if (live)
    {
      /* A failed handle cannot be revived by starting it again -- AVFoundation
         publishes a new AVCaptureDevice rather than reviving this one -- so
         refuse rather than pay a startRunning timeout per attempt. */
      if (vc->failed)
        return 0;

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
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1)
    return 0;

  if (width == vc->width && height == vc->height)
    return 1;

  @autoreleasepool
  {
    AVCaptureSessionPreset wanted = nil;
    for (AVCaptureSessionPreset candidate in vc->format_list)
    {
      int candidate_width, candidate_height;
      vc_PresetSize(candidate, &candidate_width, &candidate_height);
      if (candidate_width == width && candidate_height == height)
      {
        wanted = candidate;
        break;
      }
    }

    /* Exact match or refuse. Choosing the nearest size instead would leave
       imVideoCaptureGetImageSize disagreeing with what the caller asked for
       and, worse, would be indistinguishable from success at the call site.
       The reference refuses too (im_capture_dx.cpp:1173). */
    if (!wanted || ![vc->session canSetSessionPreset:wanted])
      return 0;

    int was_live = vc->live;
    if (was_live && !imVideoCaptureLive(vc, 0))
      return 0;

    [vc->session beginConfiguration];
    vc->session.sessionPreset = wanted;
    [vc->session commitConfiguration];

    pthread_mutex_lock(&vc->slot_mutex);
    vc->size_known = 0;              /* the old measurement is stale */
    vc_FlushSlotLocked(vc);          /* whatever is queued is the old size */
    pthread_mutex_unlock(&vc->slot_mutex);

    /* Now find out whether that actually took, which means running the camera
       again and looking at what comes out.

       Accepting the preset is not the same as honouring it. Measured on a
       FaceTime HD camera: canSetSessionPreset agreed to 352x288, the preset
       read back as 352x288, and the session went on delivering 1920x1080. So
       without this the call returns success for a size the caller will never
       receive, which is precisely the failure the exact-match rule exists to
       avoid -- and the caller cannot tell, because GetImageSize repeats the
       size that was asked for.

       The cost is that a setter briefly runs the camera, LED and all, when it
       was not already running. That is worth it to be able to answer
       truthfully; the alternative is a confident wrong answer. */
    if (!vc_LearnDeliveredSize(vc))
      return 0;

    int delivered_width = vc->width, delivered_height = vc->height;

    if (was_live && !imVideoCaptureLive(vc, 1))
      return 0;

    if (delivered_width != width || delivered_height != height)
      return 0;   /* width/height now hold the truth, whatever it is */

    return 1;
  }
}

/* How long OneFrame waits for its frame, in milliseconds. */
#define VC_ONEFRAME_TIMEOUT 5000

/* How long OneFrame throws frames away before keeping one, in milliseconds.

   A camera does not produce a usable picture the instant it starts: auto
   exposure and white balance have to converge. Measured on a FaceTime HD
   camera in a normally lit room, taking the red channel's mean over the whole
   frame -- 0 to 255:

     frame  0-2    ~1     black
     frame  3      115    exposure has jumped
     frame  24     131    settled

   So the first three frames are useless and about the fourth is fine. Without
   this, OneFrame returns frame 0 every time and every picture it takes is
   black -- which is exactly what happened while this was being written, and it
   was mistaken for the room being dark.

   500ms is roughly fifteen frames at 30fps, past the jump and most of the way
   to settled. The reference does the same thing with Sleep(VC_CAMERADELAY) at
   im_capture_dx.cpp:894, though its 200 is admittedly a guess -- "this vary
   from camera to camera, so we use a reasonable value and hope it will work
   for all". This one is at least measured on a camera.

   Deliberately not applied to Live() or Frame(). A caller polling Frame in a
   loop, which is what a preview does, watches the picture converge by itself;
   charging every such caller half a second at startup to fix a problem only
   OneFrame has would be the wrong trade. */
#define VC_ONEFRAME_WARMUP 500

/* One BGRA frame into the layout the caller asked for.

   Two things the reference never had to do. AVFoundation delivers top-down
   while this API promises bottom-up ("orientation is always bottom up",
   im_capture.h:196), so source row y becomes destination row height-1-y. And
   CVPixelBufferGetBytesPerRow is padded to a 16- or 64-byte multiple on most
   cameras, so rows are reached through the stride rather than by assuming
   width*4 -- the reference assumes width*3 throughout because DirectShow gave
   it packed rows.

   32BGRA is B,G,R,A in memory, which is the same channel order the DirectShow
   backend consumes, so the assignments below line up with im_capture_dx.cpp
   one for one apart from skipping the alpha byte. */
/* Deliberately takes a plain buffer and a stride rather than a
   CVPixelBufferRef, and deliberately has external linkage: it is the only part
   of this backend whose correctness cannot be established from a camera. The
   two ways to get it wrong -- an image upside down, or red and blue exchanged
   -- both produce a perfectly plausible picture, and neither shows up at all
   in the uniform frame a camera returns in a dark room. test_capture.cpp
   declares it and feeds it synthetic buffers with known contents, which pins
   the flip, the stride handling, the channel order and the luma exactly, on
   every platform, with no hardware. Not part of the public API and not in
   im_capture.def. */
void imVideoCaptureConvertBGRA(const unsigned char* base, int stride,
                               unsigned char* data, int color_mode,
                               int width, int height)
{
  const int count  = width * height;
  const int space  = imColorModeSpace(color_mode);
  const int packed = imColorModeIsPacked(color_mode);

  if (!base || !data)
    return;

  for (int y = 0; y < height; y++)
  {
    const unsigned char* src = base + (size_t)y * stride;
    const int line = height - 1 - y;      /* bottom-up */

    if (space == IM_RGB)
    {
      if (packed)
      {
        unsigned char* dst = data + (size_t)line * width * 3;
        for (int x = 0; x < width; x++)
        {
          *(dst+2) = *src++;   /* B */
          *(dst+1) = *src++;   /* G */
          *(dst+0) = *src++;   /* R */
          src++;               /* A dropped: capture data has no alpha channel */
          dst += 3;
        }
      }
      else
      {
        /* Three consecutive planes, red first, as the reference lays them out
           (im_capture_dx.cpp:190-193). This is the path that actually gets
           used: both callers in the tree pass a bare colour space with no
           IM_PACKED bit. */
        unsigned char* red   = data               + (size_t)line * width;
        unsigned char* green = data + count       + (size_t)line * width;
        unsigned char* blue  = data + 2*(size_t)count + (size_t)line * width;
        for (int x = 0; x < width; x++)
        {
          *blue++  = *src++;
          *green++ = *src++;
          *red++   = *src++;
          src++;
        }
      }
    }
    else
    {
      /* Deviation from im_capture_dx.cpp:208-213, which copies the blue byte
         of each pixel into the gray plane. That is not luma -- it is a blue
         filter, and on a real scene it is dark and noisy. im.h:37 defines
         IM_GRAY as "shades of gray, luma", imConvertColorSpace uses the luma
         coefficients for the same conversion, and nothing can be relying on
         the old values because the DirectShow backend does not build against
         any current SDK and this library has never been built by this tree at
         all. So: real luma, and do not "fix" it back.

         Depth is 1, so packed and planar are the same layout here. */
      unsigned char* map = data + (size_t)line * width;
      for (int x = 0; x < width; x++)
      {
        map[x] = imColorRGB2Luma<unsigned char>(src[2], src[1], src[0]);
        src += 4;
      }
    }
  }
}

static void vc_ConvertFrame(CVPixelBufferRef buffer, unsigned char* data,
                            int color_mode, int width, int height)
{
  imVideoCaptureConvertBGRA(
    (const unsigned char*)CVPixelBufferGetBaseAddress(buffer),
    (int)CVPixelBufferGetBytesPerRow(buffer),
    data, color_mode, width, height);
}

int imVideoCaptureFrame(imVideoCapture* vc, unsigned char* data, int color_mode, int timeout)
{
  assert(vc);
  assert(vc->device != -1);
  assert(vc->live);
  if (!vc || vc->device == -1 || !vc->live || !data)
    return 0;

  pthread_mutex_lock(&vc->slot_mutex);

  if (timeout != 0)   /* 0 is a poll: never wait */
  {
    uint64_t deadline = 0;
    if (timeout > 0)
      deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC)
               + (uint64_t)timeout * 1000000ull;

    while (!vc->slot_full && !vc->stopping && !vc->failed)
    {
      if (timeout < 0)
      {
        /* "Forever" is served in one-second slices rather than as a single
           unbounded wait. A camera that is unplugged mid-wait posts no frame
           and, without a run loop, may post no notification either -- so an
           unbounded pthread_cond_wait here is a hang with no way out. Waking
           periodically lets the check below notice, and a caller who asked for
           an infinite timeout still gets one as long as the camera is there.

           Disconnect can also release us: it sets stopping and broadcasts,
           which is why that flag is in the predicate. */
        struct timespec ts;
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        int wait = pthread_cond_timedwait_relative_np(&vc->slot_cond,
                                                      &vc->slot_mutex, &ts);
        if (wait != 0 && wait != ETIMEDOUT)
          break;

        if (!vc->slot_full && !vc->stopping)
        {
          /* Dropped, because vc_CheckAlive calls into AVFoundation and nothing
             in this file does that under slot_mutex. */
          pthread_mutex_unlock(&vc->slot_mutex);
          int alive = vc_CheckAlive(vc);
          pthread_mutex_lock(&vc->slot_mutex);
          if (!alive)
            break;
        }
      }
      else
      {
        /* Recomputed each pass against a deadline fixed once, so a spurious
           wakeup shortens the wait rather than restarting the caller's clock. */
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        if (now >= deadline)
          break;

        uint64_t remaining = deadline - now;
        struct timespec ts;
        ts.tv_sec  = (time_t)(remaining / 1000000000ull);
        ts.tv_nsec = (long)  (remaining % 1000000000ull);

        if (pthread_cond_timedwait_relative_np(&vc->slot_cond,
                                               &vc->slot_mutex, &ts) != 0)
          break;   /* ETIMEDOUT */
      }
    }
  }

  CVPixelBufferRef buffer = NULL;
  int width = 0, height = 0;

  if (vc->slot_full)
  {
    buffer = vc->slot_buffer;   /* the retain moves to us */
    vc->slot_buffer = NULL;
    vc->slot_full = 0;
    width  = vc->width;
    height = vc->height;
  }

  pthread_mutex_unlock(&vc->slot_mutex);

  /* "Returns zero if failed or timeout expired, the buffer is not changed."
     That holds structurally here rather than by promise: data is not written
     until after a frame has been taken out of the slot, and every path that
     returns 0 does so before this point. */
  if (!buffer)
  {
    /* A wait that produced nothing is the moment to ask whether the camera is
       still there. Not done for a poll -- timeout 0 returning empty is the
       normal case, and two property reads per poll would be a real cost in a
       loop. */
    if (timeout != 0)
      vc_CheckAlive(vc);
    return 0;
  }

  int ret = 0;
  if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly)
      == kCVReturnSuccess)
  {
    vc_ConvertFrame(buffer, data, color_mode, width, height);
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    ret = 1;
  }

  CVPixelBufferRelease(buffer);
  return ret;
}

int imVideoCaptureOneFrame(imVideoCapture* vc, unsigned char* data, int color_mode)
{
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1 || !data)
    return 0;

  int was_live = vc->live;
  if (!was_live && !imVideoCaptureLive(vc, 1))
    return 0;

  /* Anything already in the slot predates this call; the point of OneFrame is
     to return a new frame. */
  pthread_mutex_lock(&vc->slot_mutex);
  vc_FlushSlotLocked(vc);
  pthread_mutex_unlock(&vc->slot_mutex);

  /* Then throw away everything the camera produces while it works out its
     exposure, and keep the frame after that. See VC_ONEFRAME_WARMUP. */
  if (!was_live)
  {
    uint64_t until = clock_gettime_nsec_np(CLOCK_MONOTONIC)
                   + (uint64_t)VC_ONEFRAME_WARMUP * 1000000ull;

    while (clock_gettime_nsec_np(CLOCK_MONOTONIC) < until)
    {
      pthread_mutex_lock(&vc->slot_mutex);

      if (!vc->slot_full && !vc->stopping)
      {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 50 * 1000000L;      /* 50ms: a frame and a half at 30fps */
        pthread_cond_timedwait_relative_np(&vc->slot_cond, &vc->slot_mutex, &ts);
      }

      vc_FlushSlotLocked(vc);
      int stopping = vc->stopping;

      pthread_mutex_unlock(&vc->slot_mutex);

      if (stopping)
        break;
    }
  }

  /* Deviation from im_capture_dx.cpp:917, which passes -1 and waits forever.
     A camera that is present but never delivers -- a stalled virtual camera, a
     Continuity link that dropped -- would hang the caller's process with no way
     out. OneFrame is documented only as "returns zero if failed", so a bounded
     wait is inside the contract. */
  int ret = imVideoCaptureFrame(vc, data, color_mode, VC_ONEFRAME_TIMEOUT);

  if (!was_live)
    imVideoCaptureLive(vc, 0);

  return ret;
}

/* FormatCount and GetFormat are implemented; SetFormat is not.

   Reading the list is what makes SetImageSize usable at all: im_capture.h:186
   names GetFormat as the way to discover valid sizes, so with both stubbed a
   caller could only guess. The list is candidates rather than guarantees --
   see the note on vc_PresetTable, and expect SetImageSize to refuse some of
   them, on some cameras all but one. Setting a format is a different matter -- the
   API's notion of a format is a size plus an encoding, this backend asks the
   driver for BGRA regardless, and so the only part of a format selection that
   would mean anything here is the size, which SetImageSize already does. */

int imVideoCaptureFormatCount(imVideoCapture* vc)
{
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1)
    return 0;

  return (int)vc->format_list.count;
}

int imVideoCaptureGetFormat(imVideoCapture* vc, int format, int *width, int *height, char* desc)
{
  assert(vc);
  assert(vc->device != -1);
  if (!vc || vc->device == -1 ||
      format < 0 || format >= (int)vc->format_list.count)
  {
    if (width)  *width = 0;
    if (height) *height = 0;
    return 0;
  }

  @autoreleasepool
  {
    int format_width, format_height;
    vc_PresetSize(vc->format_list[format], &format_width, &format_height);

    if (width)  *width  = format_width;
    if (height) *height = format_height;

    /* The reference writes the device's encoding here ("RGB24", or a four
       character code). Every frame this backend hands back is converted from
       BGRA whatever the camera's native encoding is, so reporting that
       encoding would describe something the caller never sees. The size is the
       whole of what a format means here, and desc says so. */
    if (desc)
      snprintf(desc, 10, "BGRA");

    return 1;
  }
}

int imVideoCaptureSetFormat(imVideoCapture* vc, int format)
{
  assert(vc);
  if (!vc)
    return format == -1? -1: 0;

  /* Same reasoning as Live: the query form is answerable without a
     connection, and the answer is -1. */
  if (format == -1)
  {
    if (vc->device == -1)
      return -1;

    /* Answered from the size in force rather than from a stored index, so it
       stays true after SetImageSize. */
    for (int i = 0; i < (int)vc->format_list.count; i++)
    {
      int format_width, format_height;
      vc_PresetSize(vc->format_list[i], &format_width, &format_height);
      if (format_width == vc->width && format_height == vc->height)
        return i;
    }
    return -1;   /* a size no preset names, e.g. whatever High resolved to */
  }

  assert(vc->device != -1);
  return 0;
}
