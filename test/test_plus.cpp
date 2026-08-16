/* Tests for the C++ wrapper API in include/im_plus.h.
 *
 * 1,067 lines and 396 functions, all header-only inline, and nothing in the
 * tree instantiated a single one of them -- so nothing checked that they
 * compile, that they link, or that they forward their arguments the right way
 * round. All three are real failure modes here and one of each has already
 * bitten this repository: imAttribTableMergeFrom was missing from the Windows
 * .def file precisely because this header is its only caller, and the
 * Image(file_name, index, error, as_bitmap) constructor had its two branches
 * inverted.
 *
 * So the point of this file is breadth first. Every wrapper it touches is
 * checked against the C function it is supposed to be calling, because a
 * wrapper that swaps two arguments or picks the wrong overload produces a
 * plausible answer rather than a crash, and comparing against the C API is
 * the only way to see it.
 *
 * VideoCapture and VideoCaptureDevice are deliberately absent: they wrap
 * libim_capture, which this build does not produce, and referencing them
 * would fail to link rather than fail a test.
 */

#include "doctest/doctest.h"

#include <im_plus.h>

/* im_plus.h pulls in most of the C headers but not im_util.h, so imbyte and
   imColorEncode are not in scope through it alone. The wrapper offers
   Palette::ColorEncode for the latter; the tests below compare against the C
   entry points directly, which needs the header. */
#include <im_util.h>

#include <string.h>
#include <string>

namespace {

const int W = 6;
const int H = 4;
const int N = W * H;

std::string scratch(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

void fill(im::Image& image)
{
  for (int p = 0; p < image.Depth(); p++)
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        image.SetValue(p, y, x, (double)((y * W + x) * 3 + p * 7));
}

} /* namespace */


TEST_CASE("plus: the version wrappers return what the C functions do")
{
  CHECK(strcmp(im::Version(), imVersion()) == 0);
  CHECK(strcmp(im::VersionDate(), imVersionDate()) == 0);
  CHECK(im::VersionNumber() == imVersionNumber());
}


/* ================================================================== *
 * Palette
 * ================================================================== */

TEST_CASE("plus: Palette owns its entries and copies rather than shares")
{
  im::Palette palette(16);
  REQUIRE(palette.GetData() != NULL);
  CHECK(palette.Count() == 16);

  for (int i = 0; i < 16; i++)
    palette.GetData()[i] = imColorEncode((imbyte)(i * 3), (imbyte)i, (imbyte)(255 - i));

  /* The copy constructor duplicates, so writing through one must not reach
     the other -- and the destructor of each must then not double free, which
     is what the sanitizer build is checking here. */
  im::Palette copy(palette);
  CHECK(copy.Count() == 16);
  CHECK(copy.GetData() != palette.GetData());
  CHECK(copy.GetData()[5] == palette.GetData()[5]);

  copy.GetData()[5] = imColorEncode(1, 2, 3);
  CHECK(copy.GetData()[5] != palette.GetData()[5]);
}

TEST_CASE("plus: Palette lookups agree with the C ones")
{
  im::Palette palette(4);
  palette.GetData()[0] = imColorEncode(0, 0, 0);
  palette.GetData()[1] = imColorEncode(255, 255, 255);
  palette.GetData()[2] = imColorEncode(255, 0, 0);
  palette.GetData()[3] = imColorEncode(0, 255, 0);

  long wanted = imColorEncode(250, 5, 5);

  CHECK(palette.FindNearest(wanted) ==
        imPaletteFindNearest(palette.GetData(), 4, wanted));
  CHECK(palette.FindColor(imColorEncode(255, 0, 0), 0) ==
        imPaletteFindColor(palette.GetData(), 4, imColorEncode(255, 0, 0), 0));
}


/* ================================================================== *
 * Image
 * ================================================================== */

TEST_CASE("plus: Image reports the geometry it was built with")
{
  im::Image image(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!image.Failed());

  CHECK(image.Width() == W);
  CHECK(image.Height() == H);
  CHECK(image.ColorSpace() == IM_RGB);
  CHECK(image.DataType() == IM_BYTE);
  CHECK(image.Depth() == 3);
  CHECK(image.HasAlpha() == false);

  /* And the handle is the same object the C API would have made. */
  imImage* handle = image.GetHandle();
  REQUIRE(handle != NULL);
  CHECK(handle->width == W);
  CHECK(handle->depth == 3);
}

TEST_CASE("plus: SetValue and GetValue address the right sample")
{
  im::Image image(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!image.Failed());
  fill(image);

  /* Compared against the raw buffer, because a wrapper that transposed its
     line and column arguments would still be self-consistent. */
  for (int p = 0; p < 3; p++)
  {
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        CAPTURE(p); CAPTURE(x); CAPTURE(y);
        double expected = (double)((y * W + x) * 3 + p * 7);
        CHECK(image.GetValue(p, y, x) == expected);
        CHECK((double)((imbyte**)image.GetHandle()->data)[p][y * W + x] == expected);
      }
    }
  }

  SUBCASE("and out-of-range access is refused rather than dereferenced")
  {
    CHECK(image.GetValue(-1, 0, 0) == 0);
    CHECK(image.GetValue(3, 0, 0) == 0);
    CHECK(image.GetValue(0, -1, 0) == 0);
    CHECK(image.GetValue(0, H, 0) == 0);
    CHECK(image.GetValue(0, 0, W) == 0);

    /* Writes out of range must be dropped, not clamped onto a real sample. */
    double keep = image.GetValue(0, 0, 0);
    image.SetValue(0, -1, 0, 99);
    image.SetValue(0, 0, W, 99);
    image.SetValue(9, 0, 0, 99);
    CHECK(image.GetValue(0, 0, 0) == keep);
  }
}

TEST_CASE("plus: the channel subscript reaches the same samples")
{
  im::Image image(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!image.Failed());
  fill(image);

  /* image[plane][line][column], per the comment on the operator. */
  for (int p = 0; p < 3; p++)
  {
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        CAPTURE(p); CAPTURE(x); CAPTURE(y);
        CHECK((double)image[p][y][x] == image.GetValue(p, y, x));
      }
    }
  }
}

TEST_CASE("plus: alpha is added and removed through the wrapper")
{
  im::Image image(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!image.Failed());
  CHECK(image.HasAlpha() == false);

  image.AddAlpha();
  CHECK(image.HasAlpha() == true);
  CHECK(image.GetHandle()->has_alpha != 0);

  image.SetAlpha(128);
  CHECK((int)((imbyte**)image.GetHandle()->data)[image.Depth()][0] == 128);

  image.RemoveAlpha();
  CHECK(image.HasAlpha() == false);
}

TEST_CASE("plus: copying and duplicating produce equal images")
{
  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  SUBCASE("Duplicate makes an independent image")
  {
    im::Image copy = source.Duplicate();
    REQUIRE(!copy.Failed());
    CHECK(copy.GetHandle() != source.GetHandle());
    CHECK(source.Match(copy));

    /* The wrapper counts references in an image attribute, and everything
       that copies attributes into a fresh image copies the count too -- so a
       duplicate used to start at 1, be incremented to 2 by its new wrapper,
       and never reach zero again. The image simply leaked.
       *
       * Asserted on the count rather than by watching the allocator, because
       * LeakSanitizer only exists on the Linux jobs; this fails everywhere. */
    CHECK(copy.GetAttribInteger("_IMAGE_REF") == 1);

    for (int i = 0; i < N; i++)
      CHECK(copy.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));

    /* Independent, so a write does not reach back. */
    copy.SetValue(0, 0, 0, 200);
    CHECK(source.GetValue(0, 0, 0) != 200);
  }

  SUBCASE("Copy fills an image the caller already has")
  {
    im::Image target(W, H, IM_RGB, IM_BYTE);
    source.Copy(target);
    for (int i = 0; i < N; i++)
      CHECK(target.GetValue(1, i / W, i % W) == source.GetValue(1, i / W, i % W));
  }

  SUBCASE("CopyPlane takes one plane to another")
  {
    im::Image target(W, H, IM_RGB, IM_BYTE);
    target.Clear();
    source.CopyPlane(target, 2, 0);

    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(target.GetValue(0, i / W, i % W) == source.GetValue(2, i / W, i % W));
      CHECK(target.GetValue(1, i / W, i % W) == 0);
    }
  }
}

TEST_CASE("plus: an image based on another starts with its own reference count")
{
  /* Same defect as Duplicate, by the other route: imImageCreateBased calls
     imImageCopyAttributes, so this constructor inherited the source's count
     as well. */
  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  CHECK(source.GetAttribInteger("_IMAGE_REF") == 1);

  im::Image based(source, W * 2, H, IM_GRAY, IM_BYTE);
  REQUIRE(!based.Failed());
  CHECK(based.Width() == W * 2);
  CHECK(based.ColorSpace() == IM_GRAY);
  CHECK(based.GetAttribInteger("_IMAGE_REF") == 1);
}

TEST_CASE("plus: the reference count keeps a shared image alive")
{
  /* Image copies share one imImage and count references in an attribute, so
     the destructor of a copy must not free what the original still holds.
     Getting this wrong is a use-after-free rather than a wrong answer, which
     is why the case exists at all. */
  im::Image original(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!original.Failed());
  original.SetValue(0, 0, 0, 42);

  {
    im::Image shared(original);
    CHECK(shared.GetHandle() == original.GetHandle());
    CHECK(shared.GetValue(0, 0, 0) == 42);
  }   /* shared dies here; original must survive it */

  CHECK(original.GetValue(0, 0, 0) == 42);
  CHECK(original.Width() == W);
}

TEST_CASE("plus: the comparison family matches the C predicates")
{
  im::Image a(W, H, IM_RGB, IM_BYTE);
  im::Image same(W, H, IM_RGB, IM_BYTE);
  im::Image other_size(W + 1, H, IM_RGB, IM_BYTE);
  im::Image other_space(W, H, IM_GRAY, IM_BYTE);
  im::Image other_type(W, H, IM_RGB, IM_USHORT);

  CHECK(a.Match(same));
  CHECK(a == same);
  CHECK(a.MatchSize(same));
  CHECK(a.MatchColorSpace(same));
  CHECK(a.MatchDataType(same));

  CHECK(!a.MatchSize(other_size));
  CHECK(!a.MatchColorSpace(other_space));
  CHECK(!a.MatchDataType(other_type));
  CHECK(!a.Match(other_type));

  /* Each wrapper against the C predicate it claims to be. */
  CHECK(a.Match(same) == (imImageMatch(a.GetHandle(), same.GetHandle()) == 1));
  CHECK(a.MatchColor(same) == (imImageMatchColor(a.GetHandle(), same.GetHandle()) == 1));
}

TEST_CASE("plus: attributes round-trip through the image wrapper")
{
  im::Image image(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!image.Failed());

  image.SetAttribInteger("Answer", IM_INT, 42);
  image.SetAttribReal("Ratio", IM_DOUBLE, 1.5);
  image.SetAttribString("Title", "a caption");

  CHECK(image.GetAttribInteger("Answer") == 42);
  CHECK(image.GetAttribReal("Ratio") == doctest::Approx(1.5));
  CHECK(strcmp(image.GetAttribString("Title"), "a caption") == 0);

  /* And the same values are visible through the C API on the same handle,
     which is what proves the wrapper is not keeping its own copy. */
  CHECK(imImageGetAttribInteger(image.GetHandle(), "Answer", 0) == 42);
  CHECK(strcmp(imImageGetAttribString(image.GetHandle(), "Title"), "a caption") == 0);
}

TEST_CASE("plus: the palette wrapper survives a trip through an image")
{
  im::Image image(W, H, IM_MAP, IM_BYTE);
  REQUIRE(!image.Failed());

  im::Palette palette(4);
  palette.GetData()[0] = imColorEncode(10, 20, 30);
  palette.GetData()[1] = imColorEncode(40, 50, 60);
  palette.GetData()[2] = imColorEncode(70, 80, 90);
  palette.GetData()[3] = imColorEncode(100, 110, 120);

  image.SetPalette(palette);

  im::Palette back = image.GetPalette();
  CHECK(back.Count() == 4);
  for (int i = 0; i < 4; i++)
  {
    CAPTURE(i);
    CHECK(back.GetData()[i] == palette.GetData()[i]);
  }

  /* SetPalette duplicates, so the image does not alias the caller's copy. */
  CHECK(image.GetHandle()->palette != palette.GetData());
}

TEST_CASE("plus: conversion wrappers forward to the C conversions")
{
  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  SUBCASE("colour space")
  {
    im::Image gray(W, H, IM_GRAY, IM_BYTE);
    CHECK(source.ConvertColorSpace(gray) == IM_ERR_NONE);

    imImage* reference = imImageCreate(W, H, IM_GRAY, IM_BYTE);
    REQUIRE(reference != NULL);
    REQUIRE(imConvertColorSpace(source.GetHandle(), reference) == IM_ERR_NONE);

    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK((int)gray.GetValue(0, i / W, i % W) ==
            (int)((imbyte*)reference->data[0])[i]);
    }
    imImageDestroy(reference);
  }

  SUBCASE("data type")
  {
    im::Image wide(W, H, IM_RGB, IM_INT);
    CHECK(source.ConvertDataType(wide, IM_CPX_REAL, 0, false, IM_CAST_MINMAX) == IM_ERR_NONE);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(wide.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));
    }
  }
}


/* ================================================================== *
 * File
 * ================================================================== */

TEST_CASE("plus: File writes and reads an image back")
{
  std::string path = scratch("plus_roundtrip.png");

  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  CHECK(source.Save(path.c_str(), "PNG") == IM_ERR_NONE);

  int error = IM_ERR_NONE;
  im::File file(path.c_str(), error);
  REQUIRE_MESSAGE(!file.Failed(), "open failed, error " << error);

  char format[32] = { 0 };
  char compression[32] = { 0 };
  int image_count = 0;
  file.GetInfo(format, compression, image_count);
  CHECK(strcmp(format, "PNG") == 0);
  CHECK(image_count >= 1);

  im::Image loaded = file.LoadImage(0, error);
  REQUIRE_MESSAGE(!loaded.Failed(), "load failed, error " << error);
  CHECK(loaded.Width() == W);
  CHECK(loaded.Height() == H);

  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK(loaded.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));
  }
}

TEST_CASE("plus: the as_bitmap flag selects the loader it is named for")
{
  /* This constructor had its branches inverted -- as_bitmap chose
     imFileImageLoad and clearing it chose imFileImageLoadBitmap. Nothing
     instantiated it, so nothing noticed.
     *
     * A 16 bit source is what makes the two distinguishable: loading as a
     * bitmap demotes to IM_BYTE, loading normally keeps IM_USHORT. With an
     * 8 bit source both paths return the same thing and the inversion is
     * invisible, which is the trap. */
  std::string path = scratch("plus_bitmap.png");

  im::Image source(W, H, IM_GRAY, IM_USHORT);
  REQUIRE(!source.Failed());
  for (int i = 0; i < N; i++)
    source.SetValue(0, i / W, i % W, (double)(i * 2000));
  REQUIRE(source.Save(path.c_str(), "PNG") == IM_ERR_NONE);

  int error = IM_ERR_NONE;
  im::Image plain(path.c_str(), 0, error, false);
  REQUIRE_MESSAGE(!plain.Failed(), "plain load failed, error " << error);
  CHECK(plain.DataType() == IM_USHORT);

  error = IM_ERR_NONE;
  im::Image bitmap(path.c_str(), 0, error, true);
  REQUIRE_MESSAGE(!bitmap.Failed(), "bitmap load failed, error " << error);
  CHECK(bitmap.DataType() == IM_BYTE);
  CHECK(bitmap.IsBitmap());
}

TEST_CASE("plus: File attributes reach the underlying file")
{
  std::string path = scratch("plus_attrib.jpg");

  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  int error = IM_ERR_NONE;
  {
    im::File out(path.c_str(), "JPEG", error);
    REQUIRE_MESSAGE(!out.Failed(), "create failed, error " << error);

    out.SetAttribInteger("JPEGQuality", IM_INT, 70);
    CHECK(out.GetAttribInteger("JPEGQuality", 0) == 70);

    CHECK(out.SaveImage(source) == IM_ERR_NONE);
  }   /* the destructor closes it, which is the only way it gets flushed */

  im::File in(path.c_str(), error);
  CHECK(!in.Failed());
}


/* ================================================================== *
 * The free-function namespaces
 * ================================================================== */

TEST_CASE("plus: the Format namespace forwards to the C registry")
{
  char desc[128] = { 0 };
  char ext[128] = { 0 };
  int can_sequence = 0;

  CHECK(im::Format::Info("PNG", desc, ext, can_sequence) == IM_ERR_NONE);
  CHECK(strlen(desc) > 0);
  CHECK(strlen(ext) > 0);

  CHECK(im::Format::CanWriteImage("PNG", NULL, IM_RGB, IM_BYTE) ==
        imFormatCanWriteImage("PNG", NULL, IM_RGB, IM_BYTE));
  CHECK(im::Format::CanWriteImage("GIF", NULL, IM_RGB, IM_USHORT) ==
        imFormatCanWriteImage("GIF", NULL, IM_RGB, IM_USHORT));

  /* The array receives pointers into imFormatList's own static storage; it
     does not copy into buffers the caller supplies, and freeing what comes
     back corrupts the heap. Nothing in im_format.h says so -- the function
     carries no documentation comment at all -- so the note is here instead.
     It also writes one entry per registered format regardless of how large
     the caller's array is, which is why this one is generous. */
  char* format_list[50];
  int format_count = 0;
  im::Format::List(format_list, format_count);

  CHECK(format_count > 0);
  CHECK(format_count <= 50);

  /* PNG has to be among them, since the cases above just wrote one. */
  bool found_png = false;
  for (int i = 0; i < format_count; i++)
  {
    REQUIRE(format_list[i] != NULL);
    if (strcmp(format_list[i], "PNG") == 0)
      found_png = true;
  }
  CHECK(found_png);
}

TEST_CASE("plus: the Kernel namespace returns real kernels")
{
  /* Each of these allocates through the C API and hands ownership to an
     Image, so the case is as much about the destructor not double freeing --
     under the sanitizers -- as about the contents. */
  im::Image sobel = im::Kernel::Sobel();
  CHECK(!sobel.Failed());
  CHECK(sobel.Width() == 3);
  CHECK(sobel.Height() == 3);

  im::Image gaussian = im::Kernel::Gaussian5x5();
  CHECK(!gaussian.Failed());
  CHECK(gaussian.Width() == 5);

  im::Image mean = im::Kernel::Mean7x7();
  CHECK(!mean.Failed());
  CHECK(mean.Width() == 7);

  im::Image laplacian = im::Kernel::Laplacian4();
  CHECK(!laplacian.Failed());
  CHECK(laplacian.Width() == 3);
}

TEST_CASE("plus: the Calc namespace agrees with the C statistics")
{
  im::Image a(W, H, IM_GRAY, IM_BYTE);
  im::Image b(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!a.Failed());
  fill(a);
  fill(b);

  double rms = -1, reference_rms = -1;
  CHECK(im::Calc::RMSError(a, b, rms) != 0);
  CHECK(imCalcRMSError(a.GetHandle(), b.GetHandle(), &reference_rms) != 0);
  CHECK(rms == doctest::Approx(reference_rms));
  CHECK(rms == doctest::Approx(0.0));

  unsigned long colors = 0, reference_colors = 0;
  CHECK(im::Calc::CountColors(a, colors) != 0);
  CHECK(imCalcCountColors(a.GetHandle(), &reference_colors) != 0);
  CHECK(colors == reference_colors);

  imStats stats, reference_stats;
  CHECK(im::Calc::ImageStatistics(a, stats) != 0);
  CHECK(imCalcImageStatistics(a.GetHandle(), &reference_stats) != 0);
  CHECK(stats.max == doctest::Approx(reference_stats.max));
  CHECK(stats.min == doctest::Approx(reference_stats.min));
  CHECK(stats.mean == doctest::Approx(reference_stats.mean));
}

TEST_CASE("plus: the Histogram wrapper sizes and fills itself correctly")
{
  im::Image image(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!image.Failed());
  for (int i = 0; i < N; i++)
    image.SetValue(0, i / W, i % W, (double)(i % 5));

  im::Histogram histogram(IM_BYTE);
  CHECK(histogram.Count() == imHistogramCount(IM_BYTE));
  CHECK(histogram.Count() == 256);

  CHECK(im::Calc::GrayHistogram(image, histogram, 0) != 0);

  unsigned long reference[256];
  CHECK(imCalcGrayHistogram(image.GetHandle(), reference, 0) != 0);

  for (int i = 0; i < 256; i++)
  {
    CAPTURE(i);
    CHECK(histogram[i] == reference[i]);
  }

  SUBCASE("and the subscript refuses an index outside the table")
  {
    CHECK(histogram[-1] == (unsigned long)-1);
    CHECK(histogram[256] == (unsigned long)-1);
  }

  SUBCASE("and copies rather than shares its buffer")
  {
    im::Histogram copy(histogram);
    CHECK(copy.GetData() != histogram.GetData());
    CHECK(copy[0] == histogram[0]);
  }
}

TEST_CASE("plus: the Process namespace forwards to libim_process")
{
  im::Image source(W, H, IM_GRAY, IM_BYTE);
  im::Image target(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  SUBCASE("a unary arithmetic operation")
  {
    im::Process::UnArithmeticOp(source, target, IM_UN_EQL);
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK(target.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));
    }
  }

  SUBCASE("a binary one, against the C result")
  {
    im::Process::ArithmeticOp(source, source, target, IM_BIN_ADD);

    imImage* reference = imImageCreate(W, H, IM_GRAY, IM_BYTE);
    REQUIRE(reference != NULL);
    imProcessArithmeticOp(source.GetHandle(), source.GetHandle(), reference, IM_BIN_ADD);

    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      CHECK((int)target.GetValue(0, i / W, i % W) ==
            (int)((imbyte*)reference->data[0])[i]);
    }
    imImageDestroy(reference);
  }

  SUBCASE("a geometric one, where the argument order is easy to get wrong")
  {
    im::Process::Mirror(source, target);
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        CAPTURE(x); CAPTURE(y);
        CHECK(target.GetValue(0, y, x) == source.GetValue(0, y, W - 1 - x));
      }
    }
  }

  SUBCASE("and the scalar helpers")
  {
    CHECK(im::Process::GaussianStdDev2KernelSize(1.5) ==
          imGaussianStdDev2KernelSize(1.5));
    CHECK(im::Process::GaussianKernelSize2StdDev(5) ==
          doctest::Approx(imGaussianKernelSize2StdDev(5)));

    int new_width = 0, new_height = 0;
    int ref_width = 0, ref_height = 0;
    im::Process::CalcRotateSize(W, H, new_width, new_height, 0.5, 0.5);
    imProcessCalcRotateSize(W, H, &ref_width, &ref_height, 0.5, 0.5);
    CHECK(new_width == ref_width);
    CHECK(new_height == ref_height);
  }
}
