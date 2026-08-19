/* Tests for libim_capture.
 *
 * Split in two, because the two halves are testable to very different
 * degrees.
 *
 * The device list is safe to exercise anywhere. Enumerating cameras on macOS
 * needs no authorisation and raises no prompt -- measured, not assumed -- and
 * on every other platform the backend reports no devices at all. Either way
 * these cases run in CI.
 *
 * Nothing here may call imVideoCaptureConnect. On macOS, touching the camera
 * from a process with no NSCameraUsageDescription does not return an error:
 * TCC kills the process, SIGABRT, uncatchable. A test binary is exactly such a
 * process, so a case that connected would abort the whole suite rather than
 * fail itself. See the capture section of BUILDING.md.
 *
 * That leaves the pixel conversion, which is the part most worth testing and
 * the part a camera is worst at testing. Its two failure modes -- an image
 * upside down, or red and blue exchanged -- each produce a perfectly plausible
 * picture, and neither is visible at all in the uniform frame a camera returns
 * in a dark room. So it is exercised directly, on synthetic buffers whose
 * every byte is known, through imVideoCaptureConvertBGRA.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_capture.h>
#include <im_plus.h>

#include <algorithm>
#include <string.h>
#include <string>
#include <vector>


TEST_CASE("capture: the device list is safe to walk before anything else")
{
  /* Zero is a legitimate answer -- a CI runner has no camera, and every
     platform but macOS reports none by construction -- so the count is only
     required to be sane, not positive. */
  int count = imVideoCaptureDeviceCount();
  CHECK(count >= 0);

  SUBCASE("an index outside the list returns NULL rather than a stale string")
  {
    CHECK(imVideoCaptureDeviceDesc(-1) == NULL);
    CHECK(imVideoCaptureDeviceExDesc(-1) == NULL);
    CHECK(imVideoCaptureDevicePath(-1) == NULL);
    CHECK(imVideoCaptureDeviceVendorInfo(-1) == NULL);

    CHECK(imVideoCaptureDeviceDesc(count) == NULL);
    CHECK(imVideoCaptureDeviceDesc(count + 1000) == NULL);
  }

  SUBCASE("every listed device has a description")
  {
    /* The header says DeviceDesc "returns NULL only if it is an invalid
       device", so a valid index must produce something. The other three are
       documented as "may return NULL" and are not held to it. */
    for (int i = 0; i < count; i++)
    {
      CAPTURE(i);
      const char* desc = imVideoCaptureDeviceDesc(i);
      REQUIRE(desc != NULL);
      CHECK(strlen(desc) > 0);
    }
  }

  SUBCASE("reloading and releasing are both safe, and repeatable")
  {
    CHECK(imVideoCaptureReloadDevices() >= 0);
    CHECK(imVideoCaptureReloadDevices() >= 0);

    imVideoCaptureReleaseDevices();
    imVideoCaptureReleaseDevices();   /* twice: the second must not double free */

    /* Not asserted to be zero afterwards. This backend enumerates lazily, so
       asking for the count immediately rebuilds the list -- which is the
       point of the deviation from the DirectShow backend, where the count
       stays zero until something else happens to trigger enumeration. */
    CHECK(imVideoCaptureDeviceCount() >= 0);
  }
}

TEST_CASE("capture: a handle reports its state before it is connected")
{
  imVideoCapture* vc = imVideoCaptureCreate();

  /* NULL is the documented answer when there is no device, which is the
     normal case in CI. Nothing below runs then. */
  if (!vc)
  {
    MESSAGE("no capture device on this machine; handle cases skipped");
    return;
  }

  CHECK(imVideoCaptureConnect(vc, -1) == -1);   /* the query form: not connected */
  CHECK(imVideoCaptureLive(vc, -1) == 0);

  int width = -1, height = -1;
  imVideoCaptureGetImageSize(vc, &width, &height);
  CHECK(width == 0);      /* "width and height returns 0 if not connected" */
  CHECK(height == 0);

  /* Disconnecting something that was never connected is a no-op, not a
     crash -- the reference guards the same way. */
  imVideoCaptureDisconnect(vc);
  CHECK(imVideoCaptureConnect(vc, -1) == -1);

  imVideoCaptureDestroy(vc);
}


TEST_CASE("plus: the capture wrapper refuses an image it would overrun")
{
  /* im::VideoCapture is the reason these checks can exist at all: the C API
     takes a bare unsigned char* and cannot learn the colour space, the data
     type, or how much of it there is. The wrapper has an Image.

     Every case here runs on a handle that is not connected, so nothing touches
     the camera. That is enough, because a disconnected handle captures nothing
     and every image must therefore be refused -- which is exactly the
     behaviour a caller depends on for the guard to be worth anything. */
  im::VideoCapture capture;

  if (capture.Failed())
  {
    MESSAGE("no capture device on this machine; wrapper cases skipped");
    return;
  }

  /* Not connected, so GetImageSize is 0x0 and nothing can be captured into. */
  int width = -1, height = -1;
  capture.GetImageSize(width, height);
  CHECK(width == 0);
  CHECK(height == 0);

  SUBCASE("a well formed image is still refused while disconnected")
  {
    im::Image image(640, 480, IM_RGB, IM_BYTE);
    REQUIRE(!image.Failed());
    CHECK(capture.CaptureFrame(image, 0) == false);
    CHECK(capture.CaptureOneFrame(image) == false);
  }

}

TEST_CASE("plus: the capture wrapper knows which layouts frame data fits")
{
  /* Static, so it needs no device and runs everywhere -- which is the point.
     Asking a disconnected handle to CaptureFrame refuses everything on the
     size check alone, so a case that went through CaptureFrame could not tell
     whether this test worked at all. It is exactly the test that was broken:
     the three conditions were joined with && rather than ||, so an image
     failing only one of them passed, and an IM_RGB image of IM_FLOAT went
     straight through to a function that writes bytes. */

  SUBCASE("the two layouts frame data comes in are accepted")
  {
    im::Image rgb(64, 48, IM_RGB, IM_BYTE);
    im::Image gray(64, 48, IM_GRAY, IM_BYTE);
    REQUIRE(!rgb.Failed());
    REQUIRE(!gray.Failed());
    CHECK(im::VideoCapture::FrameLayoutSupported(rgb) == true);
    CHECK(im::VideoCapture::FrameLayoutSupported(gray) == true);
  }

  SUBCASE("a wrong data type is refused even with the right colour space")
  {
    im::Image floats(64, 48, IM_RGB, IM_FLOAT);
    im::Image shorts(64, 48, IM_GRAY, IM_USHORT);
    im::Image ints(64, 48, IM_RGB, IM_INT);
    REQUIRE(!floats.Failed());
    REQUIRE(!shorts.Failed());
    REQUIRE(!ints.Failed());
    CHECK(im::VideoCapture::FrameLayoutSupported(floats) == false);
    CHECK(im::VideoCapture::FrameLayoutSupported(shorts) == false);
    CHECK(im::VideoCapture::FrameLayoutSupported(ints) == false);
  }

  SUBCASE("a wrong colour space is refused even with the right data type")
  {
    im::Image map(64, 48, IM_MAP, IM_BYTE);
    im::Image cmyk(64, 48, IM_CMYK, IM_BYTE);
    im::Image binary(64, 48, IM_BINARY, IM_BYTE);
    REQUIRE(!map.Failed());
    REQUIRE(!cmyk.Failed());
    REQUIRE(!binary.Failed());
    CHECK(im::VideoCapture::FrameLayoutSupported(map) == false);
    CHECK(im::VideoCapture::FrameLayoutSupported(cmyk) == false);
    CHECK(im::VideoCapture::FrameLayoutSupported(binary) == false);
  }
}


#ifdef __APPLE__

/* Not in im_capture.h: this is internal to the macOS backend and is declared
   here, in the only thing that calls it from outside. A signature that drifted
   would fail to link, which is the check that keeps this honest. */
void imVideoCaptureConvertBGRA(const unsigned char* base, int stride,
                               unsigned char* data, int color_mode,
                               int width, int height);

namespace {

const int CW = 4;
const int CH = 3;

/* Deliberately wider than CW*4. AVFoundation pads rows to a 16- or 64-byte
   multiple, and the DirectShow backend this one is modelled on has no stride
   handling at all, so reading rows at width*4 is the mistake most likely to be
   inherited. The padding is poisoned so that reading it shows up as a value
   that cannot occur in the picture. */
const int CSTRIDE = CW * 4 + 12;
const unsigned char POISON = 0xEE;

/* Distinct per pixel AND per channel, so a transposition of any two of them
   changes the answer. Kept well clear of POISON. */
unsigned char blue_of (int x, int y) { return (unsigned char)(10 + y*10 + x); }
unsigned char green_of(int x, int y) { return (unsigned char)(60 + y*10 + x); }
unsigned char red_of  (int x, int y) { return (unsigned char)(110 + y*10 + x); }

std::vector<unsigned char> make_bgra()
{
  std::vector<unsigned char> source((size_t)CSTRIDE * CH, POISON);
  for (int y = 0; y < CH; y++)
  {
    unsigned char* row = &source[(size_t)y * CSTRIDE];
    for (int x = 0; x < CW; x++)
    {
      row[x*4 + 0] = blue_of(x, y);
      row[x*4 + 1] = green_of(x, y);
      row[x*4 + 2] = red_of(x, y);
      row[x*4 + 3] = 0x7F;            /* alpha, which must be dropped */
    }
  }
  return source;
}

} /* namespace */

TEST_CASE("capture: a frame is converted bottom-up, de-strided, in RGB order")
{
  std::vector<unsigned char> source = make_bgra();
  const int count = CW * CH;

  SUBCASE("packed RGB")
  {
    std::vector<unsigned char> out((size_t)count * 3, 0);
    imVideoCaptureConvertBGRA(&source[0], CSTRIDE, &out[0], IM_RGB|IM_PACKED, CW, CH);

    for (int y = 0; y < CH; y++)
    {
      for (int x = 0; x < CW; x++)
      {
        CAPTURE(x); CAPTURE(y);
        /* Source row y lands in destination row CH-1-y: the API promises
           bottom-up ("orientation is always bottom up", im_capture.h:196)
           while AVFoundation delivers top-down. */
        const unsigned char* pixel = &out[((size_t)(CH - 1 - y) * CW + x) * 3];
        CHECK((int)pixel[0] == (int)red_of(x, y));
        CHECK((int)pixel[1] == (int)green_of(x, y));
        CHECK((int)pixel[2] == (int)blue_of(x, y));
      }
    }

    /* The alpha byte and the row padding are both dropped rather than copied
       through. Three bytes per pixel times count is the whole output, so any
       leak of either would have to show as a value in it. */
    CHECK(std::find(out.begin(), out.end(), POISON) == out.end());
    CHECK(std::find(out.begin(), out.end(), 0x7F) == out.end());
  }

  SUBCASE("planar RGB")
  {
    /* The layout both in-tree callers actually get: test/glut_capture.c and
       the Lua binding each pass a bare colour space with no IM_PACKED bit. */
    std::vector<unsigned char> out((size_t)count * 3, 0);
    imVideoCaptureConvertBGRA(&source[0], CSTRIDE, &out[0], IM_RGB, CW, CH);

    const unsigned char* red   = &out[0];
    const unsigned char* green = &out[count];
    const unsigned char* blue  = &out[2 * count];

    for (int y = 0; y < CH; y++)
    {
      for (int x = 0; x < CW; x++)
      {
        CAPTURE(x); CAPTURE(y);
        size_t at = (size_t)(CH - 1 - y) * CW + x;
        CHECK((int)red[at]   == (int)red_of(x, y));
        CHECK((int)green[at] == (int)green_of(x, y));
        CHECK((int)blue[at]  == (int)blue_of(x, y));
      }
    }
  }

  SUBCASE("gray is luma, not one of the channels")
  {
    /* The reference copies the blue byte (im_capture_dx.cpp:208), which is a
       blue filter rather than luma. The values here are chosen so the two are
       far apart: asserting the luma also asserts it is not blue, and not red
       or green either. */
    std::vector<unsigned char> out((size_t)count, 0);
    imVideoCaptureConvertBGRA(&source[0], CSTRIDE, &out[0], IM_GRAY, CW, CH);

    for (int y = 0; y < CH; y++)
    {
      for (int x = 0; x < CW; x++)
      {
        CAPTURE(x); CAPTURE(y);
        int r = red_of(x, y), g = green_of(x, y), b = blue_of(x, y);
        int expected = (299*r + 587*g + 114*b) / 1000;

        int got = (int)out[(size_t)(CH - 1 - y) * CW + x];
        CHECK(got == expected);
        CHECK(got != b);      /* the defect being avoided */
        CHECK(got != r);
        CHECK(got != g);
      }
    }
  }

  SUBCASE("a NULL destination or source is refused rather than dereferenced")
  {
    std::vector<unsigned char> out((size_t)count * 3, 0x11);
    imVideoCaptureConvertBGRA(NULL, CSTRIDE, &out[0], IM_RGB, CW, CH);
    imVideoCaptureConvertBGRA(&source[0], CSTRIDE, NULL, IM_RGB, CW, CH);
    for (size_t i = 0; i < out.size(); i++)
      REQUIRE((int)out[i] == 0x11);
  }
}


/* Not in im_capture.h: internal to the macOS backend, declared here in the only
   thing that calls them from outside, so a signature drift is a link error. */
int imVideoCaptureValue2Percent(double minimum, double maximum,
                                double value, double* percent);
int imVideoCapturePercent2Value(double minimum, double maximum,
                                double percent, double* value);

TEST_CASE("capture: attribute percentages are a position within the device's range")
{
  /* The conversions IM's attribute API is built on: the device reports a range
     and a value, and the caller only ever sees a percentage. Exercised
     directly, because on a machine whose camera exposes no controls -- which is
     every built-in Mac camera measured so far, and every CI runner -- there is
     otherwise nothing here to test at all. */

  double percent = -1, value = -1;

  SUBCASE("the ends and the middle")
  {
    REQUIRE(imVideoCaptureValue2Percent(0, 255, 0, &percent) != 0);
    CHECK(percent == doctest::Approx(0.0));
    REQUIRE(imVideoCaptureValue2Percent(0, 255, 255, &percent) != 0);
    CHECK(percent == doctest::Approx(100.0));
    REQUIRE(imVideoCaptureValue2Percent(0, 255, 127.5, &percent) != 0);
    CHECK(percent == doctest::Approx(50.0));

    /* A range that does not start at zero, which is the common case: a BRIO
       reports exposure over 3..2047 and zoom over 100..500. */
    REQUIRE(imVideoCaptureValue2Percent(100, 500, 100, &percent) != 0);
    CHECK(percent == doctest::Approx(0.0));
    REQUIRE(imVideoCaptureValue2Percent(100, 500, 300, &percent) != 0);
    CHECK(percent == doctest::Approx(50.0));
  }

  SUBCASE("and back again")
  {
    const double ranges[][2] = { {0,255}, {3,2047}, {100,500}, {0,1}, {-180,180} };
    for (int r = 0; r < 5; r++)
    {
      for (double want = 0; want <= 100; want += 12.5)
      {
        CAPTURE(r); CAPTURE(want);
        REQUIRE(imVideoCapturePercent2Value(ranges[r][0], ranges[r][1], want, &value) != 0);
        CHECK(value >= ranges[r][0]);
        CHECK(value <= ranges[r][1]);
        REQUIRE(imVideoCaptureValue2Percent(ranges[r][0], ranges[r][1], value, &percent) != 0);
        CHECK(percent == doctest::Approx(want));
      }
    }
  }

  SUBCASE("a negative range works, because one of them is")
  {
    /* CameraRollAngle is documented in degrees and CoreMediaIO's roll control
       runs -180..180, so the arithmetic must not assume a positive floor. */
    REQUIRE(imVideoCaptureValue2Percent(-180, 180, -180, &percent) != 0);
    CHECK(percent == doctest::Approx(0.0));
    REQUIRE(imVideoCaptureValue2Percent(-180, 180, 0, &percent) != 0);
    CHECK(percent == doctest::Approx(50.0));
  }

  SUBCASE("a degenerate range is refused rather than dividing by zero")
  {
    /* Not hypothetical: a Logitech BRIO reports 0..0 for its temperature and
       power-line-frequency controls. Without this the caller gets a NaN
       through a double*, and a NaN percentage is worse than a refusal because
       it compares false against everything and propagates. */
    percent = -1;
    CHECK(imVideoCaptureValue2Percent(0, 0, 0, &percent) == 0);
    CHECK(percent == 0);
    CHECK(imVideoCaptureValue2Percent(5, 5, 5, &percent) == 0);
    CHECK(imVideoCaptureValue2Percent(10, 3, 5, &percent) == 0);   /* inverted */

    value = -1;
    CHECK(imVideoCapturePercent2Value(0, 0, 50, &value) == 0);
    CHECK(value == 0);
  }

  SUBCASE("a NaN bound is refused too")
  {
    /* A BRIO's pan-tilt-absolute control reports a NativeRange of
       [nan .. 7.6e-310] -- it carries NativeData rather than a NativeValue and
       the range field is meaningless. Every comparison against NaN is false,
       so the guard is written as a negation to catch it. */
    const double nan_value = 0.0 / 0.0;
    CHECK(imVideoCaptureValue2Percent(nan_value, 100, 50, &percent) == 0);
    CHECK(imVideoCaptureValue2Percent(0, nan_value, 50, &percent) == 0);
    CHECK(imVideoCapturePercent2Value(nan_value, 100, 50, &value) == 0);
  }

  SUBCASE("out of range is clamped, not extrapolated")
  {
    /* The reference passes a percentage straight through the arithmetic and
       lets the driver deal with whatever comes out (im_capture_dx.cpp:1822). */
    REQUIRE(imVideoCapturePercent2Value(0, 255, 150, &value) != 0);
    CHECK(value == doctest::Approx(255.0));
    REQUIRE(imVideoCapturePercent2Value(0, 255, -50, &value) != 0);
    CHECK(value == doctest::Approx(0.0));

    REQUIRE(imVideoCaptureValue2Percent(0, 255, 1000, &percent) != 0);
    CHECK(percent == doctest::Approx(100.0));
    REQUIRE(imVideoCaptureValue2Percent(0, 255, -1000, &percent) != 0);
    CHECK(percent == doctest::Approx(0.0));
  }

  SUBCASE("a NULL destination is refused rather than dereferenced")
  {
    CHECK(imVideoCaptureValue2Percent(0, 255, 128, NULL) == 0);
    CHECK(imVideoCapturePercent2Value(0, 255, 50, NULL) == 0);
  }
}

#ifdef NDEBUG
/* Behind NDEBUG, per the convention in test_datatype.cpp: every one of these
   calls deliberately breaks a documented precondition -- an attribute belongs
   to a device, so all four functions assert a connection, exactly as the
   reference does. The assert is what a debug build wants; the guard beside it
   is what a shipped build needs, and this proves the guard.

   Not the same case as imVideoCaptureLive(vc,-1), which answers without a
   connection: "not live" is a true statement about a handle that is not
   connected, whereas "the brightness of no device" is not a question. */
TEST_CASE("capture: attributes are refused on a handle that is not connected")
{
  imVideoCapture* vc = imVideoCaptureCreate();
  if (!vc)
  {
    MESSAGE("no capture device on this machine; attribute cases skipped");
    return;
  }

  /* Nothing here connects -- see the header comment. A disconnected handle has
     resolved no CoreMediaIO device, so every attribute call must refuse. */
  int count = -1;
  CHECK(imVideoCaptureGetAttributeList(vc, &count) == NULL);
  CHECK(count == 0);

  double percent = -1;
  CHECK(imVideoCaptureGetAttribute(vc, "VideoBrightness", &percent) == 0);
  CHECK(percent == 0);            /* written before anything can fail */

  CHECK(imVideoCaptureSetAttribute(vc, "VideoBrightness", 50.0) == 0);
  CHECK(imVideoCaptureResetAttribute(vc, "VideoBrightness", 1) == 0);

  SUBCASE("and a name that is not one of the twenty is refused too")
  {
    CHECK(imVideoCaptureGetAttribute(vc, "NoSuchAttribute", &percent) == 0);
    CHECK(imVideoCaptureGetAttribute(vc, "", &percent) == 0);
    CHECK(imVideoCaptureGetAttribute(vc, NULL, &percent) == 0);
    CHECK(imVideoCaptureSetAttribute(vc, NULL, 50.0) == 0);
  }

  imVideoCaptureDestroy(vc);
}
#endif /* NDEBUG */

#endif /* __APPLE__ */
