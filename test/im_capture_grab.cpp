/* Captures one frame and writes it to an image file.
 *
 * Not registered with CTest and not an assertion of anything: it is the tool
 * for the checks a test cannot make. The pixel conversion is pinned by
 * test_capture.cpp on synthetic frames, but whether a real camera's output
 * ends up the right way up and the right colour is a thing you confirm by
 * looking at the picture.
 *
 * On macOS this is built as an app bundle, and that is not cosmetic. A process
 * that touches the camera without an NSCameraUsageDescription is killed by TCC
 * -- SIGABRT, uncatchable, from inside the first call that reaches the device
 * -- and a bundle is what lets the key be declared and attributed to this
 * program rather than to whichever terminal launched it. Run it through
 * LaunchServices so the attribution lands here:
 *
 *   open -W --stdout /dev/stdout \
 *     -a build/lib/im_capture_grab.app --args 0 /tmp/frame.png
 *
 * Running the executable inside the bundle directly does NOT work: the
 * responsible process is then the shell's ancestor, which declares no camera
 * usage. See the capture section of BUILDING.md.
 */

#include <im.h>
#include <im_image.h>
#include <im_capture.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prints every documented attribute and what the device says about it, then
   returns. Separate mode because it is the only way to see the answer on real
   hardware: on this machine a USB camera reports nine of the twenty and the
   built-in camera reports none. */
static int show_attributes(imVideoCapture* vc)
{
  static const char* names[] = {
    "VideoBrightness", "VideoContrast", "VideoHue", "VideoSaturation",
    "VideoSharpness", "VideoGamma", "VideoColorEnable", "VideoWhiteBalance",
    "VideoBacklightCompensation", "VideoGain", "CameraPanAngle",
    "CameraTiltAngle", "CameraRollAngle", "CameraLensZoom", "CameraExposure",
    "CameraIris", "CameraFocus", "FlipHorizontal", "FlipVertical",
    "AnalogFormat"
  };

  int count = 0;
  const char** supported = imVideoCaptureGetAttributeList(vc, &count);
  printf("\nthis device reports %d of the 20 documented attributes\n", count);
  for (int i = 0; i < count && supported; i++)
    printf("  %s\n", supported[i]);

  printf("\n");
  for (int i = 0; i < 20; i++)
  {
    double percent = 0;
    if (imVideoCaptureGetAttribute(vc, names[i], &percent))
      printf("  %-28s %6.1f%%\n", names[i], percent);
    else
      printf("  %-28s unsupported\n", names[i]);
  }
  return 0;
}

/* Captures in a loop and reports when the camera goes away. Run it, then
   unplug the camera: Frame starts returning 0, Live(-1) turns 0, and one line
   goes to stderr explaining what happened and how to recover. */
static int watch(imVideoCapture* vc)
{
  int width = 0, height = 0;
  if (!imVideoCaptureLive(vc, 1))
    return 1;
  imVideoCaptureGetImageSize(vc, &width, &height);

  imImage* image = imImageCreate(width, height, IM_RGB, IM_BYTE);
  if (!image)
    return 1;

  printf("capturing %dx%d -- unplug the camera to see what happens\n", width, height);
  for (int i = 0; i < 120; i++)
  {
    int got = imVideoCaptureFrame(vc, (unsigned char*)image->data[0], IM_RGB, 1000);
    int live = imVideoCaptureLive(vc, -1);
    printf("  frame %3d: Frame=%d Live(-1)=%d\n", i, got, live);
    fflush(stdout);
    if (!live)
    {
      printf("  the camera is gone; stopping\n");
      break;
    }
  }

  imImageDestroy(image);
  return 0;
}

int main(int argc, char** argv)
{
  int device = (argc > 1)? atoi(argv[1]): 0;
  const char* path = (argc > 2)? argv[2]: "capture.png";
  int as_gray = (argc > 3 && strcmp(argv[3], "gray") == 0);
  int mode_attrs = (argc > 2 && strcmp(argv[2], "attrs") == 0);
  int mode_watch = (argc > 2 && strcmp(argv[2], "watch") == 0);

  int count = imVideoCaptureDeviceCount();
  printf("%d capture device(s):\n", count);
  for (int i = 0; i < count; i++)
  {
    const char* vendor = imVideoCaptureDeviceVendorInfo(i);
    printf("  %-40s %s\n", imVideoCaptureDeviceDesc(i),
           (vendor && *vendor)? vendor: "");
  }
  if (count == 0)
    return 1;

  imVideoCapture* vc = imVideoCaptureCreate();
  if (!vc)
  {
    fprintf(stderr, "imVideoCaptureCreate failed\n");
    return 1;
  }

  if (!imVideoCaptureConnect(vc, device))
  {
    /* The likeliest cause by far, and the API has no way to distinguish it
       from any other failure, so say both. */
    fprintf(stderr, "could not connect to device %d -- if the camera "
                    "permission prompt has not been answered, or was denied, "
                    "this is what that looks like\n", device);
    imVideoCaptureDestroy(vc);
    return 1;
  }

  int width = 0, height = 0;
  imVideoCaptureGetImageSize(vc, &width, &height);
  printf("\nconnected to device %d\n", device);

  if (mode_attrs)
  {
    int result = show_attributes(vc);
    imVideoCaptureDestroy(vc);
    return result;
  }

  if (mode_watch)
  {
    int result = watch(vc);
    imVideoCaptureDestroy(vc);
    return result;
  }

  int formats = imVideoCaptureFormatCount(vc);
  if (formats > 0)
  {
    printf("settable sizes:");
    for (int i = 0; i < formats; i++)
    {
      int format_width = 0, format_height = 0;
      if (imVideoCaptureGetFormat(vc, i, &format_width, &format_height, NULL))
        printf(" %dx%d", format_width, format_height);
    }
    printf("\n");
  }

  /* Start briefly to learn the size the camera will actually deliver: that is
     only settled once the session is running. */
  if (!imVideoCaptureLive(vc, 1))
  {
    fprintf(stderr, "could not start the capture\n");
    imVideoCaptureDestroy(vc);
    return 1;
  }
  imVideoCaptureGetImageSize(vc, &width, &height);
  imVideoCaptureLive(vc, 0);

  printf("capturing %d x %d %s\n", width, height, as_gray? "gray": "rgb");

  imImage* image = imImageCreate(width, height,
                                 as_gray? IM_GRAY: IM_RGB, IM_BYTE);
  if (!image)
  {
    imVideoCaptureDestroy(vc);
    return 1;
  }

  /* OneFrame rather than Live plus Frame, for the warm-up: a camera's first
     frames are black while its exposure converges, so grabbing the first thing
     that arrives produces a picture that looks like a bug in this library
     rather than a camera that has not woken up yet. */
  if (!imVideoCaptureOneFrame(vc, (unsigned char*)image->data[0],
                              image->color_space))
  {
    fprintf(stderr, "no frame arrived\n");
    imImageDestroy(image);
    imVideoCaptureDestroy(vc);
    return 1;
  }

  int error = imFileImageSave(path, "PNG", image);
  if (error != IM_ERR_NONE)
    fprintf(stderr, "could not write %s (error %d)\n", path, error);
  else
    printf("wrote %s\n", path);

  imVideoCaptureDisconnect(vc);
  imVideoCaptureDestroy(vc);
  imImageDestroy(image);
  return error == IM_ERR_NONE? 0: 1;
}
