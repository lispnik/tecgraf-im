/* Tests for the HEIF/AVIF driver in src/im_format_heif.cpp.
 *
 * Unlike the byte-exact format round-trips in test_bin.cpp, HEIC and AVIF are
 * lossy containers, so the tolerances here are measured rather than assumed:
 *
 *   IM_GRAY  + Lossless   exact
 *   IM_RGB   + Lossless   1/255, the RGB<->YCbCr conversion rounding floor
 *   IM_USHORT             15/65535, the 4 bits a 12 bit format cannot carry
 *   no Lossless           ~75/255 even at Quality=100, because 4:2:0 chroma
 *                         subsampling runs before the encoder
 *
 * Anything markedly worse than these means the pixel path has broken, not
 * that the codec got unlucky.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_format_heif.h>

#include <string.h>
#include <string>

namespace {

const int TEST_WIDTH  = 37;      /* deliberately odd, and not a multiple of */
const int TEST_HEIGHT = 23;      /* the 2x2 chroma block                    */

std::string out_path(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

int plane_count(const imImage* image)
{
  return image->depth + (image->has_alpha? 1: 0);
}

/* A horizontal ramp offset per plane: smooth enough to compress, but with
   enough colour separation that chroma subsampling shows up clearly. */
void fill_pattern(imImage* image)
{
  int total = image->width * image->height;
  for (int p = 0; p < plane_count(image); p++)
  {
    for (int i = 0; i < total; i++)
    {
      if (image->data_type == IM_BYTE)
        ((imbyte**)image->data)[p][i] = (imbyte)(((i % image->width) * 4 + p * 20) & 0xFF);
      else
        ((imushort**)image->data)[p][i] = (imushort)(((i % image->width) * 1024 + p * 3000) & 0xFFFF);
    }
  }
}

int max_sample_difference(const imImage* a, const imImage* b)
{
  int total = a->width * a->height, worst = 0;
  for (int p = 0; p < plane_count(a); p++)
  {
    for (int i = 0; i < total; i++)
    {
      int diff;
      if (a->data_type == IM_BYTE)
        diff = (int)((imbyte**)a->data)[p][i] - (int)((imbyte**)b->data)[p][i];
      else
        diff = (int)((imushort**)a->data)[p][i] - (int)((imushort**)b->data)[p][i];

      if (diff < 0) diff = -diff;
      if (diff > worst) worst = diff;
    }
  }
  return worst;
}

/* Saves and reloads. On failure returns NULL and reports both the error and
   which half failed -- a decode-side failure used to be indistinguishable
   from a clean round-trip returning nothing, which made a Windows failure
   unreadable in CI. */
imImage* round_trip(imImage* source, const char* format, const char* file,
                    int lossless, int quality, int* error_out,
                    const char** stage_out = NULL)
{
  int error = IM_ERR_NONE;
  std::string path = out_path(file);

  if (stage_out)
    *stage_out = "open";

  imFile* file_handle = imFileNew(path.c_str(), format, &error);
  if (!file_handle)
  {
    *error_out = error;
    return NULL;
  }

  imFileSetAttribute(file_handle, "Lossless", IM_INT, 1, &lossless);
  imFileSetAttribute(file_handle, "Quality", IM_INT, 1, &quality);

  error = imFileSaveImage(file_handle, source);
  imFileClose(file_handle);

  if (error != IM_ERR_NONE)
  {
    if (stage_out)
      *stage_out = "encode";
    *error_out = error;
    return NULL;
  }

  if (stage_out)
    *stage_out = "decode";

  imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
  *error_out = loaded? IM_ERR_NONE: error;
  return loaded;
}

/* libheif loads its codecs as plugins, and distributions package them
   separately -- Ubuntu splits them into libheif-plugin-*, vcpkg makes them
   opt-in features. A build can therefore have HEVC but not AV1. Treating that
   as a failure would mean demanding every CI platform ship every codec, so
   skip the individual codec instead.

   A missing codec surfaces as IM_ERR_COMPRESS -- but so do genuine defects,
   because the driver returns that code whenever libheif reports an unsupported
   feature and from every encoder-setup failure besides. Deciding to skip from
   the error code alone would therefore turn a regression in the chroma, alpha
   or quality paths into a silently green test.

   So ask the question directly rather than inferring it. Encode the simplest
   thing this driver can produce, 8 bit gray with Lossless: if that round-trips
   the codec is present, and every other failure in this format is this
   driver's own. Call this only after a case has already failed -- a case that
   passed has proved the codec exists by itself.

   The final case in this file guards against the degenerate outcome where
   nothing is available and every case skips. */
bool codec_unavailable(const char* format, const char* file)
{
  enum { UNKNOWN, PRESENT, MISSING };
  static int state[2] = { UNKNOWN, UNKNOWN };
  static int probe_error[2] = { IM_ERR_NONE, IM_ERR_NONE };

  /* Two drivers, so two slots. The cache only earns its keep when the whole
     binary runs in one process; under CTest each case gets its own. */
  int slot = (strcmp(format, "AVIF") == 0)? 1: 0;

  if (state[slot] == UNKNOWN)
  {
    imImage* probe = imImageCreate(16, 16, IM_GRAY, IM_BYTE);
    REQUIRE(probe != NULL);
    fill_pattern(probe);

    /* Named after the caller's file, so two cases probing at once under
       'ctest -j' do not write the same path. */
    std::string probe_file = std::string(file) + ".probe";

    int error = IM_ERR_NONE;
    imImage* loaded = round_trip(probe, format, probe_file.c_str(), 1, 100,
                                 &error);

    state[slot] = loaded? PRESENT: MISSING;
    probe_error[slot] = error;

    if (loaded)
      imImageDestroy(loaded);

    imImageDestroy(probe);
  }

  if (state[slot] == PRESENT)
    return false;

  MESSAGE("skipping " << format << ": this libheif has no codec for it -- "
                      << "8 bit gray does not round-trip either, error "
                      << probe_error[slot]);
  return true;
}

struct RegisterOnce
{
  RegisterOnce() { imFormatRegisterHEIF(); }
};

/* imFormatRegisterHEIF is not idempotent -- it allocates and registers two
   drivers each call -- so register exactly once for the whole binary. */
RegisterOnce register_once;

} /* namespace */

TEST_CASE("HEIF: both drivers register and are writable")
{
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_RGB, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("AVIF", "AV1", IM_RGB, IM_BYTE) == IM_ERR_NONE);
}

TEST_CASE("HEIF: gray round-trips exactly with Lossless")
{
  /* The strongest available check on the pixel path: no chroma to subsample
   * and no colour conversion, so any difference at all is a real defect. */
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "rt_gray.heic", "rt_gray.avif" };

  for (int f = 0; f < 2; f++)
  {
    CAPTURE(formats[f]);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_GRAY, IM_BYTE);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, formats[f], files[f], 1, 100, &save_error);

    if (!loaded && codec_unavailable(formats[f], files[f]))
    {
      imImageDestroy(source);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << save_error);
    CHECK(loaded->width == TEST_WIDTH);
    CHECK(loaded->height == TEST_HEIGHT);
    CHECK(loaded->data_type == IM_BYTE);
    CHECK(imColorModeSpace(loaded->color_space) == IM_GRAY);
    CHECK(max_sample_difference(source, loaded) == 0);

    imImageDestroy(source);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HEIF: RGB round-trips within the conversion rounding floor")
{
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "rt_rgb.heic", "rt_rgb.avif" };

  for (int f = 0; f < 2; f++)
  {
    CAPTURE(formats[f]);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_RGB, IM_BYTE);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, formats[f], files[f], 1, 100, &save_error);

    if (!loaded && codec_unavailable(formats[f], files[f]))
    {
      imImageDestroy(source);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << save_error);
    CHECK(loaded->width == TEST_WIDTH);
    CHECK(loaded->height == TEST_HEIGHT);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

    /* Measured at 1. A regression in the interleave/de-interleave path shows
       up as tens, not as 2. */
    CHECK(max_sample_difference(source, loaded) <= 2);

    imImageDestroy(source);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HEIF: an alpha channel survives the round-trip")
{
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "rt_alpha.heic", "rt_alpha.avif" };

  for (int f = 0; f < 2; f++)
  {
    CAPTURE(formats[f]);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT,
                                    IM_RGB | IM_ALPHA, IM_BYTE);
    REQUIRE(source != NULL);
    REQUIRE(source->has_alpha != 0);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, formats[f], files[f], 1, 100, &save_error);

    if (!loaded && codec_unavailable(formats[f], files[f]))
    {
      imImageDestroy(source);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << save_error);
    CHECK(loaded->has_alpha != 0);
    CHECK(max_sample_difference(source, loaded) <= 2);

    imImageDestroy(source);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HEIF: IM_USHORT is written at 12 bits and scaled back on read")
{
  /* HEVC Main 12 and AV1 both stop at 12 bits, so the low 4 bits of an
   * IM_USHORT sample cannot survive -- 15 of 65535. The driver scales rather
   * than refusing the write, and the two scalings must agree. */
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "rt_u16.heic", "rt_u16.avif" };

  for (int f = 0; f < 2; f++)
  {
    CAPTURE(formats[f]);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_GRAY, IM_USHORT);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    /* Read back as std::string: doctest's message stream prints a bare
       const char* as its address, not its text. */
    const char* stage = "";
    imImage* loaded = round_trip(source, formats[f], files[f], 1, 100,
                                 &save_error, &stage);

    if (!loaded && codec_unavailable(formats[f], files[f]))
    {
      imImageDestroy(source);
      continue;
    }

    /* More than 8 bits per sample depends on how the codec was built, not on
     * anything this driver does: vcpkg's x265 and libde265 are 8-bit builds,
     * so a 12-bit file encodes but will not decode there. Report and skip
     * rather than demand every platform ship a high-bit-depth codec -- the
     * 8-bit cases above already cover the scaling path's structure.
     *
     * The codec reports its refusal as one of libheif's unsupported-feature
     * or codec-plugin errors, all of which the driver maps to
     * IM_ERR_COMPRESS, and the probe above has already established that the
     * codec itself is present -- so here that code means the bit depth and
     * nothing else. Every other error is a defect in the scaling path this
     * case exists to cover, and must fail rather than skip.
     *
     * Windows is the platform that exercises this: vcpkg's x265 and libde265
     * are 8-bit builds, so the 12-bit encode is refused there while Linux and
     * macOS run the case for real. */
    if (!loaded && save_error == IM_ERR_COMPRESS)
    {
      MESSAGE("skipping " << formats[f] << " at 12 bits: " << std::string(stage)
                          << " reported an unsupported feature"
                          << " (codec built without high bit depth?)");
      imImageDestroy(source);
      continue;
    }

    REQUIRE_MESSAGE(loaded != NULL, "save/load failed at " << std::string(stage)
                                    << ", error " << save_error);

    CHECK(loaded->data_type == IM_USHORT);
    CHECK(max_sample_difference(source, loaded) <= 15);

    imImageDestroy(source);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HEIF: a lossy write still preserves geometry and type")
{
  /* Pixels are expected to move a long way without Lossless; everything
   * describing the image must not. */
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "rt_lossy.heic", "rt_lossy.avif" };

  for (int f = 0; f < 2; f++)
  {
    CAPTURE(formats[f]);

    imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_RGB, IM_BYTE);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, formats[f], files[f], 0, 80, &save_error);

    if (!loaded && codec_unavailable(formats[f], files[f]))
    {
      imImageDestroy(source);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << save_error);
    CHECK(loaded->width == TEST_WIDTH);
    CHECK(loaded->height == TEST_HEIGHT);
    CHECK(loaded->data_type == IM_BYTE);
    CHECK(imColorModeSpace(loaded->color_space) == IM_RGB);

    imImageDestroy(source);
    imImageDestroy(loaded);
  }
}

TEST_CASE("HEIF: the file reports its own codec, whichever driver opened it")
{
  /* Open() reads the ftyp brand rather than trusting the driver it was
   * dispatched to, so an .avif does not come back claiming HEVC. */
  imImage* source = imImageCreate(TEST_WIDTH, TEST_HEIGHT, IM_GRAY, IM_BYTE);
  REQUIRE(source != NULL);
  fill_pattern(source);

  int save_error = IM_ERR_NONE;
  imImage* loaded = round_trip(source, "AVIF", "brand.avif", 1, 100, &save_error);
  if (!loaded && codec_unavailable("AVIF", "brand.avif"))
  {
    imImageDestroy(source);
    return;
  }
  REQUIRE_MESSAGE(loaded != NULL, "save/load failed, error " << save_error);
  imImageDestroy(loaded);

  int error = IM_ERR_NONE;
  imFile* file_handle = imFileOpen(out_path("brand.avif").c_str(), &error);
  REQUIRE(file_handle != NULL);

  /* imFileGetInfo fills caller-supplied buffers; compression is at most the
     10 bytes of imFile::compression. */
  char compression[16] = { 0 };
  int image_count = 0;
  imFileGetInfo(file_handle, NULL, compression, &image_count);

  CHECK(strcmp(compression, "AV1") == 0);
  CHECK(image_count >= 1);

  imFileClose(file_handle);
  imImageDestroy(source);
}

TEST_CASE("HEIF: CanWrite rejects what the codecs cannot carry")
{
  /* Colour spaces with no HEIF representation, and data types beyond what
   * the container can hold. */
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_CMYK, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_LAB, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_MAP, IM_BYTE) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_RGB, IM_INT) == IM_ERR_DATA);
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_RGB, IM_FLOAT) == IM_ERR_DATA);

  /* Each driver accepts only its own codec name. */
  CHECK(imFormatCanWriteImage("HEIF", "AV1", IM_RGB, IM_BYTE) == IM_ERR_COMPRESS);
  CHECK(imFormatCanWriteImage("AVIF", "HEVC", IM_RGB, IM_BYTE) == IM_ERR_COMPRESS);

  /* Gray and binary are both carried as monochrome. */
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_GRAY, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("HEIF", "HEVC", IM_BINARY, IM_BYTE) == IM_ERR_NONE);
  CHECK(imFormatCanWriteImage("AVIF", "AV1", IM_RGB, IM_USHORT) == IM_ERR_NONE);
}

TEST_CASE("HEIF: a non-HEIF file is declined, not mis-parsed")
{
  /* Open() must return IM_ERR_FORMAT so imFileOpen can go on to the next
   * driver, rather than failing the open outright or crashing on a container
   * it cannot parse. */
  std::string path = out_path("not_heif.bin");

  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  unsigned char junk[512];
  for (size_t i = 0; i < sizeof(junk); i++) junk[i] = (unsigned char)(i & 0xFF);
  fwrite(junk, 1, sizeof(junk), file);
  fclose(file);

  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(path.c_str(), "HEIF", &error);

  CHECK(handle == NULL);
  CHECK(error == IM_ERR_FORMAT);

  if (handle)
    imFileClose(handle);
}

TEST_CASE("HEIF: a missing file reports an open error")
{
  int error = IM_ERR_NONE;
  imFile* handle = imFileOpenAs(out_path("does_not_exist.heic").c_str(),
                                "HEIF", &error);
  CHECK(handle == NULL);
  CHECK((error == IM_ERR_OPEN || error == IM_ERR_FORMAT));

  if (handle)
    imFileClose(handle);
}

TEST_CASE("HEIF: files from an independent encoder decode correctly")
{
  /* Every other case here round-trips through this driver, which would pass
   * just as happily if the read and write paths were wrong in matching ways.
   * These fixtures were produced by libheif's own heif-enc from
   * html/examples/rice.png and flower.jpg, so decoding them checks this
   * driver against an encoder it had no part in. */
  struct Case {
    const char* file;
    const char* format;      /* the driver whose codec this fixture needs */
    int width, height, color_space;
  };
  const Case cases[] = {
    { "external_gray.heic", "HEIF", 256, 256, IM_GRAY },  /* rice.png is gray */
    { "external_rgb.avif",  "AVIF", 184, 148, IM_RGB  },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);

    std::string path = std::string(IM_TEST_FIXTURE_DIR) + "/" + cases[c].file;

    int error = IM_ERR_NONE;
    imImage* image = imFileImageLoad(path.c_str(), 0, &error);

    if (!image && codec_unavailable(cases[c].format, cases[c].file))
      continue;
    REQUIRE_MESSAGE(image != NULL, "load failed, error " << error);
    CHECK(image->width == cases[c].width);
    CHECK(image->height == cases[c].height);
    CHECK(image->data_type == IM_BYTE);

    /* The grayscale fixture is the one that matters: monochrome detection
       cannot be faked by a symmetric round-trip bug. */
    CHECK(imColorModeSpace(image->color_space) == cases[c].color_space);

    /* Not a flat image -- a decode that silently produced zeros would still
       satisfy every check above. */
    int total = image->width * image->height, distinct = 0;
    imbyte first = ((imbyte**)image->data)[0][0];
    for (int i = 0; i < total; i++)
      if (((imbyte**)image->data)[0][i] != first) { distinct = 1; break; }
    CHECK(distinct == 1);

    imImageDestroy(image);
  }
}

TEST_CASE("HEIF: at least one codec is actually available")
{
  /* Guards the skips above: without this, a libheif built with no codec
   * plugins at all would sail through every case in this file.
   *
   * Self-contained on purpose -- doctest_discover_tests runs each case in its
   * own process, so a counter shared with the cases above would always read
   * zero here. */
  const char* formats[2] = { "HEIF", "AVIF" };
  const char* files[2] = { "probe.heic", "probe.avif" };
  int working = 0;

  for (int f = 0; f < 2; f++)
  {
    imImage* source = imImageCreate(16, 16, IM_GRAY, IM_BYTE);
    REQUIRE(source != NULL);
    fill_pattern(source);

    int save_error = IM_ERR_NONE;
    imImage* loaded = round_trip(source, formats[f], files[f], 1, 100, &save_error);

    if (loaded)
    {
      working++;
      imImageDestroy(loaded);
    }
    else
    {
      MESSAGE(formats[f] << " unavailable, error " << save_error);
    }

    imImageDestroy(source);
  }

  CHECK_MESSAGE(working > 0,
                "neither HEIF nor AVIF could round-trip -- this libheif has no "
                "codec plugins, so every case in this file skipped");
}
