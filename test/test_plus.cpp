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


/* ================================================================== *
 * The rest of the wrapper surface.
 *
 * Same argument as the first pass, applied to what it left: an inline that
 * nothing instantiates is not covered by "it compiles", because it is not
 * compiled. Three defects in this header have been found exactly that way --
 * an inverted flag, a leak, and a copy constructor using an uninitialised
 * pointer -- so the remainder is worth walking through rather than assuming
 * the pattern held.
 *
 * VideoCapture and VideoCaptureDevice stay out, as before: they wrap
 * libim_capture, which this build does not produce, so referencing them fails
 * to link rather than failing a test. That is about thirty of the functions
 * still showing as uncovered and there is nothing to be done about them here.
 * ================================================================== */

TEST_CASE("plus: the palette colour helpers agree with the C encoders")
{
  /* Static helpers on Palette rather than free functions, so they need the
     class instantiated to exist at all. */
  for (int i = 0; i < 256; i += 37)
  {
    CAPTURE(i);
    imbyte r = (imbyte)i, g = (imbyte)(255 - i), b = (imbyte)((i * 3) & 0xFF);

    long encoded = im::Palette::ColorEncode(r, g, b);
    CHECK(encoded == imColorEncode(r, g, b));
    CHECK(im::Palette::ColorRed(encoded) == r);
    CHECK(im::Palette::ColorGreen(encoded) == g);
    CHECK(im::Palette::ColorBlue(encoded) == b);
  }
}

TEST_CASE("plus: every built-in palette generator is reachable")
{
  /* Each of these allocates through the C API and hands ownership to a
     Palette, so this is as much about the destructor not double freeing --
     under the sanitizers -- as about the entries. */
  struct { const char* name; im::Palette (*make)(); } cases[] = {
    { "gray",          im::Palette::Gray          },
    { "red",           im::Palette::Red           },
    { "green",         im::Palette::Green         },
    { "blue",          im::Palette::Blue          },
    { "yellow",        im::Palette::Yellow        },
    { "magenta",       im::Palette::Magenta       },
    { "cyan",          im::Palette::Cyan          },
    { "rainbow",       im::Palette::Rainbow       },
    { "hues",          im::Palette::Hues          },
    { "blue ice",      im::Palette::BlueIce       },
    { "hot iron",      im::Palette::HotIron       },
    { "black body",    im::Palette::BlackBody     },
    { "high contrast", im::Palette::HighContrast  },
    { "linear",        im::Palette::Linear        },
    { "uniform",       im::Palette::Uniform       },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].name);
    im::Palette p = cases[c].make();
    CHECK(p.Count() == 256);
    REQUIRE(p.GetData() != NULL);

    /* Not one flat colour, which is what an unfilled palette looks like.
       Individual entries may legitimately repeat -- HighContrast cycles a
       small set deliberately -- so this counts distinct values rather than
       comparing two arbitrary slots. */
    int distinct = 0;
    for (int i = 1; i < 256; i++)
      if (p.GetData()[i] != p.GetData()[0]) { distinct = 1; break; }
    CHECK(distinct == 1);
  }
}

TEST_CASE("plus: every kernel generator is reachable and odd-sized")
{
  struct { const char* name; im::Image (*make)(); int size; } cases[] = {
    { "Sobel",          im::Kernel::Sobel,          3 },
    { "Prewitt",        im::Kernel::Prewitt,        3 },
    { "Kirsh",          im::Kernel::Kirsh,          3 },
    { "Laplacian4",     im::Kernel::Laplacian4,     3 },
    { "Laplacian8",     im::Kernel::Laplacian8,     3 },
    { "Laplacian5x5",   im::Kernel::Laplacian5x5,   5 },
    { "Laplacian7x7",   im::Kernel::Laplacian7x7,   7 },
    { "Gradian3x3",     im::Kernel::Gradian3x3,     3 },
    { "Gradian7x7",     im::Kernel::Gradian7x7,     7 },
    { "Sculpt",         im::Kernel::Sculpt,         3 },
    { "Mean3x3",        im::Kernel::Mean3x3,        3 },
    { "Mean5x5",        im::Kernel::Mean5x5,        5 },
    { "CircularMean5x5",im::Kernel::CircularMean5x5,5 },
    { "Mean7x7",        im::Kernel::Mean7x7,        7 },
    { "CircularMean7x7",im::Kernel::CircularMean7x7,7 },
    { "Gaussian3x3",    im::Kernel::Gaussian3x3,    3 },
    { "Gaussian5x5",    im::Kernel::Gaussian5x5,    5 },
    { "Barlett5x5",     im::Kernel::Barlett5x5,     5 },
    { "TopHat5x5",      im::Kernel::TopHat5x5,      5 },
    { "TopHat7x7",      im::Kernel::TopHat7x7,      7 },
    { "Enhance",        im::Kernel::Enhance,        5 },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].name);
    im::Image k = cases[c].make();
    REQUIRE(!k.Failed());

    /* A convolution kernel has to have a centre, so both sides are odd, and
       the declared size is what the name says. */
    CHECK(k.Width() == cases[c].size);
    CHECK(k.Height() == cases[c].size);
    CHECK(k.Width() % 2 == 1);

    /* And it is not all zeroes, which would convolve everything to nothing. */
    double sum_abs = 0;
    for (int y = 0; y < k.Height(); y++)
      for (int x = 0; x < k.Width(); x++)
      {
        double v = k.GetValue(0, y, x);
        sum_abs += v < 0 ? -v : v;
      }
    CHECK(sum_abs > 0);
  }
}

TEST_CASE("plus: the remaining Image methods reach their C functions")
{
  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  SUBCASE("attributes copy and merge between images")
  {
    source.SetAttribInteger("Alpha", IM_INT, 1);
    source.SetAttribInteger("Beta", IM_INT, 2);

    im::Image target(W, H, IM_RGB, IM_BYTE);
    target.SetAttribInteger("Beta", IM_INT, 99);
    target.SetAttribInteger("Gamma", IM_INT, 3);

    source.CopyAttributes(target);
    CHECK(target.GetAttribInteger("Alpha") == 1);
    CHECK(target.GetAttribInteger("Beta") == 2);   /* copy overwrites */

    im::Image merged(W, H, IM_RGB, IM_BYTE);
    merged.SetAttribInteger("Beta", IM_INT, 99);
    source.MergeAttributes(merged);
    CHECK(merged.GetAttribInteger("Alpha") == 1);
    CHECK(merged.GetAttribInteger("Beta") == 99);  /* merge preserves */
  }

  SUBCASE("the raw attribute accessors round-trip")
  {
    const int values[3] = { 7, 8, 9 };
    source.SetAttribute("Triple", IM_INT, 3, values);

    int data_type = -1, count = -1;
    const void* got = source.GetAttribute("Triple", &data_type, &count);
    REQUIRE(got != NULL);
    CHECK(data_type == IM_INT);
    CHECK(count == 3);
    CHECK(((const int*)got)[2] == 9);

    /* The list has to name what was set. */
    char* names[64];
    int name_count = 64;
    source.GetAttributeList(names, name_count);
    CHECK(name_count > 0);

    bool found = false;
    for (int i = 0; i < name_count; i++)
      if (names[i] && strcmp(names[i], "Triple") == 0)
        found = true;
    CHECK(found);
  }

  SUBCASE("CopyData moves samples without touching the metadata")
  {
    im::Image target(W, H, IM_RGB, IM_BYTE);
    target.SetAttribInteger("Kept", IM_INT, 5);
    source.CopyData(target);

    for (int i = 0; i < N; i++)
      CHECK(target.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));
    CHECK(target.GetAttribInteger("Kept") == 5);
  }

  SUBCASE("the setters relabel and the makers rescale, and neither does both")
  {
    /* Easy to assume otherwise from the names. SetGray, SetBinary and SetMap
       change what the image claims to be and leave every sample alone.
       MakeBinary and MakeGray do the opposite: they rescale the samples --
       to 0/1 and to 0/255 -- and leave the colour space exactly as it was. */
    im::Image image(W, H, IM_GRAY, IM_BYTE);
    for (int i = 0; i < N; i++)
      image.SetValue(0, i / W, i % W, (double)((i % 2) ? 200 : 0));

    image.SetBinary();
    CHECK(image.ColorSpace() == IM_BINARY);
    CHECK(image.GetValue(0, 0, 1) == 200);      /* relabelled, not rescaled */

    image.SetMap();
    CHECK(image.ColorSpace() == IM_MAP);
    image.SetGray();
    CHECK(image.ColorSpace() == IM_GRAY);
    CHECK(image.GetValue(0, 0, 1) == 200);

    image.MakeBinary();
    CHECK(image.GetValue(0, 0, 1) == 1);        /* rescaled, not relabelled */
    CHECK(image.GetValue(0, 0, 0) == 0);
    CHECK(image.ColorSpace() == IM_GRAY);

    image.MakeGray();
    CHECK(image.GetValue(0, 0, 1) == 255);
    CHECK(image.GetValue(0, 0, 0) == 0);
    CHECK(image.ColorSpace() == IM_GRAY);
  }

  SUBCASE("Reshape keeps the sample count")
  {
    im::Image flat(W * H, 1, IM_GRAY, IM_BYTE);
    REQUIRE(!flat.Failed());
    flat.Reshape(W, H);
    CHECK(flat.Width() == W);
    CHECK(flat.Height() == H);
  }

  SUBCASE("ConvertToBitmap produces something displayable")
  {
    im::Image wide(W, H, IM_GRAY, IM_INT);
    for (int i = 0; i < N; i++)
      wide.SetValue(0, i / W, i % W, (double)(i * 100));

    im::Image bitmap(W, H, IM_GRAY, IM_BYTE);
    CHECK(wide.ConvertToBitmap(bitmap, IM_CPX_REAL, 0, false, IM_CAST_MINMAX) == IM_ERR_NONE);
    CHECK(bitmap.IsBitmap());
  }
}

TEST_CASE("plus: the remaining File methods reach their C functions")
{
  std::string path = scratch("plus_file_attribs.tif");

  im::Image source(W, H, IM_RGB, IM_BYTE);
  REQUIRE(!source.Failed());
  fill(source);

  int error = IM_ERR_NONE;
  {
    im::File out(path.c_str(), "TIFF", error);
    REQUIRE_MESSAGE(!out.Failed(), "create failed, error " << error);

    out.SetInfo("NONE");

    const int triple[3] = { 4, 5, 6 };
    out.SetAttribute("Triple", IM_INT, 3, triple);
    out.SetAttribReal("Scale", IM_DOUBLE, 2.5);
    out.SetAttribString("Author", "a name");

    int data_type = -1, count = -1;
    const void* got = out.GetAttribute("Triple", data_type, count);
    REQUIRE(got != NULL);
    CHECK(data_type == IM_INT);
    CHECK(count == 3);

    CHECK(out.GetAttribReal("Scale", 0) == doctest::Approx(2.5));
    CHECK(strcmp(out.GetAttribString("Author"), "a name") == 0);

    CHECK(out.SaveImage(source) == IM_ERR_NONE);
  }

  SUBCASE("LoadFrame fills an image the caller already has")
  {
    im::File in(path.c_str(), error);
    REQUIRE_MESSAGE(!in.Failed(), "open failed, error " << error);

    im::Image target(W, H, IM_RGB, IM_BYTE);
    error = IM_ERR_NONE;
    in.LoadFrame(0, target, error, false);
    REQUIRE_MESSAGE(error == IM_ERR_NONE, "load frame error " << error);

    for (int i = 0; i < N; i++)
      CHECK(target.GetValue(0, i / W, i % W) == source.GetValue(0, i / W, i % W));
  }
}

TEST_CASE("plus: the Format namespace reaches its remaining entry points")
{
  /* RegisterInternal is idempotent -- the C side guards it with a flag -- so
     calling it here is safe. RemoveAll deliberately is not called: it would
     unregister every driver for the rest of the process, and while CTest runs
     each case separately, a developer running the binary directly would see
     every later format case fail for no visible reason. */
  im::Format::RegisterInternal();

  char extra[256] = { 0 };
  CHECK(im::Format::InfoExtra("TIFF", extra) == IM_ERR_NONE);

  /* Same contract as Format::List: the array receives pointers into
     imFormatCompressions' own static storage rather than copies into buffers
     the caller supplies, so allocating them and freeing what comes back
     corrupts the heap. Neither function documents it. */
  char* comp[64];
  int comp_count = 0;

  CHECK(im::Format::Compressions("TIFF", comp, comp_count, IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(comp_count > 0);
  CHECK(comp_count <= 64);

  /* TIFF advertises several, and NONE is always among the ones it can write
     an 8-bit RGB image with. */
  bool found_none = false;
  for (int i = 0; i < comp_count; i++)
  {
    REQUIRE(comp[i] != NULL);
    if (strcmp(comp[i], "NONE") == 0) found_none = true;
  }
  CHECK(found_none);
}

TEST_CASE("plus: the remaining Calc entry points agree with the C ones")
{
  im::Image image(W, H, IM_GRAY, IM_BYTE);
  REQUIRE(!image.Failed());
  for (int i = 0; i < N; i++)
    image.SetValue(0, i / W, i % W, (double)((i * 11) % 200));

  SUBCASE("per-plane histogram")
  {
    im::Histogram histogram(IM_BYTE);
    CHECK(im::Calc::Histogram(image, histogram, 0, 0) != 0);

    unsigned long reference[256];
    CHECK(imCalcHistogram(image.GetHandle(), reference, 0, 0) != 0);
    for (int i = 0; i < 256; i++)
    {
      CAPTURE(i);
      CHECK(histogram[i] == reference[i]);
    }
  }

  SUBCASE("histogram statistics")
  {
    imStats stats, reference;
    CHECK(im::Calc::HistogramStatistics(image, stats) != 0);
    CHECK(imCalcHistogramStatistics(image.GetHandle(), &reference) != 0);
    CHECK(stats.mean == doctest::Approx(reference.mean));
    CHECK(stats.stddev == doctest::Approx(reference.stddev));
  }

  SUBCASE("median and mode")
  {
    int median = -1, mode = -1, ref_median = -1, ref_mode = -1;
    CHECK(im::Calc::HistoImageStatistics(image, &median, &mode) != 0);
    CHECK(imCalcHistoImageStatistics(image.GetHandle(), &ref_median, &ref_mode) != 0);
    CHECK(median == ref_median);
    CHECK(mode == ref_mode);
  }

  SUBCASE("percent min and max")
  {
    int lo = -1, hi = -1, ref_lo = -1, ref_hi = -1;
    CHECK(im::Calc::PercentMinMax(image, 10, 0, lo, hi) != 0);
    CHECK(imCalcPercentMinMax(image.GetHandle(), 10, 0, &ref_lo, &ref_hi) != 0);
    CHECK(lo == ref_lo);
    CHECK(hi == ref_hi);
    CHECK(lo <= hi);
  }

  SUBCASE("signal to noise ratio")
  {
    im::Image noise(W, H, IM_GRAY, IM_BYTE);
    for (int i = 0; i < N; i++)
      noise.SetValue(0, i / W, i % W, (double)(i % 5));

    double snr = -1, reference = -1;
    CHECK(im::Calc::SNR(image, noise, snr) != 0);
    CHECK(imCalcSNR(image.GetHandle(), noise.GetHandle(), &reference) != 0);
    CHECK(snr == doctest::Approx(reference));
  }
}

TEST_CASE("plus: the remaining Analyze measurements reach their C functions")
{
  /* Its own size rather than the file's 6x4: FindRegions is called with
     touch_border 0, which discards any region reaching the edge, and a blob
     big enough to have a hole does not fit inside a 6x4 image without
     touching one. */
  const int AW = 16, AH = 12;

  im::Image binary(AW, AH, IM_BINARY, IM_BYTE);
  REQUIRE(!binary.Failed());
  binary.Clear();

  /* A solid blob with a single hole punched in it, well clear of the edge. */
  for (int y = 3; y <= 8; y++)
    for (int x = 3; x <= 9; x++)
      binary.SetValue(0, y, x, 1);
  binary.SetValue(0, 5, 6, 0);

  im::Image regions(AW, AH, IM_GRAY, IM_USHORT);
  int region_count = 0;
  REQUIRE(im::Analyze::FindRegions(binary, regions, 4, 0, region_count) != 0);
  REQUIRE(region_count >= 1);

  im::MeasureTable measures(region_count);

  /* Order matters here and nothing enforces it: MeasurePrincipalAxis reads
     the Area and Centroid rows out of the table and passes whatever it finds
     to the C function, so calling it before MeasureArea and MeasureCentroid
     hands that function null pointers. The wrapper offers no hint of the
     dependency -- worth knowing before reaching for these out of order. */
  REQUIRE(im::Analyze::MeasureArea(regions, measures) != 0);
  REQUIRE(im::Analyze::MeasureCentroid(regions, measures) != 0);

  CHECK(im::Analyze::MeasurePerimArea(regions, measures) != 0);
  CHECK(im::Analyze::MeasurePrincipalAxis(regions, measures) != 0);
  CHECK(im::Analyze::MeasureHoles(regions, 4, measures) != 0);
  CHECK(im::Analyze::MeasurePerimeter(regions, measures) != 0);

  /* The blob is 6 by 7 with one pixel punched out, so its area and hole
     count are both known rather than merely plausible. */
  const int* area = measures.GetMeasureInt("Area");
  REQUIRE(area != NULL);
  CHECK(area[0] == 6 * 7 - 1);

  const int* holes = measures.GetMeasureInt("HolesCount");
  REQUIRE(holes != NULL);
  CHECK(holes[0] == 1);

  const int* holes_area = measures.GetMeasureInt("HolesArea");
  REQUIRE(holes_area != NULL);
  CHECK(holes_area[0] == 1);

  /* GetMeasureDouble is the separate accessor for the real-valued rows. */
  const double* perimeter = measures.GetMeasureDouble("Perimeter");
  REQUIRE(perimeter != NULL);
  CHECK(perimeter[0] > 0);

  const double* major = measures.GetMeasureDouble("MajorLength");
  REQUIRE(major != NULL);
  CHECK(major[0] > 0);
}

TEST_CASE("plus: AttribTable iterates its entries")
{
  im::AttribTable table(101);
  table.SetInteger("one", IM_INT, 1);
  table.SetInteger("two", IM_INT, 2);
  table.SetInteger("three", IM_INT, 3);

  struct Counter
  {
    static int count(void* user_data, int, const char*, int, int, const void*)
    {
      (*(int*)user_data)++;
      return 1;
    }
  };

  int seen = 0;
  table.ForEach(&seen, Counter::count);
  CHECK(seen == 3);
  CHECK(seen == table.Count());
}


/* ================================================================== *
 * The last of the Process namespace.
 *
 * Eighty-seven wrappers were still unexecuted after everything above,
 * which is nearly all of what remained uncovered in this header once
 * VideoCapture is set aside. They are all one-line forwards, so what
 * is left to get wrong is narrow but real: these are not templates,
 * so their bodies are type-checked by anything that includes the
 * header whether or not it calls them -- but an argument passed in the
 * wrong position, or a parameter quietly dropped, type-checks fine and
 * produces a plausible answer. That is what the inverted as_bitmap
 * flag was, and reading through this batch turned up MultipleMedian
 * putting its "delete[]" after the "return", which leaked the array on
 * every call and which no compiler warned about because nothing had
 * instantiated the function.
 *
 * Every case runs the wrapper and the C function it names on identical
 * inputs and compares the results byte for byte, which is the only way
 * a transposed pair of arguments shows up. Inputs are seeded by filling
 * the C image and copying its planes to the C++ one, rather than
 * filling both from the same formula twice -- two loops that were meant
 * to agree are one more thing that can silently disagree.
 * ================================================================== */

namespace {

/* Values that suit the data type, rather than a byte pattern reinterpreted --
   a float plane filled with arbitrary bytes is mostly NaN, and while memcmp
   compares those fine it makes every operation's output meaningless. */
template <class T>
void fill_typed(void* data, int count, int plane, int variant)
{
  T* values = (T*)data;
  for (int i = 0; i < count; i++)
    values[i] = (T)(((i * (7 + variant * 4) + plane * 31 + variant * 13) % 200) + 1);
}

/* "variant" makes two sources genuinely different. Several operations here take
   two images and are not symmetric in them, and the only way to show a wrapper
   passes them in the right order is for exchanging them to change the answer --
   which it cannot if both sources were seeded from the same formula. */
void seed(imImage* image, int variant = 0)
{
  int planes = image->has_alpha? image->depth + 1: image->depth;
  for (int p = 0; p < planes; p++)
  {
    if (image->color_space == IM_BINARY)
    {
      /* 0 and 1 only, in blobs rather than a checkerboard, so the region
         operations have something connected to find. */
      imbyte* values = (imbyte*)image->data[p];
      for (int i = 0; i < image->count; i++)
      {
        int x = i % image->width, y = i / image->width;
        values[i] = (imbyte)(((x / 3 + y / 2 + variant) % 2 == 0 ||
                              (x * y) % (11 + variant) == 0)? 1: 0);
      }
      continue;
    }

    switch (image->data_type)
    {
    case IM_BYTE:    fill_typed<imbyte>  (image->data[p], image->count, p, variant); break;
    case IM_SHORT:   fill_typed<short>   (image->data[p], image->count, p, variant); break;
    case IM_USHORT:  fill_typed<imushort>(image->data[p], image->count, p, variant); break;
    case IM_INT:     fill_typed<int>     (image->data[p], image->count, p, variant); break;
    case IM_FLOAT:   fill_typed<float>   (image->data[p], image->count, p, variant); break;
    case IM_DOUBLE:  fill_typed<double>  (image->data[p], image->count, p, variant); break;
    /* A complex plane is real and imaginary interleaved, so twice the reals. */
    case IM_CFLOAT:  fill_typed<float>   (image->data[p], image->count*2, p, variant); break;
    case IM_CDOUBLE: fill_typed<double>  (image->data[p], image->count*2, p, variant); break;
    default: break;
    }
  }
}

/* Values in 0-1, which several of the colour and normalization operations
   require of a real source. */
void seed_normalized(imImage* image)
{
  int planes = image->has_alpha? image->depth + 1: image->depth;
  for (int p = 0; p < planes; p++)
    for (int i = 0; i < image->count; i++)
    {
      double v = (double)((i * 7 + p * 31) % 100) / 99.0;
      if (image->data_type == IM_FLOAT) ((float*)image->data[p])[i] = (float)v;
      else                              ((double*)image->data[p])[i] = v;
    }
}

/* A matched pair: one C image and one wrapper image of the same shape, holding
   the same bytes. Destroys the C one and lets the wrapper destroy its own. */
struct Pair
{
  imImage* c;
  im::Image plus;

  Pair(int color_space, int data_type, int variant = 0, int normalized = 0)
    : c(imImageCreate(BW, BH, color_space, data_type)),
      plus(BW, BH, color_space, data_type)
  {
    REQUIRE(c != NULL);
    REQUIRE(!plus.Failed());
    if (normalized) seed_normalized(c); else seed(c, variant);
    int planes = c->has_alpha? c->depth + 1: c->depth;
    int plane_bytes = c->count * imDataTypeSize(c->data_type);
    for (int p = 0; p < planes; p++)
      memcpy(plus.GetHandle()->data[p], c->data[p], plane_bytes);
  }

  ~Pair() { if (c) imImageDestroy(c); }

private:
  Pair(const Pair&);
  Pair& operator=(const Pair&);
};

/* A destination still holding nothing but its fill byte means nothing ran.
   Worth asserting next to every comparison against C, because when a
   precondition fails BOTH sides return without writing and the comparison
   agrees on "untouched" -- which is exactly how a guard that made
   imProcessDirectConv impossible to satisfy passed this file until a build
   with asserts enabled aborted on the assert beside it. */
bool wrote_something(const imImage* image)
{
  int planes = image->has_alpha? image->depth + 1: image->depth;
  int plane_bytes = image->count * imDataTypeSize(image->data_type);
  for (int p = 0; p < planes; p++)
  {
    const unsigned char* raw = (const unsigned char*)image->data[p];
    for (int i = 0; i < plane_bytes; i++)
      if (raw[i] != 0x5A) return true;
  }
  return false;
}

/* An empty destination pair, not seeded -- both start as imImageCreate leaves
   them so that "the wrapper never wrote anything" cannot pass by accident. */
struct DstPair
{
  imImage* c;
  im::Image plus;

  DstPair(int color_space, int data_type, int w = BW, int h = BH)
    : c(imImageCreate(w, h, color_space, data_type)), plus(w, h, color_space, data_type)
  {
    REQUIRE(c != NULL);
    REQUIRE(!plus.Failed());
    int planes = c->has_alpha? c->depth + 1: c->depth;
    int plane_bytes = c->count * imDataTypeSize(c->data_type);
    for (int p = 0; p < planes; p++)
    {
      memset(c->data[p], 0x5A, plane_bytes);
      memset(plus.GetHandle()->data[p], 0x5A, plane_bytes);
    }
  }

  ~DstPair() { if (c) imImageDestroy(c); }

private:
  DstPair(const DstPair&);
  DstPair& operator=(const DstPair&);
};

} /* namespace */


TEST_CASE("plus: the remaining gray filters forward to their C functions")
{
  Pair src(IM_GRAY, IM_BYTE);
  DstPair dst(IM_GRAY, IM_BYTE);

  SUBCASE("BarlettConvolve")
  {
    CHECK(im::Process::BarlettConvolve(src.plus, dst.plus, 5) ==
          imProcessBarlettConvolve(src.c, dst.c, 5));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("RankClosestConvolve")
  {
    CHECK(im::Process::RankClosestConvolve(src.plus, dst.plus, 3) ==
          imProcessRankClosestConvolve(src.c, dst.c, 3));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("LapOfGaussianConvolve")
  {
    CHECK(im::Process::LapOfGaussianConvolve(src.plus, dst.plus, 1.5) ==
          imProcessLapOfGaussianConvolve(src.c, dst.c, 1.5));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("DiffOfGaussianConvolve")
  {
    /* Two stddevs, and they are not interchangeable -- a difference of
       gaussians with them the wrong way round is the negative of the right
       answer, which is exactly the slip this comparison catches. */
    CHECK(im::Process::DiffOfGaussianConvolve(src.plus, dst.plus, 1.0, 2.0) ==
          imProcessDiffOfGaussianConvolve(src.c, dst.c, 1.0, 2.0));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("SplineEdgeConvolve")
  {
    CHECK(im::Process::SplineEdgeConvolve(src.plus, dst.plus) ==
          imProcessSplineEdgeConvolve(src.c, dst.c));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("Canny")
  {
    CHECK(im::Process::Canny(src.plus, dst.plus, 1.0) ==
          imProcessCanny(src.c, dst.c, 1.0));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("LocalMaxThreshold")
  {
    CHECK(im::Process::LocalMaxThreshold(src.plus, dst.plus, 5, 10) ==
          imProcessLocalMaxThreshold(src.c, dst.c, 5, 10));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("RangeContrastThreshold")
  {
    CHECK(im::Process::RangeContrastThreshold(src.plus, dst.plus, 5, 10) ==
          imProcessRangeContrastThreshold(src.c, dst.c, 5, 10));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("QuantizeGrayMedianCut")
  {
    im::Process::QuantizeGrayMedianCut(src.plus, dst.plus, 16);
    imProcessQuantizeGrayMedianCut(src.c, dst.c, 16);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ArithmeticConstOp")
  {
    /* The constant sits between the two images in the argument list rather
       than after them, which is unusual enough in this API to be worth a
       case of its own. */
    im::Process::ArithmeticConstOp(src.plus, 3.0, dst.plus, IM_BIN_MUL);
    imProcessArithmeticConstOp(src.c, 3.0, dst.c, IM_BIN_MUL);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("BackSub")
  {
    Pair background(IM_GRAY, IM_BYTE, 1);
    im::Process::BackSub(src.plus, background.plus, dst.plus, 10.0, 1);
    imProcessBackSub(src.c, background.c, dst.c, 10.0, 1);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("AbnormalHyperionCorrection")
  {
    /* The fifth argument is an output too -- the abnormal pixel map -- so
       both have to be compared, not just the image. */
    DstPair abnormal(IM_BINARY, IM_BYTE);
    im::Process::AbnormalHyperionCorrection(src.plus, dst.plus, 3, 50, abnormal.plus);
    imProcessAbnormalHyperionCorrection(src.c, dst.c, 3, 50, abnormal.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
    CHECK(same_data(abnormal.plus.GetHandle(), abnormal.c));
    CHECK(wrote_something(abnormal.c));
  }
  SUBCASE("CalcAutoGamma")
  {
    CHECK(im::Process::CalcAutoGamma(src.plus) == imProcessCalcAutoGamma(src.c));
  }
  SUBCASE("HysteresisThresEstimate")
  {
    /* Two out-parameters taken by reference and forwarded as addresses, so a
       swap here is invisible unless both are read back. */
    int plus_low = -1, plus_high = -1, c_low = -1, c_high = -1;
    im::Process::HysteresisThresEstimate(src.plus, plus_low, plus_high);
    imProcessHysteresisThresEstimate(src.c, &c_low, &c_high);
    CHECK(plus_low == c_low);
    CHECK(plus_high == c_high);
    CHECK(plus_low <= plus_high);
  }
  SUBCASE("LocalMaxThresEstimate")
  {
    int plus_level = -1, c_level = -1;
    im::Process::LocalMaxThresEstimate(src.plus, plus_level);
    imProcessLocalMaxThresEstimate(src.c, &c_level);
    CHECK(plus_level == c_level);
  }
}

TEST_CASE("plus: the OpenMP settings forward to their C functions")
{
  SUBCASE("OpenMPSetMinCount stores what it is given")
  {
    /* Process-wide, so it is put back. The return is the previous value, which
       is what makes restoring possible. */
    int previous = im::Process::OpenMPSetMinCount(1000);
    CHECK(im::Process::OpenMPSetMinCount(previous) == 1000);
  }

  SUBCASE("OpenMPSetNumThreads forwards, and does nothing in this build")
  {
    /* im_tests links the plain libim_process rather than libim_process_omp, so
       imProcessOpenMPSetNumThreads compiles to "return 1" and stores nothing.
       Asserting the round trip here would be asserting a property of the OMP
       build, which is not the one under test -- so what is checked is that the
       wrapper returns exactly what the C function does. */
    CHECK(im::Process::OpenMPSetNumThreads(2) == imProcessOpenMPSetNumThreads(2));
    CHECK(im::Process::OpenMPSetNumThreads(1) == imProcessOpenMPSetNumThreads(1));
  }
}

TEST_CASE("plus: the conversion wrappers forward their four trailing arguments")
{
  /* ConvertDataType and ConvertToBitmap each take cpx2real, gamma, absolute
     and cast_mode in that order -- four parameters of three different
     meanings, two of them ints that would transpose without complaint. */
  Pair src(IM_GRAY, IM_INT);

  SUBCASE("ConvertDataType")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::ConvertDataType(src.plus, dst.plus, IM_CPX_REAL, 0.0, 0, IM_CAST_MINMAX) ==
          imProcessConvertDataType(src.c, dst.c, IM_CPX_REAL, 0.0, 0, IM_CAST_MINMAX));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ConvertToBitmap")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::ConvertToBitmap(src.plus, dst.plus, IM_CPX_REAL, 0.0, 0, IM_CAST_MINMAX) ==
          imProcessConvertToBitmap(src.c, dst.c, IM_CPX_REAL, 0.0, 0, IM_CAST_MINMAX));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("DirectConv")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    im::Process::DirectConv(src.plus, dst.plus);
    imProcessDirectConv(src.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ConvertColorSpace")
  {
    Pair rgb(IM_RGB, IM_BYTE);
    DstPair gray(IM_GRAY, IM_BYTE);
    CHECK(im::Process::ConvertColorSpace(rgb.plus, gray.plus) ==
          imProcessConvertColorSpace(rgb.c, gray.c));
    CHECK(same_data(gray.plus.GetHandle(), gray.c));
    CHECK(wrote_something(gray.c));
  }
  SUBCASE("UnNormalize")
  {
    /* Source has to be real and in 0-1 for this to mean anything. */
    Pair normalized(IM_GRAY, IM_FLOAT, 0, 1);
    DstPair dst(IM_GRAY, IM_BYTE);
    im::Process::UnNormalize(normalized.plus, dst.plus);
    imProcessUnNormalize(normalized.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}

TEST_CASE("plus: the kernel-taking wrappers pass the kernel in the right place")
{
  /* The group with the most room for a transposition, since the kernel is
     another image and would be accepted in a source or destination position
     without complaint. */
  Pair src(IM_GRAY, IM_BYTE);
  DstPair dst(IM_GRAY, IM_BYTE);

  SUBCASE("ConvolveSep")
  {
    im::Image kernel = im::Kernel::Gaussian5x5();
    CHECK(im::Process::ConvolveSep(src.plus, dst.plus, kernel) ==
          imProcessConvolveSep(src.c, dst.c, kernel.GetHandle()));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ConvolveRep")
  {
    im::Image kernel = im::Kernel::Mean3x3();
    CHECK(im::Process::ConvolveRep(src.plus, dst.plus, kernel, 3) ==
          imProcessConvolveRep(src.c, dst.c, kernel.GetHandle(), 3));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ConvolveDual")
  {
    /* Two kernels, and the operation takes the magnitude of both results, so
       swapping them would pass. They are made different in shape rather than
       in orientation so that the comparison against C still pins the order. */
    im::Image kernel1 = im::Kernel::Sobel();
    im::Image kernel2 = im::Kernel::Prewitt();
    CHECK(im::Process::ConvolveDual(src.plus, dst.plus, kernel1, kernel2) ==
          imProcessConvolveDual(src.c, dst.c, kernel1.GetHandle(), kernel2.GetHandle()));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("CompassConvolve")
  {
    im::Image kernel = im::Kernel::Kirsh();
    CHECK(im::Process::CompassConvolve(src.plus, dst.plus, kernel) ==
          imProcessCompassConvolve(src.c, dst.c, kernel.GetHandle()));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("SharpKernel")
  {
    /* Note the argument order: the kernel comes SECOND here, before the
       destination, unlike every other kernel operation in this group. A
       wrapper written by pattern-matching the others would have it wrong. */
    im::Image kernel = im::Kernel::Laplacian4();
    CHECK(im::Process::SharpKernel(src.plus, kernel, dst.plus, 0.5, 0.0) ==
          imProcessSharpKernel(src.c, kernel.GetHandle(), dst.c, 0.5, 0.0));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("GrayMorphConvolve")
  {
    im::Image kernel(3, 3, IM_GRAY, IM_INT);
    imImage* c_kernel = imImageCreate(3, 3, IM_GRAY, IM_INT);
    REQUIRE(c_kernel != NULL);
    for (int i = 0; i < 9; i++)
    {
      ((int*)kernel.GetHandle()->data[0])[i] = (i == 4)? 0: -1;
      ((int*)c_kernel->data[0])[i] = (i == 4)? 0: -1;
    }
    CHECK(im::Process::GrayMorphConvolve(src.plus, dst.plus, kernel, 1) ==
          imProcessGrayMorphConvolve(src.c, dst.c, kernel.GetHandle(), 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
    imImageDestroy(c_kernel);
  }
  SUBCASE("RotateKernel")
  {
    /* In place on the kernel itself, so the comparison is between two kernels
       rotated the same number of times. */
    im::Image kernel = im::Kernel::Sobel();
    imImage* c_kernel = imKernelSobel();
    REQUIRE(c_kernel != NULL);
    im::Process::RotateKernel(kernel);
    imProcessRotateKernel(c_kernel);
    CHECK(same_data(kernel.GetHandle(), c_kernel));
    imImageDestroy(c_kernel);
  }
}

TEST_CASE("plus: the binary-image wrappers forward to their C functions")
{
  Pair src(IM_BINARY, IM_BYTE);
  DstPair dst(IM_BINARY, IM_BYTE);

  SUBCASE("FillHoles")
  {
    CHECK(im::Process::FillHoles(src.plus, dst.plus, 4) ==
          imProcessFillHoles(src.c, dst.c, 4));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("RemoveByArea")
  {
    /* Four ints in a row -- connect, start_size, end_size, inside -- which is
       the worst case for a silent transposition. */
    CHECK(im::Process::RemoveByArea(src.plus, dst.plus, 4, 2, 20, 1) ==
          imProcessRemoveByArea(src.c, dst.c, 4, 2, 20, 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("PerimeterLine")
  {
    CHECK(im::Process::PerimeterLine(src.plus, dst.plus) ==
          imProcessPerimeterLine(src.c, dst.c));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("BinThinNhMaps")
  {
    /* This one never returned before: its pass loop stops when a pass deletes
       nothing, but the neighborhood map at the left edge marked already-zero
       pixels as deletable and each was counted as a deletion, so the count
       never reached zero. Reaching the assertions below is the result -- and
       the three that follow the comparison are what say the fix did not just
       stop the loop early, since a thinning that halted at the wrong moment
       would still forward identically to C.

       Not behind a timeout, deliberately: if this regresses, a test that hangs
       is a clearer signal than one that fails. */
    CHECK(im::Process::BinThinNhMaps(src.plus, dst.plus) ==
          imProcessBinThinNhMaps(src.c, dst.c));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));

    int set_in = 0, set_out = 0, appeared = 0, out_of_range = 0;
    for (int i = 0; i < src.c->count; i++)
    {
      imbyte before = ((imbyte*)src.c->data[0])[i];
      imbyte after = ((imbyte*)dst.c->data[0])[i];
      set_in += before? 1: 0;
      set_out += after? 1: 0;
      if (after && !before) appeared++;
      if (after > 1) out_of_range++;
    }
    CHECK(appeared == 0);                 /* thinning only ever removes */
    CHECK(out_of_range == 0);             /* and leaves a binary image  */
    CHECK(set_out > 0);                   /* without erasing everything */
    CHECK(set_out < set_in);              /* but it did thin something  */

    /* And it had really reached a fixed point: thinning the skeleton again
       removes nothing. A loop stopped one pass too early would fail this. */
    DstPair again(IM_BINARY, IM_BYTE);
    REQUIRE(imProcessBinThinNhMaps(dst.c, again.c) != 0);
    CHECK(same_data(again.c, dst.c));
  }
  SUBCASE("BinMorphConvolve")
  {
    im::Image kernel(3, 3, IM_GRAY, IM_INT);
    for (int i = 0; i < 9; i++) ((int*)kernel.GetHandle()->data[0])[i] = 1;
    CHECK(im::Process::BinMorphConvolve(src.plus, dst.plus, kernel, 1, 2) ==
          imProcessBinMorphConvolve(src.c, dst.c, kernel.GetHandle(), 1, 2));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("BinaryMask")
  {
    /* The mask is the third argument and the destination the second, which is
       the reverse of what the name order suggests. */
    Pair image(IM_GRAY, IM_BYTE, 1);
    DstPair masked(IM_GRAY, IM_BYTE);
    im::Process::BinaryMask(image.plus, masked.plus, src.plus);
    imProcessBinaryMask(image.c, masked.c, src.c);
    CHECK(same_data(masked.plus.GetHandle(), masked.c));
    CHECK(wrote_something(masked.c));
  }
  SUBCASE("DistanceTransform")
  {
    DstPair distance(IM_GRAY, IM_FLOAT);
    im::Process::DistanceTransform(src.plus, distance.plus);
    imProcessDistanceTransform(src.c, distance.c);
    CHECK(same_data(distance.plus.GetHandle(), distance.c));
    CHECK(wrote_something(distance.c));
  }
  SUBCASE("RegionalMaximum")
  {
    /* Takes the distance transform's output, so it needs a float source and a
       binary target -- the opposite way round from DistanceTransform. */
    Pair distance(IM_GRAY, IM_FLOAT);
    DstPair maxima(IM_BINARY, IM_BYTE);
    im::Process::RegionalMaximum(distance.plus, maxima.plus);
    imProcessRegionalMaximum(distance.c, maxima.c);
    CHECK(same_data(maxima.plus.GetHandle(), maxima.c));
    CHECK(wrote_something(maxima.c));
  }
}

TEST_CASE("plus: the geometric wrappers forward their offsets and orders")
{
  Pair src(IM_GRAY, IM_BYTE);

  SUBCASE("AddMargins")
  {
    /* The destination has to be big enough for the source plus the margins,
       so it is a different size from the source -- which also means a wrapper
       that passed the two images the wrong way round could not succeed. */
    DstPair dst(IM_GRAY, IM_BYTE, BW + 4, BH + 6);
    CHECK(im::Process::AddMargins(src.plus, dst.plus, 2, 3) ==
          imProcessAddMargins(src.c, dst.c, 2, 3));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("Insert")
  {
    Pair region(IM_GRAY, IM_BYTE, 1);
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::Insert(src.plus, region.plus, dst.plus, 2, 3) ==
          imProcessInsert(src.c, region.c, dst.c, 2, 3));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("RotateRef")
  {
    /* Eight arguments, six of them numbers: cos, sin, x, y, to_origin,
       order. The cos and sin are given rather than an angle, so passing them
       in the wrong order rotates the other way by the complementary angle --
       a plausible image, and a wrong one. */
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::RotateRef(src.plus, dst.plus, 0.8, 0.6, 4, 5, 1, 1) ==
          imProcessRotateRef(src.c, dst.c, 0.8, 0.6, 4, 5, 1, 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("Radial")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::Radial(src.plus, dst.plus, 0.1, 1) ==
          imProcessRadial(src.c, dst.c, 0.1, 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("Swirl")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::Swirl(src.plus, dst.plus, 0.5, 1) ==
          imProcessSwirl(src.c, dst.c, 0.5, 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("LensDistort")
  {
    /* Three coefficients that are not interchangeable and an order after
       them. */
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::LensDistort(src.plus, dst.plus, 0.1, 0.02, 0.003, 1) ==
          imProcessLensDistort(src.c, dst.c, 0.1, 0.02, 0.003, 1));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("InterlaceSplit")
  {
    /* Two destinations, each half the height, and the odd and even fields are
       different -- so a wrapper that passed them the wrong way round is
       caught by comparing both. */
    DstPair odd(IM_GRAY, IM_BYTE, BW, BH/2);
    DstPair even(IM_GRAY, IM_BYTE, BW, BH/2);
    CHECK(im::Process::InterlaceSplit(src.plus, odd.plus, even.plus) ==
          imProcessInterlaceSplit(src.c, odd.c, even.c));
    CHECK(same_data(odd.plus.GetHandle(), odd.c));
    CHECK(wrote_something(odd.c));
    CHECK(same_data(even.plus.GetHandle(), even.c));
    CHECK(wrote_something(even.c));
    CHECK(!same_data(odd.plus.GetHandle(), even.plus.GetHandle()));
  }
}

TEST_CASE("plus: the colour wrappers forward to their C functions")
{
  Pair rgb(IM_RGB, IM_BYTE);

  SUBCASE("SelectHue")
  {
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::SelectHue(rgb.plus, dst.plus, 60.0, 180.0);
    imProcessSelectHue(rgb.c, dst.c, 60.0, 180.0);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("SelectHSI")
  {
    /* Six bounds in three pairs. Passing them as three ranges rather than as
       start-then-end throughout would type-check and select a different
       region entirely. */
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::SelectHSI(rgb.plus, dst.plus, 60.0, 180.0, 0.2, 0.9, 0.1, 0.8);
    imProcessSelectHSI(rgb.c, dst.c, 60.0, 180.0, 0.2, 0.9, 0.1, 0.8);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ShiftHSI")
  {
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::ShiftHSI(rgb.plus, dst.plus, 30.0, 0.1, -0.05);
    imProcessShiftHSI(rgb.c, dst.c, 30.0, 0.1, -0.05);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ShiftComponent")
  {
    /* Same shape of call as ShiftHSI and a different operation, which is why
       both are here: a wrapper wired to the wrong one of the two would look
       right at every call site, and the comparison against C below only
       catches that if the two C functions actually disagree on these
       arguments. They do not always -- with whole-number shifts like
       (10, 20, 30) both saturate and the outputs are identical over the whole
       image, which is why the shifts here are the fractional ones ShiftHSI is
       meant for. Measured: they differ at 560 of 576 samples. */
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::ShiftComponent(rgb.plus, dst.plus, 30.0, 0.1, -0.05);
    imProcessShiftComponent(rgb.c, dst.c, 30.0, 0.1, -0.05);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));

    DstPair as_hsi(IM_RGB, IM_BYTE);
    imProcessShiftHSI(rgb.c, as_hsi.c, 30.0, 0.1, -0.05);
    CHECK(!same_data(dst.c, as_hsi.c));
  }
  SUBCASE("SplitHSI and MergeHSI round-trip through each other")
  {
    /* The HSI planes are produced rather than invented, so MergeHSI gets
       inputs in the ranges it expects -- hue in degrees, the other two in
       0-1. Three targets of the same type, so the split is where a
       transposition hides, and the merge takes them back in the same order. */
    DstPair h(IM_GRAY, IM_FLOAT), s(IM_GRAY, IM_FLOAT), i(IM_GRAY, IM_FLOAT);

    im::Process::SplitHSI(rgb.plus, h.plus, s.plus, i.plus);
    imProcessSplitHSI(rgb.c, h.c, s.c, i.c);
    CHECK(same_data(h.plus.GetHandle(), h.c));
    CHECK(wrote_something(h.c));
    CHECK(same_data(s.plus.GetHandle(), s.c));
    CHECK(wrote_something(s.c));
    CHECK(same_data(i.plus.GetHandle(), i.c));
    CHECK(wrote_something(i.c));

    /* The three planes are different from each other, so comparing all three
       is a real check on the order rather than three copies of one answer. */
    CHECK(!same_data(h.c, s.c));
    CHECK(!same_data(s.c, i.c));

    DstPair back(IM_RGB, IM_BYTE);
    im::Process::MergeHSI(h.plus, s.plus, i.plus, back.plus);
    imProcessMergeHSI(h.c, s.c, i.c, back.c);
    CHECK(same_data(back.plus.GetHandle(), back.c));
    CHECK(wrote_something(back.c));
  }
  SUBCASE("SplitYChroma")
  {
    /* Two targets of different shapes -- gray luma, RGB chroma -- so this one
       could not compile with them swapped, but the values still have to line
       up with C. */
    DstPair luma(IM_GRAY, IM_BYTE);
    DstPair chroma(IM_RGB, IM_BYTE);
    im::Process::SplitYChroma(rgb.plus, luma.plus, chroma.plus);
    imProcessSplitYChroma(rgb.c, luma.c, chroma.c);
    CHECK(same_data(luma.plus.GetHandle(), luma.c));
    CHECK(wrote_something(luma.c));
    CHECK(same_data(chroma.plus.GetHandle(), chroma.c));
    CHECK(wrote_something(chroma.c));
  }
  SUBCASE("ThresholdColor")
  {
    /* The colour is an array with one entry per component, so it is the
       argument most likely to be passed with the wrong count somewhere. */
    DstPair dst(IM_BINARY, IM_BYTE);
    double plus_color[3] = { 100.0, 120.0, 140.0 };
    double c_color[3] = { 100.0, 120.0, 140.0 };
    im::Process::ThresholdColor(rgb.plus, dst.plus, plus_color, 30.0);
    imProcessThresholdColor(rgb.c, dst.c, c_color, 30.0);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("ThresholdSaturation")
  {
    DstPair dst(IM_BINARY, IM_BYTE);
    im::Process::ThresholdSaturation(rgb.plus, dst.plus, 0.3);
    imProcessThresholdSaturation(rgb.c, dst.c, 0.3);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("PseudoColor")
  {
    Pair gray(IM_GRAY, IM_BYTE);
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::PseudoColor(gray.plus, dst.plus);
    imProcessPseudoColor(gray.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("QuantizeRGBMedianCut")
  {
    /* Target is IM_MAP, so the palette is part of the answer and comparing
       the samples alone would miss a wrapper that dropped it. */
    DstPair dst(IM_MAP, IM_BYTE);
    im::Process::QuantizeRGBMedianCut(rgb.plus, dst.plus);
    imProcessQuantizeRGBMedianCut(rgb.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
    CHECK(dst.plus.GetHandle()->palette_count == dst.c->palette_count);
    CHECK(memcmp(dst.plus.GetHandle()->palette, dst.c->palette,
                 (size_t)dst.c->palette_count * sizeof(long)) == 0);
  }
  SUBCASE("NormDiffRatio")
  {
    Pair a(IM_GRAY, IM_BYTE), b(IM_GRAY, IM_BYTE, 1);
    DstPair dst(IM_GRAY, IM_FLOAT);
    im::Process::NormDiffRatio(a.plus, b.plus, dst.plus);
    imProcessNormDiffRatio(a.c, b.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}

TEST_CASE("plus: the alpha wrappers forward to their C functions")
{
  SUBCASE("Compose")
  {
    /* Both sources need an alpha channel for the OVER operator to mean
       anything, and same_data compares the alpha plane too. */
    Pair a(IM_RGB|IM_ALPHA, IM_BYTE), b(IM_RGB|IM_ALPHA, IM_BYTE, 1);
    DstPair dst(IM_RGB|IM_ALPHA, IM_BYTE);
    im::Process::Compose(a.plus, b.plus, dst.plus);
    imProcessCompose(a.c, b.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("Blend")
  {
    /* Four images, and the alpha one sits third rather than last -- the
       position a reader would expect the destination to be in. */
    Pair a(IM_RGB, IM_BYTE), b(IM_RGB, IM_BYTE, 1), alpha(IM_GRAY, IM_BYTE, 2);
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::Blend(a.plus, b.plus, alpha.plus, dst.plus);
    imProcessBlend(a.c, b.c, alpha.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("BlendConst")
  {
    Pair a(IM_RGB, IM_BYTE), b(IM_RGB, IM_BYTE, 1);
    DstPair dst(IM_RGB, IM_BYTE);
    im::Process::BlendConst(a.plus, b.plus, dst.plus, 0.25);
    imProcessBlendConst(a.c, b.c, dst.c, 0.25);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));

    /* 0.25 rather than 0.5: a symmetric blend is the same with the two
       sources exchanged, so it could not catch them being swapped. */
    DstPair swapped(IM_RGB, IM_BYTE);
    imProcessBlendConst(b.c, a.c, swapped.c, 0.25);
    CHECK(!same_data(dst.c, swapped.c));
  }
  SUBCASE("SetAlphaColor")
  {
    Pair src(IM_RGB, IM_BYTE);
    DstPair dst(IM_RGB|IM_ALPHA, IM_BYTE);

    /* The colour is read out of the source rather than invented: this sets
       alpha only where the colour actually occurs and leaves the rest alone,
       so a colour that appears nowhere makes the whole call a no-op -- and two
       no-ops compare equal, which is the pass this case is written to avoid. */
    double plus_color[3], c_color[3];
    for (int p = 0; p < 3; p++)
    {
      plus_color[p] = (double)((imbyte*)src.c->data[p])[5];
      c_color[p] = plus_color[p];
    }

    im::Process::SetAlphaColor(src.plus, dst.plus, plus_color, 128.0);
    imProcessSetAlphaColor(src.c, dst.c, c_color, 128.0);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}

TEST_CASE("plus: the complex wrappers forward to their C functions")
{
  SUBCASE("SplitComplex and MergeComplex, both readings")
  {
    /* The trailing flag chooses real/imaginary or magnitude/phase, so it is
       run both ways: a wrapper that dropped it would agree with C on
       whichever value the C side defaulted to and disagree on the other. */
    for (int polar = 0; polar <= 1; polar++)
    {
      CAPTURE(polar);
      Pair src(IM_GRAY, IM_CFLOAT);
      DstPair first(IM_GRAY, IM_FLOAT), second(IM_GRAY, IM_FLOAT);

      im::Process::SplitComplex(src.plus, first.plus, second.plus, polar);
      imProcessSplitComplex(src.c, first.c, second.c, polar);
      CHECK(same_data(first.plus.GetHandle(), first.c));
      CHECK(wrote_something(first.c));
      CHECK(same_data(second.plus.GetHandle(), second.c));
      CHECK(wrote_something(second.c));

      DstPair merged(IM_GRAY, IM_CFLOAT);
      im::Process::MergeComplex(first.plus, second.plus, merged.plus, polar);
      imProcessMergeComplex(first.c, second.c, merged.c, polar);
      CHECK(same_data(merged.plus.GetHandle(), merged.c));
      CHECK(wrote_something(merged.c));
    }
  }
  SUBCASE("MultiplyConj")
  {
    /* Conj(a)*b, so it is not symmetric and swapping the sources conjugates
       the other one -- checked below, since otherwise the comparison against
       C would pass either way round. */
    Pair a(IM_GRAY, IM_CFLOAT), b(IM_GRAY, IM_CFLOAT, 1);
    DstPair dst(IM_GRAY, IM_CFLOAT);
    im::Process::MultiplyConj(a.plus, b.plus, dst.plus);
    imProcessMultiplyConj(a.c, b.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));

    DstPair swapped(IM_GRAY, IM_CFLOAT);
    imProcessMultiplyConj(b.c, a.c, swapped.c);
    CHECK(!same_data(dst.c, swapped.c));
  }
  SUBCASE("AutoCovariance")
  {
    Pair src(IM_GRAY, IM_BYTE), mean(IM_GRAY, IM_BYTE, 1);
    DstPair dst(IM_GRAY, IM_FLOAT);
    CHECK(im::Process::AutoCovariance(src.plus, mean.plus, dst.plus) ==
          imProcessAutoCovariance(src.c, mean.c, dst.c));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}

#ifdef IM_TEST_HAS_FFTW3
TEST_CASE("plus: the Fourier wrappers forward to their C functions")
{
  /* Guarded the same way test_transform.cpp guards its Fourier cases: these
     six live in libim_fftw3, which is its own build option, and referencing
     them without it fails to link rather than failing a test. */
  SUBCASE("FFT and IFFT")
  {
    Pair src(IM_GRAY, IM_FLOAT);
    DstPair frequency(IM_GRAY, IM_CFLOAT);
    im::Process::FFT(src.plus, frequency.plus);
    imProcessFFT(src.c, frequency.c);
    CHECK(same_data(frequency.plus.GetHandle(), frequency.c));
    CHECK(wrote_something(frequency.c));

    DstPair back(IM_GRAY, IM_CFLOAT);
    im::Process::IFFT(frequency.plus, back.plus);
    imProcessIFFT(frequency.c, back.c);
    CHECK(same_data(back.plus.GetHandle(), back.c));
    CHECK(wrote_something(back.c));
  }
  SUBCASE("FFTraw forwards its three flags")
  {
    /* In place, and the three flags are inverse, center and normalize in that
       order -- three ints that would transpose silently. */
    Pair image(IM_GRAY, IM_CFLOAT);
    im::Process::FFTraw(image.plus, 0, 1, 2);
    imProcessFFTraw(image.c, 0, 1, 2);
    CHECK(same_data(image.plus.GetHandle(), image.c));
    CHECK(wrote_something(image.c));
  }
  SUBCASE("SwapQuadrants")
  {
    Pair image(IM_GRAY, IM_CFLOAT);
    im::Process::SwapQuadrants(image.plus, 1);
    imProcessSwapQuadrants(image.c, 1);
    CHECK(same_data(image.plus.GetHandle(), image.c));
    CHECK(wrote_something(image.c));
  }
  SUBCASE("CrossCorrelation and AutoCorrelation")
  {
    Pair a(IM_GRAY, IM_FLOAT), b(IM_GRAY, IM_FLOAT, 1);
    DstPair cross(IM_GRAY, IM_CFLOAT);
    im::Process::CrossCorrelation(a.plus, b.plus, cross.plus);
    imProcessCrossCorrelation(a.c, b.c, cross.c);
    CHECK(same_data(cross.plus.GetHandle(), cross.c));
    CHECK(wrote_something(cross.c));

    DstPair aut(IM_GRAY, IM_CFLOAT);
    im::Process::AutoCorrelation(a.plus, aut.plus);
    imProcessAutoCorrelation(a.c, aut.c);
    CHECK(same_data(aut.plus.GetHandle(), aut.c));
    CHECK(wrote_something(aut.c));
  }
}
#endif /* IM_TEST_HAS_FFTW3 */

TEST_CASE("plus: the Hough wrappers forward to their C functions")
{
  /* The accumulator's size is dictated by the header: IM_GRAY, IM_INT, 180
     wide and 2*rmax+1 tall. Getting it wrong is refused rather than
     tolerated, so building it correctly is part of the case. */
  const int rmax = (int)sqrt((double)(BW*BW + BH*BH));
  Pair src(IM_BINARY, IM_BYTE);
  DstPair hough(IM_GRAY, IM_INT, 180, 2*rmax + 1);

  CHECK(im::Process::HoughLines(src.plus, hough.plus) ==
        imProcessHoughLines(src.c, hough.c));
  CHECK(same_data(hough.plus.GetHandle(), hough.c));
  CHECK(wrote_something(hough.c));

  SUBCASE("HoughLinesDraw takes the accumulator and its thresholded peaks")
  {
    /* Four images, three of them inputs of three different kinds: the
       original, the accumulator, and the accumulator thresholded to a binary
       peak map. Nothing about the types prevents the middle two from being
       exchanged, which is what this compares against C for. RGB targets so
       the operation draws into the red plane rather than converting a gray
       target to IM_MAP under us. */
    DstPair peaks(IM_BINARY, IM_BYTE, 180, 2*rmax + 1);
    REQUIRE(imProcessLocalMaxThreshold(hough.c, peaks.c, 5, 1) != 0);
    memcpy(peaks.plus.GetHandle()->data[0], peaks.c->data[0], (size_t)peaks.c->count);

    Pair picture(IM_RGB, IM_BYTE);
    DstPair drawn_plus(IM_RGB, IM_BYTE), drawn_c(IM_RGB, IM_BYTE);

    int plus_lines = im::Process::HoughLinesDraw(picture.plus, hough.plus,
                                                 peaks.plus, drawn_plus.plus);
    int c_lines = imProcessHoughLinesDraw(picture.c, hough.c, peaks.c, drawn_c.c);
    CHECK(plus_lines == c_lines);
    CHECK(same_data(drawn_plus.plus.GetHandle(), drawn_c.c));
    CHECK(wrote_something(drawn_c.c));
  }
}

TEST_CASE("plus: ZeroCrossing forwards to its C function")
{
  /* Needs a signed or real source -- it looks for sign changes, which a byte
     image cannot have.

     This case found a heap overflow rather than a wrapper problem. The
     per-line code advanced its offsets past the last pixel of the row before
     handling it, so it read one element beyond the source on the final row and
     wrote column 0 of the next row in place of the last column; the same slip
     at the end of the last line wrote one element past the destination. The
     three assertions after the comparison are the ones that fail on the old
     code -- and the comparison against C is not among them, because both sides
     were wrong in the same way. */
  Pair src(IM_GRAY, IM_FLOAT);
  DstPair dst(IM_GRAY, IM_FLOAT);
  CHECK(im::Process::ZeroCrossing(src.plus, dst.plus) ==
        imProcessZeroCrossing(src.c, dst.c));
  CHECK(same_data(dst.plus.GetHandle(), dst.c));
  CHECK(wrote_something(dst.c));

  /* Every pixel of the last column was written. It used to be skipped
     entirely, leaving the destination's own contents there. */
  int last_column_untouched = 0;
  for (int y = 0; y < BH; y++)
  {
    float value = ((float*)dst.c->data[0])[y*BW + (BW - 1)];
    unsigned char raw[4];
    memcpy(raw, &value, 4);
    if (raw[0] == 0x5A && raw[1] == 0x5A && raw[2] == 0x5A && raw[3] == 0x5A)
      last_column_untouched++;
  }
  CHECK(last_column_untouched == 0);

  /* And the answer does not depend on what was in the heap next door: the
     out-of-bounds read made this differ between two runs on the same input,
     which is how the overflow first showed up. */
  DstPair again(IM_GRAY, IM_FLOAT);
  REQUIRE(imProcessZeroCrossing(src.c, again.c) != 0);
  CHECK(same_data(again.c, dst.c));
}


/* ------------------------------------------------------------------ *
 * The wrappers that take a function pointer or an image array. These
 * are the only ones in the namespace that do more than forward -- the
 * three Multiple* and two MultiPoint* wrappers each build a C array of
 * handles and free it -- so they are the ones where something other
 * than argument order can go wrong, and MultipleMedian is where it
 * had: its delete[] sat after the return.
 * ------------------------------------------------------------------ */

namespace {

/* Deterministic and dependent on every argument it is given, so a wrapper that
   passed the coordinates in the wrong order would produce a different image
   rather than the same one. */
double render_ramp(int x, int y, int d, double* params)
{
  return params[0] * x + params[1] * y + 10.0 * d;
}

double render_cond_ramp(int x, int y, int d, int* cond, double* params)
{
  *cond = ((x + y) % 3 != 0)? 1: 0;
  return params[0] * x + params[1] * y + 10.0 * d;
}

int unary_point(double src_value, double* dst_value, double* params,
                void* userdata, int x, int y, int d)
{
  (void)userdata;
  *dst_value = src_value * params[0] + x + 2*y + 3*d;
  return 1;
}

int unary_point_color(const double* src_value, double* dst_value, double* params,
                      void* userdata, int x, int y)
{
  (void)userdata;
  dst_value[0] = src_value[0] * params[0] + x - y;
  return 1;
}

int multi_point(const double* src_value, double* dst_value, double* params,
                void* userdata, int x, int y, int d, int src_image_count)
{
  (void)userdata;
  double total = 0;
  for (int i = 0; i < src_image_count; i++) total += src_value[i];
  *dst_value = total * params[0] + x + 2*y + 3*d;
  return 1;
}

int multi_point_color(double* src_value, double* dst_value, double* params,
                      void* userdata, int x, int y, int src_image_count,
                      int src_depth, int dst_depth)
{
  (void)userdata; (void)dst_depth;
  double total = 0;
  for (int i = 0; i < src_image_count * src_depth; i++) total += src_value[i];
  dst_value[0] = total * params[0] + x - y;
  return 1;
}

} /* namespace */

TEST_CASE("plus: the render wrappers forward their callbacks")
{
  double plus_params[2] = { 1.5, 2.5 };
  double c_params[2] = { 1.5, 2.5 };

  SUBCASE("RenderOp")
  {
    /* The trailing "plus" flag decides whether the result is added to what is
       already there, so the image is seeded rather than blank and the flag is
       set -- with a blank image and plus=0 the flag would make no difference
       and dropping it would go unnoticed. */
    Pair image(IM_GRAY, IM_BYTE);
    imImage* c_copy = imImageDuplicate(image.c);
    REQUIRE(c_copy != NULL);

    CHECK(im::Process::RenderOp(image.plus, render_ramp, "ramp", plus_params, 1) ==
          imProcessRenderOp(c_copy, render_ramp, "ramp", c_params, 1));
    CHECK(same_data(image.plus.GetHandle(), c_copy));
    CHECK(!same_data(image.plus.GetHandle(), image.c));   /* it did render */
    imImageDestroy(c_copy);
  }
  SUBCASE("RenderOpAlpha")
  {
    Pair image(IM_RGB|IM_ALPHA, IM_BYTE);
    imImage* c_copy = imImageDuplicate(image.c);
    REQUIRE(c_copy != NULL);
    CHECK(im::Process::RenderOpAlpha(image.plus, render_ramp, "ramp", plus_params, 1) ==
          imProcessRenderOpAlpha(c_copy, render_ramp, "ramp", c_params, 1));
    CHECK(same_data(image.plus.GetHandle(), c_copy));
    imImageDestroy(c_copy);
  }
  SUBCASE("RenderCondOp")
  {
    Pair image(IM_GRAY, IM_BYTE);
    imImage* c_copy = imImageDuplicate(image.c);
    REQUIRE(c_copy != NULL);
    CHECK(im::Process::RenderCondOp(image.plus, render_cond_ramp, "cond", plus_params) ==
          imProcessRenderCondOp(c_copy, render_cond_ramp, "cond", c_params));
    CHECK(same_data(image.plus.GetHandle(), c_copy));
    imImageDestroy(c_copy);
  }
  SUBCASE("RenderCondOpAlpha")
  {
    Pair image(IM_RGB|IM_ALPHA, IM_BYTE);
    imImage* c_copy = imImageDuplicate(image.c);
    REQUIRE(c_copy != NULL);
    CHECK(im::Process::RenderCondOpAlpha(image.plus, render_cond_ramp, "cond", plus_params) ==
          imProcessRenderCondOpAlpha(c_copy, render_cond_ramp, "cond", c_params));
    CHECK(same_data(image.plus.GetHandle(), c_copy));
    imImageDestroy(c_copy);
  }
  SUBCASE("RenderChessboard")
  {
    DstPair image(IM_GRAY, IM_BYTE);
    /* Different spacings, so a wrapper that passed one twice is caught. */
    CHECK(im::Process::RenderChessboard(image.plus, 2, 5) ==
          imProcessRenderChessboard(image.c, 2, 5));
    CHECK(same_data(image.plus.GetHandle(), image.c));
    CHECK(wrote_something(image.c));

    DstPair transposed(IM_GRAY, IM_BYTE);
    imProcessRenderChessboard(transposed.c, 5, 2);
    CHECK(!same_data(image.c, transposed.c));
  }
  SUBCASE("RenderLapOfGaussian")
  {
    DstPair image(IM_GRAY, IM_FLOAT);
    CHECK(im::Process::RenderLapOfGaussian(image.plus, 2.0) ==
          imProcessRenderLapOfGaussian(image.c, 2.0));
    CHECK(same_data(image.plus.GetHandle(), image.c));
    CHECK(wrote_something(image.c));
  }
  SUBCASE("RenderFloodFill")
  {
    /* The start point is x then y, and the image is not square, so passing
       them the other way round reaches a different pixel. */
    Pair image(IM_RGB, IM_BYTE);
    double plus_color[3] = { 10.0, 20.0, 30.0 };
    double c_color[3] = { 10.0, 20.0, 30.0 };
    im::Process::RenderFloodFill(image.plus, 3, 7, plus_color, 40.0);
    imProcessRenderFloodFill(image.c, 3, 7, c_color, 40.0);
    CHECK(same_data(image.plus.GetHandle(), image.c));
    CHECK(wrote_something(image.c));
  }
}

TEST_CASE("plus: the point-operation wrappers forward their callbacks")
{
  double plus_params[1] = { 2.0 };
  double c_params[1] = { 2.0 };

  SUBCASE("UnaryPointOp")
  {
    Pair src(IM_GRAY, IM_BYTE);
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::UnaryPointOp(src.plus, dst.plus, unary_point,
                                    plus_params, NULL, "unary") ==
          imProcessUnaryPointOp(src.c, dst.c, unary_point, c_params, NULL, "unary"));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("UnaryPointColorOp")
  {
    Pair src(IM_RGB, IM_BYTE);
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::UnaryPointColorOp(src.plus, dst.plus, unary_point_color,
                                         plus_params, NULL, "unary color") ==
          imProcessUnaryPointColorOp(src.c, dst.c, unary_point_color, c_params,
                                     NULL, "unary color"));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("MultiPointOp")
  {
    /* One of the two wrappers that allocates: it copies the handles out of the
       C++ array into a C one, so a wrong index or a missed element shows up as
       a different result rather than a crash. Three sources, all different. */
    im::Image a(BW, BH, IM_GRAY, IM_BYTE);
    im::Image b(BW, BH, IM_GRAY, IM_BYTE);
    im::Image c(BW, BH, IM_GRAY, IM_BYTE);
    REQUIRE(!a.Failed()); REQUIRE(!b.Failed()); REQUIRE(!c.Failed());
    seed(a.GetHandle());
    for (int i = 0; i < a.GetHandle()->count; i++)
    {
      ((imbyte*)b.GetHandle()->data[0])[i] = (imbyte)((i * 3 + 5) & 0x7F);
      ((imbyte*)c.GetHandle()->data[0])[i] = (imbyte)((i * 11 + 9) & 0x7F);
    }

    im::Image list[3] = { a, b, c };
    const imImage* c_list[3] = { a.GetHandle(), b.GetHandle(), c.GetHandle() };

    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::MultiPointOp(list, 3, dst.plus, multi_point,
                                    plus_params, NULL, "multi") ==
          imProcessMultiPointOp(c_list, 3, dst.c, multi_point, c_params, NULL, "multi"));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
  SUBCASE("MultiPointColorOp")
  {
    im::Image a(BW, BH, IM_RGB, IM_BYTE);
    im::Image b(BW, BH, IM_RGB, IM_BYTE);
    REQUIRE(!a.Failed()); REQUIRE(!b.Failed());
    seed(a.GetHandle());
    for (int p = 0; p < 3; p++)
      for (int i = 0; i < b.GetHandle()->count; i++)
        ((imbyte*)b.GetHandle()->data[p])[i] = (imbyte)((i * 5 + p * 17) & 0x7F);

    im::Image list[2] = { a, b };
    const imImage* c_list[2] = { a.GetHandle(), b.GetHandle() };

    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::MultiPointColorOp(list, 2, dst.plus, multi_point_color,
                                         plus_params, NULL, "multi color") ==
          imProcessMultiPointColorOp(c_list, 2, dst.c, multi_point_color, c_params,
                                     NULL, "multi color"));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}

TEST_CASE("plus: the multiple-image wrappers forward their arrays")
{
  /* Three images that differ, so a mean, a median and a standard deviation
     are all distinguishable from each other and from any one input. */
  im::Image a(BW, BH, IM_GRAY, IM_BYTE);
  im::Image b(BW, BH, IM_GRAY, IM_BYTE);
  im::Image c(BW, BH, IM_GRAY, IM_BYTE);
  REQUIRE(!a.Failed()); REQUIRE(!b.Failed()); REQUIRE(!c.Failed());
  seed(a.GetHandle());
  for (int i = 0; i < a.GetHandle()->count; i++)
  {
    ((imbyte*)b.GetHandle()->data[0])[i] = (imbyte)((i * 3 + 5) & 0x7F);
    ((imbyte*)c.GetHandle()->data[0])[i] = (imbyte)((i * 11 + 9) & 0x7F);
  }

  im::Image list[3] = { a, b, c };
  const imImage* c_list[3] = { a.GetHandle(), b.GetHandle(), c.GetHandle() };

  SUBCASE("MultipleMean")
  {
    DstPair dst(IM_GRAY, IM_BYTE);
    im::Process::MultipleMean(list, 3, dst.plus);
    imProcessMultipleMean(c_list, 3, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
    CHECK(!same_data(dst.c, a.GetHandle()));    /* and it combined them */
  }
  SUBCASE("MultipleMedian")
  {
    /* The one that leaked. Its delete[] was written after the return, so the
       array of handles was abandoned on every call -- invisible here on macOS,
       where there is no LeakSanitizer, but caught by the Linux sanitizer job
       now that something calls it. */
    DstPair dst(IM_GRAY, IM_BYTE);
    CHECK(im::Process::MultipleMedian(list, 3, dst.plus) ==
          imProcessMultipleMedian(c_list, 3, dst.c));
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));

    /* Called repeatedly, so a leak of the handle array is worth reporting as
       a growing total rather than a single block. */
    for (int repeat = 0; repeat < 32; repeat++)
      CHECK(im::Process::MultipleMedian(list, 3, dst.plus) != 0);
  }
  SUBCASE("MultipleStdDev")
  {
    /* Takes the mean as a fourth argument, between the array and the
       destination, and it has to be the mean of the same set. */
    DstPair mean(IM_GRAY, IM_BYTE);
    imProcessMultipleMean(c_list, 3, mean.c);
    memcpy(mean.plus.GetHandle()->data[0], mean.c->data[0], (size_t)mean.c->count);

    DstPair dst(IM_GRAY, IM_BYTE);
    im::Process::MultipleStdDev(list, 3, mean.plus, dst.plus);
    imProcessMultipleStdDev(c_list, 3, mean.c, dst.c);
    CHECK(same_data(dst.plus.GetHandle(), dst.c));
    CHECK(wrote_something(dst.c));
  }
}
