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


/* ================================================================== *
 * Breadth: every wrapper below is run beside the C function it claims
 * to call, on the same inputs, and the two results compared.
 *
 * The comparison does not need to know what the operation computes. If the
 * wrapper forwards its arguments correctly the two are bit-identical, and if
 * it swaps a pair or picks a different overload they diverge -- which is the
 * only failure mode a header of thin inline forwarders really has. That makes
 * it possible to cover the wide namespaces at a rate that writing expected
 * values never would.
 *
 * Operations that consume randomness are called but not compared; they are
 * marked where they appear.
 * ================================================================== */

namespace {

const int BW = 16;
const int BH = 12;

imImage* c_image(int color_space, int data_type)
{
  imImage* image = imImageCreate(BW, BH, color_space, data_type);
  REQUIRE(image != NULL);
  return image;
}

void fill_c(imImage* image)
{
  int planes = image->has_alpha ? image->depth + 1 : image->depth;
  for (int p = 0; p < planes; p++)
    for (int i = 0; i < image->count; i++)
      ((imbyte**)image->data)[p][i] = (imbyte)((i * 7 + p * 31) & 0xFF);
}

void fill_plus(im::Image& image)
{
  int planes = image.HasAlpha() ? image.Depth() + 1 : image.Depth();
  for (int p = 0; p < planes; p++)
    for (int i = 0; i < BW * BH; i++)
      image.SetValue(p, i / BW, i % BW, (double)((i * 7 + p * 31) & 0xFF));
}

/* Byte-for-byte over every plane, alpha included. */
bool same_data(const imImage* a, const imImage* b)
{
  if (a->width != b->width || a->height != b->height ||
      a->data_type != b->data_type || a->depth != b->depth)
    return false;

  int planes = a->has_alpha ? a->depth + 1 : a->depth;
  int plane_bytes = a->count * imDataTypeSize(a->data_type);
  for (int p = 0; p < planes; p++)
    if (memcmp(a->data[p], b->data[p], plane_bytes) != 0)
      return false;
  return true;
}

} /* namespace */


TEST_CASE("plus: the threshold wrappers match their C functions")
{
  im::Image src(BW, BH, IM_GRAY, IM_BYTE);
  im::Image via_plus(BW, BH, IM_BINARY, IM_BYTE);
  REQUIRE(!src.Failed());
  fill_plus(src);

  imImage* c_src = c_image(IM_GRAY, IM_BYTE);
  imImage* via_c = c_image(IM_BINARY, IM_BYTE);
  fill_c(c_src);

  SUBCASE("Threshold")
  {
    im::Process::Threshold(src, via_plus, 100, 1);
    imProcessThreshold(c_src, via_c, 100, 1);
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("SliceThreshold")
  {
    im::Process::SliceThreshold(src, via_plus, 50, 150);
    imProcessSliceThreshold(c_src, via_c, 50, 150);
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("HysteresisThreshold")
  {
    im::Process::HysteresisThreshold(src, via_plus, 50, 150);
    imProcessHysteresisThreshold(c_src, via_c, 50, 150);
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("OtsuThreshold")
  {
    CHECK(im::Process::OtsuThreshold(src, via_plus) ==
          imProcessOtsuThreshold(c_src, via_c));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("UniformErrThreshold")
  {
    CHECK(im::Process::UniformErrThreshold(src, via_plus) ==
          imProcessUniformErrThreshold(c_src, via_c));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("PercentThreshold")
  {
    CHECK(im::Process::PercentThreshold(src, via_plus, 40) ==
          imProcessPercentThreshold(c_src, via_c, 40));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("MinMaxThreshold")
  {
    CHECK(im::Process::MinMaxThreshold(src, via_plus) ==
          doctest::Approx(imProcessMinMaxThreshold(c_src, via_c)));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("DiffusionErrThreshold")
  {
    im::Process::DiffusionErrThreshold(src, via_plus, 120);
    imProcessDiffusionErrThreshold(c_src, via_c, 120);
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("ThresholdByDiff")
  {
    im::Image other(BW, BH, IM_GRAY, IM_BYTE);
    other.Clear();
    imImage* c_other = c_image(IM_GRAY, IM_BYTE);
    imImageClear(c_other);

    im::Process::ThresholdByDiff(src, other, via_plus);
    imProcessThresholdByDiff(c_src, c_other, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));
    imImageDestroy(c_other);
  }

  imImageDestroy(c_src);
  imImageDestroy(via_c);
}

TEST_CASE("plus: the gray morphology wrappers match their C functions")
{
  im::Image src(BW, BH, IM_GRAY, IM_BYTE);
  im::Image via_plus(BW, BH, IM_GRAY, IM_BYTE);
  REQUIRE(!src.Failed());
  fill_plus(src);

  imImage* c_src = c_image(IM_GRAY, IM_BYTE);
  imImage* via_c = c_image(IM_GRAY, IM_BYTE);
  fill_c(c_src);

  SUBCASE("Erode")
  {
    CHECK(im::Process::GrayMorphErode(src, via_plus, 3) ==
          imProcessGrayMorphErode(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Dilate")
  {
    CHECK(im::Process::GrayMorphDilate(src, via_plus, 3) ==
          imProcessGrayMorphDilate(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Open")
  {
    CHECK(im::Process::GrayMorphOpen(src, via_plus, 3) ==
          imProcessGrayMorphOpen(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Close")
  {
    CHECK(im::Process::GrayMorphClose(src, via_plus, 3) ==
          imProcessGrayMorphClose(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("TopHat")
  {
    CHECK(im::Process::GrayMorphTopHat(src, via_plus, 3) ==
          imProcessGrayMorphTopHat(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Well")
  {
    CHECK(im::Process::GrayMorphWell(src, via_plus, 3) ==
          imProcessGrayMorphWell(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Gradient")
  {
    CHECK(im::Process::GrayMorphGradient(src, via_plus, 3) ==
          imProcessGrayMorphGradient(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }

  imImageDestroy(c_src);
  imImageDestroy(via_c);
}

TEST_CASE("plus: the binary morphology wrappers match their C functions")
{
  im::Image src(BW, BH, IM_BINARY, IM_BYTE);
  im::Image via_plus(BW, BH, IM_BINARY, IM_BYTE);
  REQUIRE(!src.Failed());
  for (int i = 0; i < BW * BH; i++)
    src.SetValue(0, i / BW, i % BW, (double)((i / 3) % 2));

  imImage* c_src = c_image(IM_BINARY, IM_BYTE);
  imImage* via_c = c_image(IM_BINARY, IM_BYTE);
  for (int i = 0; i < c_src->count; i++)
    ((imbyte*)c_src->data[0])[i] = (imbyte)((i / 3) % 2);

  SUBCASE("Erode")
  {
    CHECK(im::Process::BinMorphErode(src, via_plus, 3, 1) ==
          imProcessBinMorphErode(c_src, via_c, 3, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Dilate")
  {
    CHECK(im::Process::BinMorphDilate(src, via_plus, 3, 1) ==
          imProcessBinMorphDilate(c_src, via_c, 3, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Open")
  {
    CHECK(im::Process::BinMorphOpen(src, via_plus, 3, 1) ==
          imProcessBinMorphOpen(c_src, via_c, 3, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Close")
  {
    CHECK(im::Process::BinMorphClose(src, via_plus, 3, 1) ==
          imProcessBinMorphClose(c_src, via_c, 3, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Outline")
  {
    CHECK(im::Process::BinMorphOutline(src, via_plus, 3, 1) ==
          imProcessBinMorphOutline(c_src, via_c, 3, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Thinning")
  {
    im::Process::BinThinZhangSuen(src, via_plus);
    imProcessBinThinZhangSuen(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }

  imImageDestroy(c_src);
  imImageDestroy(via_c);
}

TEST_CASE("plus: the convolution wrappers match their C functions")
{
  im::Image src(BW, BH, IM_GRAY, IM_BYTE);
  im::Image via_plus(BW, BH, IM_GRAY, IM_BYTE);
  REQUIRE(!src.Failed());
  fill_plus(src);

  imImage* c_src = c_image(IM_GRAY, IM_BYTE);
  imImage* via_c = c_image(IM_GRAY, IM_BYTE);
  fill_c(c_src);

  SUBCASE("MeanConvolve")
  {
    CHECK(im::Process::MeanConvolve(src, via_plus, 3) ==
          imProcessMeanConvolve(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("GaussianConvolve")
  {
    CHECK(im::Process::GaussianConvolve(src, via_plus, 1.5) ==
          imProcessGaussianConvolve(c_src, via_c, 1.5));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("MedianConvolve")
  {
    CHECK(im::Process::MedianConvolve(src, via_plus, 3) ==
          imProcessMedianConvolve(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("RangeConvolve")
  {
    CHECK(im::Process::RangeConvolve(src, via_plus, 3) ==
          imProcessRangeConvolve(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("RankMaxConvolve")
  {
    CHECK(im::Process::RankMaxConvolve(src, via_plus, 3) ==
          imProcessRankMaxConvolve(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("RankMinConvolve")
  {
    CHECK(im::Process::RankMinConvolve(src, via_plus, 3) ==
          imProcessRankMinConvolve(c_src, via_c, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("SobelConvolve")
  {
    CHECK(im::Process::SobelConvolve(src, via_plus) ==
          imProcessSobelConvolve(c_src, via_c));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("PrewittConvolve")
  {
    CHECK(im::Process::PrewittConvolve(src, via_plus) ==
          imProcessPrewittConvolve(c_src, via_c));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Convolve with an explicit kernel")
  {
    /* The kernel is a third image, which is where an argument order slip
       would actually be possible. */
    im::Image kernel = im::Kernel::Laplacian4();
    CHECK(im::Process::Convolve(src, via_plus, kernel) ==
          imProcessConvolve(c_src, via_c, kernel.GetHandle()));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Sharp")
  {
    CHECK(im::Process::Sharp(src, via_plus, 1.0, 0.0) ==
          imProcessSharp(c_src, via_c, 1.0, 0.0));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }
  SUBCASE("Unsharp")
  {
    CHECK(im::Process::Unsharp(src, via_plus, 1.5, 1.0, 0.0) ==
          imProcessUnsharp(c_src, via_c, 1.5, 1.0, 0.0));
    CHECK(same_data(via_plus.GetHandle(), via_c));
  }

  imImageDestroy(c_src);
  imImageDestroy(via_c);
}

TEST_CASE("plus: the geometric wrappers match their C functions")
{
  im::Image src(BW, BH, IM_RGB, IM_BYTE);
  REQUIRE(!src.Failed());
  fill_plus(src);

  imImage* c_src = c_image(IM_RGB, IM_BYTE);
  fill_c(c_src);

  SUBCASE("Mirror, Flip and Rotate180 keep the size")
  {
    im::Image via_plus(BW, BH, IM_RGB, IM_BYTE);
    imImage* via_c = c_image(IM_RGB, IM_BYTE);

    im::Process::Mirror(src, via_plus);
    imProcessMirror(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::Flip(src, via_plus);
    imProcessFlip(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::Rotate180(src, via_plus);
    imProcessRotate180(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(via_c);
  }

  SUBCASE("Rotate90 swaps them")
  {
    im::Image via_plus(BH, BW, IM_RGB, IM_BYTE);
    imImage* via_c = imImageCreate(BH, BW, IM_RGB, IM_BYTE);
    REQUIRE(via_c != NULL);

    CHECK(im::Process::Rotate90(src, via_plus, 1) ==
          imProcessRotate90(c_src, via_c, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(via_c);
  }

  SUBCASE("Crop takes a corner")
  {
    im::Image via_plus(BW / 2, BH / 2, IM_RGB, IM_BYTE);
    imImage* via_c = imImageCreate(BW / 2, BH / 2, IM_RGB, IM_BYTE);
    REQUIRE(via_c != NULL);

    CHECK(im::Process::Crop(src, via_plus, 2, 3) ==
          imProcessCrop(c_src, via_c, 2, 3));
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(via_c);
  }

  SUBCASE("Resize and Reduce change it")
  {
    im::Image via_plus(BW * 2, BH * 2, IM_RGB, IM_BYTE);
    imImage* via_c = imImageCreate(BW * 2, BH * 2, IM_RGB, IM_BYTE);
    REQUIRE(via_c != NULL);

    CHECK(im::Process::Resize(src, via_plus, 1) ==
          imProcessResize(c_src, via_c, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));
    imImageDestroy(via_c);

    im::Image small(BW / 2, BH / 2, IM_RGB, IM_BYTE);
    imImage* c_small = imImageCreate(BW / 2, BH / 2, IM_RGB, IM_BYTE);
    REQUIRE(c_small != NULL);

    CHECK(im::Process::Reduce(src, small, 1) ==
          imProcessReduce(c_src, c_small, 1));
    CHECK(same_data(small.GetHandle(), c_small));

    CHECK(im::Process::ReduceBy4(src, small) ==
          imProcessReduceBy4(c_src, c_small));
    CHECK(same_data(small.GetHandle(), c_small));
    imImageDestroy(c_small);
  }

  SUBCASE("Rotate carries its angle through")
  {
    /* Five arguments, two of them a matched pair -- the shape most likely to
       be forwarded in the wrong order. */
    im::Image via_plus(BW, BH, IM_RGB, IM_BYTE);
    imImage* via_c = c_image(IM_RGB, IM_BYTE);

    CHECK(im::Process::Rotate(src, via_plus, 0.8, 0.6, 1) ==
          imProcessRotate(c_src, via_c, 0.8, 0.6, 1));
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(via_c);
  }

  imImageDestroy(c_src);
}

TEST_CASE("plus: the tone and colour wrappers match their C functions")
{
  SUBCASE("on a gray image")
  {
    im::Image src(BW, BH, IM_GRAY, IM_BYTE);
    im::Image via_plus(BW, BH, IM_GRAY, IM_BYTE);
    fill_plus(src);

    imImage* c_src = c_image(IM_GRAY, IM_BYTE);
    imImage* via_c = c_image(IM_GRAY, IM_BYTE);
    fill_c(c_src);

    im::Process::Negative(src, via_plus);
    imProcessNegative(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::EqualizeHistogram(src, via_plus);
    imProcessEqualizeHistogram(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::ExpandHistogram(src, via_plus, 5);
    imProcessExpandHistogram(c_src, via_c, 5);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::Posterize(src, via_plus, 3);
    imProcessPosterize(c_src, via_c, 3);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::Pixelate(src, via_plus, 4);
    imProcessPixelate(c_src, via_c, 4);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    im::Process::QuantizeGrayUniform(src, via_plus, 8);
    imProcessQuantizeGrayUniform(c_src, via_c, 8);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    double gamma_params[1] = { 2.0 };
    im::Process::ToneGamut(src, via_plus, IM_GAMUT_POW, gamma_params);
    imProcessToneGamut(c_src, via_c, IM_GAMUT_POW, gamma_params);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(c_src);
    imImageDestroy(via_c);
  }

  SUBCASE("on a colour image")
  {
    im::Image src(BW, BH, IM_RGB, IM_BYTE);
    im::Image via_plus(BW, BH, IM_RGB, IM_BYTE);
    fill_plus(src);

    imImage* c_src = c_image(IM_RGB, IM_BYTE);
    imImage* via_c = c_image(IM_RGB, IM_BYTE);
    fill_c(c_src);

    im::Process::FixBGR(src, via_plus);
    imProcessFixBGR(c_src, via_c);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    /* NormalizeComponents divides by the component sum, so its target has to
       be IM_FLOAT or IM_DOUBLE -- the header says so, and nothing enforces
       it. Handing it a byte target writes floats into a byte-sized buffer and
       walks off the end of the allocation, which is the same unchecked
       precondition the arithmetic operations have. */
    {
      im::Image normal_plus(BW, BH, IM_RGB, IM_FLOAT);
      imImage* normal_c = imImageCreate(BW, BH, IM_RGB, IM_FLOAT);
      REQUIRE(normal_c != NULL);

      im::Process::NormalizeComponents(src, normal_plus);
      imProcessNormalizeComponents(c_src, normal_c);
      CHECK(same_data(normal_plus.GetHandle(), normal_c));
      imImageDestroy(normal_c);
    }

    /* Quantization produces an indexed image, not another RGB one. */
    {
      im::Image map_plus(BW, BH, IM_MAP, IM_BYTE);
      imImage* map_c = imImageCreate(BW, BH, IM_MAP, IM_BYTE);
      REQUIRE(map_c != NULL);

      im::Process::QuantizeRGBUniform(src, map_plus, 0);
      imProcessQuantizeRGBUniform(c_src, map_c, 0);
      CHECK(same_data(map_plus.GetHandle(), map_c));
      imImageDestroy(map_c);
    }

    double src_color[3] = { 10, 20, 30 };
    double dst_color[3] = { 40, 50, 60 };
    im::Process::ReplaceColor(src, via_plus, src_color, dst_color);
    imProcessReplaceColor(c_src, via_c, src_color, dst_color);
    CHECK(same_data(via_plus.GetHandle(), via_c));

    imImageDestroy(c_src);
    imImageDestroy(via_c);
  }

  SUBCASE("splitting a colour image into planes and back")
  {
    im::Image src(BW, BH, IM_RGB, IM_BYTE);
    fill_plus(src);

    im::Image parts[3] = {
      im::Image(BW, BH, IM_GRAY, IM_BYTE),
      im::Image(BW, BH, IM_GRAY, IM_BYTE),
      im::Image(BW, BH, IM_GRAY, IM_BYTE)
    };
    im::Process::SplitComponents(src, parts);

    for (int p = 0; p < 3; p++)
    {
      CAPTURE(p);
      for (int i = 0; i < BW * BH; i++)
        CHECK(parts[p].GetValue(0, i / BW, i % BW) ==
              src.GetValue(p, i / BW, i % BW));
    }

    im::Image rebuilt(BW, BH, IM_RGB, IM_BYTE);
    im::Process::MergeComponents(parts, rebuilt);
    CHECK(same_data(rebuilt.GetHandle(), src.GetHandle()));
  }
}

TEST_CASE("plus: the bitwise wrappers match their C functions")
{
  im::Image a(BW, BH, IM_GRAY, IM_BYTE);
  im::Image b(BW, BH, IM_GRAY, IM_BYTE);
  im::Image via_plus(BW, BH, IM_GRAY, IM_BYTE);
  fill_plus(a);
  fill_plus(b);

  imImage* c_a = c_image(IM_GRAY, IM_BYTE);
  imImage* c_b = c_image(IM_GRAY, IM_BYTE);
  imImage* via_c = c_image(IM_GRAY, IM_BYTE);
  fill_c(c_a);
  fill_c(c_b);

  im::Process::BitwiseOp(a, b, via_plus, IM_BIT_AND);
  imProcessBitwiseOp(c_a, c_b, via_c, IM_BIT_AND);
  CHECK(same_data(via_plus.GetHandle(), via_c));

  im::Process::BitwiseNot(a, via_plus);
  imProcessBitwiseNot(c_a, via_c);
  CHECK(same_data(via_plus.GetHandle(), via_c));

  im::Process::BitMask(a, via_plus, 0x3C, IM_BIT_OR);
  imProcessBitMask(c_a, via_c, 0x3C, IM_BIT_OR);
  CHECK(same_data(via_plus.GetHandle(), via_c));

  im::Process::BitPlane(a, via_plus, 3, 0);
  imProcessBitPlane(c_a, via_c, 3, 0);
  CHECK(same_data(via_plus.GetHandle(), via_c));

  imImageDestroy(c_a);
  imImageDestroy(c_b);
  imImageDestroy(via_c);
}

TEST_CASE("plus: the render wrappers match their C functions")
{
  /* Deterministic renders only. RenderRandomNoise and the three noise
     wrappers consume the random generator, so two calls cannot agree; they
     are exercised for compilation and linkage in the case after this one. */
  im::Image via_plus(BW, BH, IM_GRAY, IM_BYTE);
  imImage* via_c = c_image(IM_GRAY, IM_BYTE);

  double value[1] = { 77 };
  CHECK(im::Process::RenderConstant(via_plus, value) ==
        imProcessRenderConstant(via_c, value));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderRamp(via_plus, 0, 255, 1) ==
        imProcessRenderRamp(via_c, 0, 255, 1));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderBox(via_plus, 6, 4) ==
        imProcessRenderBox(via_c, 6, 4));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderCone(via_plus, 5) ==
        imProcessRenderCone(via_c, 5));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderTent(via_plus, 6, 4) ==
        imProcessRenderTent(via_c, 6, 4));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderWheel(via_plus, 2, 5) ==
        imProcessRenderWheel(via_c, 2, 5));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderSinc(via_plus, 2.0, 2.0) ==
        imProcessRenderSinc(via_c, 2.0, 2.0));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderGaussian(via_plus, 2.0) ==
        imProcessRenderGaussian(via_c, 2.0));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderCosine(via_plus, 4.0, 4.0) ==
        imProcessRenderCosine(via_c, 4.0, 4.0));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  CHECK(im::Process::RenderGrid(via_plus, 3, 3) ==
        imProcessRenderGrid(via_c, 3, 3));
  CHECK(same_data(via_plus.GetHandle(), via_c));

  imImageDestroy(via_c);
}

TEST_CASE("plus: the random render wrappers compile, link and run")
{
  /* No comparison is possible -- each call draws from the generator -- so
     this case exists for the two failure modes that remain: a wrapper that
     does not compile, and one that does not link. Both have happened in this
     header already. */
  im::Image target(BW, BH, IM_GRAY, IM_BYTE);
  im::Image source(BW, BH, IM_GRAY, IM_BYTE);
  fill_plus(source);

  CHECK(im::Process::RenderRandomNoise(target) != 0);
  CHECK(im::Process::RenderAddGaussianNoise(source, target, 0.0, 10.0) != 0);
  CHECK(im::Process::RenderAddUniformNoise(source, target, 0.0, 10.0) != 0);
  CHECK(im::Process::RenderAddSpeckleNoise(source, target, 5.0) != 0);
}

TEST_CASE("plus: AttribTable copies its entries instead of an uninitialised pointer")
{
  im::AttribTable table(101);
  table.SetInteger("count", IM_INT, 7);
  table.SetReal("ratio", IM_DOUBLE, 2.5);
  table.SetString("label", "hello");
  CHECK(table.Count() == 3);

  /* The copy constructor never assigned its own ptable before copying into
     it, so this dereferenced whatever the member happened to hold. Nothing
     instantiated it, so nothing found out. */
  im::AttribTable copy(table);
  CHECK(copy.Count() == 3);
  CHECK(copy.GetInteger("count") == 7);
  CHECK(copy.GetReal("ratio") == doctest::Approx(2.5));
  CHECK(strcmp(copy.GetString("label"), "hello") == 0);

  SUBCASE("and the copy is independent")
  {
    copy.SetInteger("count", IM_INT, 99);
    CHECK(table.GetInteger("count") == 7);
  }

  SUBCASE("and the rest of the table interface works")
  {
    im::AttribTable other(101);
    other.SetInteger("extra", IM_INT, 5);
    other.MergeFrom(table);
    CHECK(other.Count() == 4);
    CHECK(other.GetInteger("count") == 7);

    other.Reset("count");
    CHECK(other.Get("count") == NULL);

    other.RemoveAll();
    CHECK(other.Count() == 0);
  }
}

TEST_CASE("plus: the Analyze namespace forwards to the C analysis")
{
  im::Image binary(BW, BH, IM_BINARY, IM_BYTE);
  REQUIRE(!binary.Failed());
  binary.Clear();

  /* Two separated blobs, so the region count is knowable without trusting
     the implementation. */
  binary.SetValue(0, 1, 1, 1);
  binary.SetValue(0, 1, 2, 1);
  binary.SetValue(0, 2, 1, 1);
  binary.SetValue(0, 8, 10, 1);
  binary.SetValue(0, 9, 10, 1);

  im::Image regions(BW, BH, IM_GRAY, IM_USHORT);
  int region_count = 0;
  CHECK(im::Analyze::FindRegions(binary, regions, 4, 0, region_count) != 0);
  CHECK(region_count == 2);

  im::MeasureTable measures(region_count);
  CHECK(measures.RegionCount() == 2);

  CHECK(im::Analyze::MeasureArea(regions, measures) != 0);
  CHECK(im::Analyze::MeasureCentroid(regions, measures) != 0);
  CHECK(im::Analyze::MeasurePerimeter(regions, measures) != 0);

  /* The first blob has three pixels and the second two, whichever order the
     labelling assigned them. */
  const int* area = (const int*)measures.GetMeasure("Area");
  REQUIRE(area != NULL);
  CHECK(((area[0] == 3 && area[1] == 2) || (area[0] == 2 && area[1] == 3)));
}

TEST_CASE("plus: FileRaw opens and closes a raw file")
{
  std::string path = scratch("plus_raw.raw");

  im::Image source(BW, BH, IM_GRAY, IM_BYTE);
  fill_plus(source);

  int error = IM_ERR_NONE;
  {
    im::FileRaw out(path.c_str(), error, true);
    REQUIRE_MESSAGE(!out.Failed(), "create failed, error " << error);

    out.SetAttribInteger("Width", IM_INT, BW);
    out.SetAttribInteger("Height", IM_INT, BH);
    out.SetAttribInteger("ColorMode", IM_INT, IM_GRAY);
    out.SetAttribInteger("DataType", IM_INT, IM_BYTE);
    CHECK(out.SaveImage(source) == IM_ERR_NONE);
  }

  im::FileRaw in(path.c_str(), error, false);
  REQUIRE_MESSAGE(!in.Failed(), "open failed, error " << error);

  in.SetAttribInteger("Width", IM_INT, BW);
  in.SetAttribInteger("Height", IM_INT, BH);
  in.SetAttribInteger("ColorMode", IM_INT, IM_GRAY);
  in.SetAttribInteger("DataType", IM_INT, IM_BYTE);

  im::Image loaded = in.LoadImage(0, error);
  REQUIRE_MESSAGE(!loaded.Failed(), "load failed, error " << error);
  CHECK(same_data(loaded.GetHandle(), source.GetHandle()));
}
