/* Tests for the four format drivers people actually ship images in:
 *
 *   src/im_format_png.cpp   src/im_format_tiff.cpp
 *   src/im_format_gif.cpp   src/im_format_jpeg.cpp
 *
 * Between them roughly 3,600 lines that sat at about 1% covered, while the
 * formats with some coverage -- SGI, BMP, TGA, RAS, PNM -- are the ones
 * nobody sends anybody. The imbalance was the point of writing this.
 *
 * Three of the four are lossless, so their round trips are exact equality
 * rather than a tolerance, and a single wrong byte fails them. JPEG is lossy
 * and gets a measured bound instead, with its structural properties -- size,
 * colour space, data type, reported compression -- asserted exactly.
 *
 * The dimensions are deliberately not multiples of four or eight: row
 * padding, MCU blocks and LZW strip boundaries all like round numbers, and a
 * bug in any of them hides on a 32x32 image.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_palette.h>

#include <stdio.h>
#include <string.h>
#include <string>

namespace {

const int W = 37;
const int H = 23;
const int N = W * H;

std::string scratch(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

int plane_count(const imImage* image)
{
  return image->depth + (image->has_alpha ? 1 : 0);
}

/* A pattern with structure in both axes and a different phase per plane, so
   a transposed row, a dropped column or two planes swapped all show up. A
   flat fill would survive most of those. */
void fill_pattern(imImage* image)
{
  for (int p = 0; p < plane_count(image); p++)
  {
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        int i = y * W + x;
        int v = (x * 7 + y * 13 + p * 53);
        if (image->data_type == IM_BYTE)
          ((imbyte**)image->data)[p][i] = (imbyte)(v & 0xFF);
        else
          ((imushort**)image->data)[p][i] = (imushort)((v * 313) & 0xFFFF);
      }
    }
  }
}

/* A smooth gradient, for the lossy cases. The high-frequency pattern above is
   the worst case for a DCT and round-trips only to within about 150, which is
   too loose a bound to catch anything; a gradient is what JPEG is designed
   for and holds to single digits, so a real regression in the pixel path
   stands out. */
void fill_smooth(imImage* image)
{
  for (int p = 0; p < plane_count(image); p++)
  {
    for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
      {
        /* Kept under 255 without masking: a wrap would put a hard edge in
           the middle of the gradient, which is the one thing a DCT handles
           worst, and would make this bound meaningless. */
        int v = (x * 150) / W + (y * 40) / H + p * 15;
        ((imbyte**)image->data)[p][y * W + x] = (imbyte)v;
      }
    }
  }
}

/* Indices stay inside the palette; the palette itself is not gray, so a
   driver that lost it and fell back to intensities cannot pass. */
void fill_map(imImage* image, int palette_count)
{
  imbyte* data = (imbyte*)image->data[0];
  for (int i = 0; i < N; i++)
    data[i] = (imbyte)(i % palette_count);

  for (int i = 0; i < palette_count; i++)
    image->palette[i] = imColorEncode((imbyte)(i * 7 + 3),
                                      (imbyte)(255 - i * 5),
                                      (imbyte)(i * 11 + 40));
  image->palette_count = palette_count;
}

int max_difference(const imImage* a, const imImage* b)
{
  int worst = 0;
  for (int p = 0; p < plane_count(a); p++)
  {
    for (int i = 0; i < a->count; i++)
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

/* Saves and reloads, reporting which half failed. Returns NULL on failure,
   as in test_heif.cpp -- an unattributed "load returned nothing" was
   unreadable in CI there and would be here too. */
imImage* round_trip(imImage* source, const char* format, const char* file,
                    int* error_out, const char** stage_out)
{
  std::string path = scratch(file);
  *stage_out = "save";

  int error = imFileImageSave(path.c_str(), format, source);
  if (error != IM_ERR_NONE)
  {
    *error_out = error;
    return NULL;
  }

  *stage_out = "load";
  imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
  *error_out = loaded ? IM_ERR_NONE : error;
  return loaded;
}

/* A format a build cannot write is reported and skipped rather than failed,
   matching the convention the other format tests use. */
bool unwritable(const char* format, imImage* source, int error)
{
  if (imFormatCanWriteImage(format, NULL, source->color_space,
                            source->data_type) == IM_ERR_NONE)
    return false;

  MESSAGE("skipping " << format << ": this build cannot write that "
          << "combination (save error " << error << ")");
  return true;
}

} /* namespace */


/* ================================================================== *
 * The lossless three: every sample must survive exactly
 * ================================================================== */

TEST_CASE("PNG: lossless for every colour space and depth it accepts")
{
  struct Case { const char* file; int color_space; int data_type; };
  const Case cases[] = {
    { "rt_rgb.png",    IM_RGB,             IM_BYTE   },
    { "rt_gray.png",   IM_GRAY,            IM_BYTE   },
    { "rt_rgba.png",   IM_RGB | IM_ALPHA,  IM_BYTE   },
    /* 16 bit: PNG stores big-endian, so on a little-endian host every
       sample goes through imBinSwapBytes on the way in and out. */
    { "rt_rgb16.png",  IM_RGB,             IM_USHORT },
    { "rt_gray16.png", IM_GRAY,            IM_USHORT },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);

    imImage* src = imImageCreate(W, H, cases[c].color_space, cases[c].data_type);
    REQUIRE(src != NULL);
    fill_pattern(src);

    int error = IM_ERR_NONE;
    const char* stage = "";
    imImage* loaded = round_trip(src, "PNG", cases[c].file, &error, &stage);

    if (!loaded && unwritable("PNG", src, error))
    {
      imImageDestroy(src);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL,
                    "failed at " << std::string(stage) << ", error " << error);

    CHECK(loaded->width == W);
    CHECK(loaded->height == H);
    CHECK(loaded->data_type == cases[c].data_type);
    CHECK(imColorModeSpace(loaded->color_space) ==
          imColorModeSpace(cases[c].color_space));
    CHECK(loaded->has_alpha == src->has_alpha);

    /* Lossless means lossless -- zero, not a tolerance. */
    CHECK(max_difference(src, loaded) == 0);

    imImageDestroy(src);
    imImageDestroy(loaded);
  }
}

TEST_CASE("PNG: an indexed image keeps its palette")
{
  imImage* src = imImageCreate(W, H, IM_MAP, IM_BYTE);
  REQUIRE(src != NULL);
  fill_map(src, 16);

  int error = IM_ERR_NONE;
  const char* stage = "";
  imImage* loaded = round_trip(src, "PNG", "rt_map.png", &error, &stage);

  if (!loaded && unwritable("PNG", src, error))
  {
    imImageDestroy(src);
    return;
  }
  REQUIRE_MESSAGE(loaded != NULL,
                  "failed at " << std::string(stage) << ", error " << error);

  REQUIRE(imColorModeSpace(loaded->color_space) == IM_MAP);
  REQUIRE(loaded->palette_count >= 16);

  /* The indices may be renumbered, so compare the colours they resolve to
     rather than the raw index -- that is the property that actually matters
     and the weaker check would pass on a palette that was silently rebuilt
     wrong. */
  const imbyte* src_idx = (const imbyte*)src->data[0];
  const imbyte* dst_idx = (const imbyte*)loaded->data[0];
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    REQUIRE(dst_idx[i] < loaded->palette_count);
    CHECK(loaded->palette[dst_idx[i]] == src->palette[src_idx[i]]);
  }

  imImageDestroy(src);
  imImageDestroy(loaded);
}

TEST_CASE("TIFF: lossless across its compressions and depths")
{
  /* NONE and the two deflate spellings are the ones a caller is likely to
     pick, and they take different paths through the writer. LZW is the
     default. */
  struct Case { const char* file; const char* compression;
                int color_space; int data_type; };
  const Case cases[] = {
    { "rt_none.tif",     "NONE",         IM_RGB,            IM_BYTE   },
    { "rt_lzw.tif",      "LZW",          IM_RGB,            IM_BYTE   },
    { "rt_deflate.tif",  "DEFLATE",      IM_RGB,            IM_BYTE   },
    { "rt_adobe.tif",    "ADOBEDEFLATE", IM_RGB,            IM_BYTE   },
    { "rt_gray.tif",     "LZW",          IM_GRAY,           IM_BYTE   },
    { "rt_gray16.tif",   "LZW",          IM_GRAY,           IM_USHORT },
    { "rt_rgb16.tif",    "NONE",         IM_RGB,            IM_USHORT },
    { "rt_rgba.tif",     "LZW",          IM_RGB | IM_ALPHA, IM_BYTE   },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);
    CAPTURE(cases[c].compression);

    imImage* src = imImageCreate(W, H, cases[c].color_space, cases[c].data_type);
    REQUIRE(src != NULL);
    fill_pattern(src);

    std::string path = scratch(cases[c].file);
    int error = IM_ERR_NONE;

    imFile* out = imFileNew(path.c_str(), "TIFF", &error);
    if (!out)
    {
      MESSAGE("skipping " << cases[c].file << ": open failed, error " << error);
      imImageDestroy(src);
      continue;
    }
    imFileSetInfo(out, cases[c].compression);
    error = imFileSaveImage(out, src);
    imFileClose(out);

    if (error != IM_ERR_NONE)
    {
      MESSAGE("skipping " << cases[c].compression << ": save error " << error);
      imImageDestroy(src);
      continue;
    }

    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
    REQUIRE_MESSAGE(loaded != NULL, "load failed, error " << error);

    CHECK(loaded->width == W);
    CHECK(loaded->height == H);
    CHECK(loaded->data_type == cases[c].data_type);
    CHECK(max_difference(src, loaded) == 0);

    imImageDestroy(src);
    imImageDestroy(loaded);
  }
}

TEST_CASE("GIF: lossless for the colour spaces it accepts")
{
  SUBCASE("indexed, through the palette")
  {
    imImage* src = imImageCreate(W, H, IM_MAP, IM_BYTE);
    REQUIRE(src != NULL);
    fill_map(src, 32);

    int error = IM_ERR_NONE;
    const char* stage = "";
    imImage* loaded = round_trip(src, "GIF", "rt_map.gif", &error, &stage);

    if (!loaded && unwritable("GIF", src, error))
    {
      imImageDestroy(src);
      return;
    }
    REQUIRE_MESSAGE(loaded != NULL,
                    "failed at " << std::string(stage) << ", error " << error);

    const imbyte* src_idx = (const imbyte*)src->data[0];
    const imbyte* dst_idx = (const imbyte*)loaded->data[0];
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      REQUIRE(dst_idx[i] < loaded->palette_count);
      CHECK(loaded->palette[dst_idx[i]] == src->palette[src_idx[i]]);
    }

    imImageDestroy(src);
    imImageDestroy(loaded);
  }

  SUBCASE("gray")
  {
    imImage* src = imImageCreate(W, H, IM_GRAY, IM_BYTE);
    REQUIRE(src != NULL);
    fill_pattern(src);

    int error = IM_ERR_NONE;
    const char* stage = "";
    imImage* loaded = round_trip(src, "GIF", "rt_gray.gif", &error, &stage);

    if (!loaded && unwritable("GIF", src, error))
    {
      imImageDestroy(src);
      return;
    }
    REQUIRE_MESSAGE(loaded != NULL,
                    "failed at " << std::string(stage) << ", error " << error);

    /* GIF is indexed, so gray comes back as MAP with a gray palette. What
       has to survive is the intensity, whichever way it is stored. */
    const imbyte* src_data = (const imbyte*)src->data[0];
    const imbyte* dst_data = (const imbyte*)loaded->data[0];
    for (int i = 0; i < N; i++)
    {
      CAPTURE(i);
      int value;
      if (imColorModeSpace(loaded->color_space) == IM_MAP)
      {
        imbyte r, g, b;
        REQUIRE(dst_data[i] < loaded->palette_count);
        imColorDecode(&r, &g, &b, loaded->palette[dst_data[i]]);
        value = r;
      }
      else
        value = dst_data[i];

      CHECK(value == (int)src_data[i]);
    }

    imImageDestroy(src);
    imImageDestroy(loaded);
  }
}


/* ================================================================== *
 * JPEG: lossy, so structure exactly and pixels within a measured bound
 * ================================================================== */

TEST_CASE("JPEG: geometry and type survive exactly, pixels approximately")
{
  struct Case { const char* file; int color_space; };
  const Case cases[] = {
    { "rt_rgb.jpg",  IM_RGB  },
    { "rt_gray.jpg", IM_GRAY },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);

    imImage* src = imImageCreate(W, H, cases[c].color_space, IM_BYTE);
    REQUIRE(src != NULL);
    fill_smooth(src);

    int error = IM_ERR_NONE;
    const char* stage = "";
    imImage* loaded = round_trip(src, "JPEG", cases[c].file, &error, &stage);

    if (!loaded && unwritable("JPEG", src, error))
    {
      imImageDestroy(src);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL,
                    "failed at " << std::string(stage) << ", error " << error);

    /* Everything describing the image is exact even though the pixels are
       not -- that separation is the whole assertion here. */
    CHECK(loaded->width == W);
    CHECK(loaded->height == H);
    CHECK(loaded->data_type == IM_BYTE);
    CHECK(imColorModeSpace(loaded->color_space) ==
          imColorModeSpace(cases[c].color_space));

    /* A gradient at the default quality. Measured in single digits, so this
       bound leaves room for libjpeg version differences while still failing
       on anything structurally wrong -- a swapped plane or a half-shifted
       row would be tens or hundreds. */
    CHECK(max_difference(src, loaded) <= 20);

    imImageDestroy(src);
    imImageDestroy(loaded);
  }
}

TEST_CASE("JPEG: a higher quality setting loses less")
{
  /* The Quality attribute is the only knob most callers ever touch, and
     nothing checked it was wired to anything. */
  imImage* src = imImageCreate(W, H, IM_RGB, IM_BYTE);
  REQUIRE(src != NULL);
  fill_smooth(src);

  int worst_low = -1, worst_high = -1;
  long size_low = 0, size_high = 0;

  const int qualities[2] = { 15, 95 };
  for (int q = 0; q < 2; q++)
  {
    std::string path = scratch(q == 0 ? "quality_low.jpg" : "quality_high.jpg");

    int error = IM_ERR_NONE;
    imFile* out = imFileNew(path.c_str(), "JPEG", &error);
    if (!out)
    {
      MESSAGE("skipping: JPEG open failed with error " << error);
      imImageDestroy(src);
      return;
    }
    /* "JPEGQuality", not "Quality" -- each driver names its own, and
       HEIF's happens to be the shorter spelling. */
    imFileSetAttribute(out, "JPEGQuality", IM_INT, 1, &qualities[q]);
    error = imFileSaveImage(out, src);
    imFileClose(out);
    REQUIRE(error == IM_ERR_NONE);

    FILE* f = fopen(path.c_str(), "rb");
    REQUIRE(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    imImage* loaded = imFileImageLoad(path.c_str(), 0, &error);
    REQUIRE_MESSAGE(loaded != NULL, "load failed, error " << error);

    if (q == 0) { worst_low = max_difference(src, loaded); size_low = size; }
    else        { worst_high = max_difference(src, loaded); size_high = size; }

    imImageDestroy(loaded);
  }

  /* Both directions, because either one alone could pass on an attribute
     that was read but ignored. */
  CHECK(worst_high < worst_low);
  CHECK(size_high > size_low);

  imImageDestroy(src);
}


/* ================================================================== *
 * Dispatch: the right driver has to claim the right file
 * ================================================================== */

TEST_CASE("format: a file opened without a hint reports its own format")
{
  /* imFileOpen tries each registered driver in turn. A driver whose Open()
     is too permissive claims files belonging to another one, which shows up
     as a mis-detected format long before it shows up as bad pixels. */
  struct Case { const char* format; const char* file; int color_space; };
  const Case cases[] = {
    { "PNG",  "detect.png", IM_RGB  },
    { "TIFF", "detect.tif", IM_RGB  },
    { "JPEG", "detect.jpg", IM_RGB  },
    { "GIF",  "detect.gif", IM_GRAY },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].format);

    imImage* src = imImageCreate(W, H, cases[c].color_space, IM_BYTE);
    REQUIRE(src != NULL);
    fill_pattern(src);

    std::string path = scratch(cases[c].file);
    int error = imFileImageSave(path.c_str(), cases[c].format, src);
    imImageDestroy(src);

    if (error != IM_ERR_NONE)
    {
      MESSAGE("skipping " << cases[c].format << ": save error " << error);
      continue;
    }

    imFile* file = imFileOpen(path.c_str(), &error);
    REQUIRE_MESSAGE(file != NULL, "open failed, error " << error);

    char format[32] = { 0 };
    char compression[32] = { 0 };
    int image_count = 0;
    imFileGetInfo(file, format, compression, &image_count);

    CHECK(strcmp(format, cases[c].format) == 0);
    CHECK(image_count >= 1);
    CHECK(strlen(compression) > 0);

    imFileClose(file);
  }
}

TEST_CASE("format: every advertised compression name survives being set")
{
  /* imFileSetInfo used to strcpy into a char[10]. The TIFF driver advertises
     "ADOBEDEFLATE" (12 characters) and "THUNDERSCAN" (11) in its own
     compression table, so passing either -- a documented value, obtained from
     the library itself -- overflowed the field into image_count beside it.
     glibc's _FORTIFY_SOURCE aborts on it; on macOS it silently corrupted the
     struct, which is why only the Linux jobs caught it.

     Asserted as a round trip rather than by watching for a crash, so it fails
     on every platform if the buffer is ever shrunk again: a name that comes
     back truncated is the same defect one step earlier. */
  const char* names[] = { "NONE", "LZW", "DEFLATE", "ADOBEDEFLATE",
                          "THUNDERSCAN", "CCITTFAX4", "PIXARLOG" };

  for (size_t c = 0; c < sizeof(names)/sizeof(names[0]); c++)
  {
    CAPTURE(names[c]);

    imImage* src = imImageCreate(W, H, IM_RGB, IM_BYTE);
    REQUIRE(src != NULL);
    fill_pattern(src);

    std::string path = scratch("compname.tif");
    int error = IM_ERR_NONE;
    imFile* out = imFileNew(path.c_str(), "TIFF", &error);
    REQUIRE_MESSAGE(out != NULL, "open failed, error " << error);

    imFileSetInfo(out, names[c]);

    /* Straight back out of the same handle: whether the codec is available
       is a separate question from whether the name was stored intact. */
    char stored[32] = { 0 };
    imFileGetInfo(out, NULL, stored, NULL);
    CHECK(strcmp(stored, names[c]) == 0);

    imFileSaveImage(out, src);
    imFileClose(out);
    imImageDestroy(src);
  }
}

TEST_CASE("format: none of the drivers claims a file of junk")
{
  /* Every Open() has to decline cleanly so imFileOpen can move on, rather
     than half-parsing a header and returning nonsense. */
  std::string path = scratch("junk.dat");

  FILE* f = fopen(path.c_str(), "wb");
  REQUIRE(f != NULL);
  unsigned char junk[1024];
  for (size_t i = 0; i < sizeof(junk); i++)
    junk[i] = (unsigned char)((i * 37 + 11) & 0xFF);
  fwrite(junk, 1, sizeof(junk), f);
  fclose(f);

  int error = IM_ERR_NONE;
  imFile* file = imFileOpen(path.c_str(), &error);

  CHECK(file == NULL);
  CHECK(error == IM_ERR_FORMAT);

  if (file)
    imFileClose(file);

  /* And the same file offered to a specific driver, which is the path a
     caller takes when it already believes it knows the format. */
  const char* formats[4] = { "PNG", "TIFF", "JPEG", "GIF" };
  for (int i = 0; i < 4; i++)
  {
    CAPTURE(formats[i]);
    error = IM_ERR_NONE;
    imFile* handle = imFileOpenAs(path.c_str(), formats[i], &error);
    CHECK(handle == NULL);
    CHECK(error == IM_ERR_FORMAT);
    if (handle)
      imFileClose(handle);
  }
}

TEST_CASE("format: a truncated file is refused, not half-decoded")
{
  /* Half a PNG is the shape of a download that died. The driver has to fail
     rather than hand back an image with a torn-off bottom edge. */
  imImage* src = imImageCreate(W, H, IM_RGB, IM_BYTE);
  REQUIRE(src != NULL);
  fill_pattern(src);

  std::string whole = scratch("truncate_src.png");
  REQUIRE(imFileImageSave(whole.c_str(), "PNG", src) == IM_ERR_NONE);
  imImageDestroy(src);

  FILE* in = fopen(whole.c_str(), "rb");
  REQUIRE(in != NULL);
  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  fseek(in, 0, SEEK_SET);
  REQUIRE(size > 64);

  long keep = size / 2;
  unsigned char* buffer = (unsigned char*)malloc((size_t)keep);
  REQUIRE(buffer != NULL);
  REQUIRE(fread(buffer, 1, (size_t)keep, in) == (size_t)keep);
  fclose(in);

  std::string cut = scratch("truncated.png");
  FILE* out = fopen(cut.c_str(), "wb");
  REQUIRE(out != NULL);
  fwrite(buffer, 1, (size_t)keep, out);
  fclose(out);
  free(buffer);

  int error = IM_ERR_NONE;
  imImage* loaded = imFileImageLoad(cut.c_str(), 0, &error);

  /* Either outcome is defensible and libpng picks the second: it decodes the
     rows it received and reports success, so the caller gets a real image
     with an incomplete lower half rather than nothing. What is not
     negotiable is that the geometry comes from the header rather than from
     how much data arrived -- a driver that sized its buffer from the header
     and then wrote as many rows as it could read is the shape of a heap
     overflow, and this case exists to put that path under the sanitizers.

     Asserted as a disjunction on purpose. Pinning it to "returns NULL" would
     be pinning libpng's recovery policy rather than anything this tree
     controls. */
  if (loaded)
  {
    CHECK(loaded->width == W);
    CHECK(loaded->height == H);
    CHECK(loaded->data_type == IM_BYTE);
    imImageDestroy(loaded);
  }
  else
    CHECK(error != IM_ERR_NONE);
}

TEST_CASE("ICO and PCX: lossless for the colour spaces they accept")
{
  /* The two remaining format drivers, both around 6% covered and both
     lossless -- PCX by run-length coding, ICO by storing the samples raw --
     so these are exact comparisons like PNG's rather than tolerances. Each
     accepts RGB, gray and indexed at IM_BYTE and nothing else. */
  struct Case { const char* format; const char* file; int color_space; };
  const Case cases[] = {
    { "ICO", "rt_rgb.ico",  IM_RGB  },
    { "ICO", "rt_gray.ico", IM_GRAY },
    { "PCX", "rt_rgb.pcx",  IM_RGB  },
    { "PCX", "rt_gray.pcx", IM_GRAY },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].file);

    imImage* src = imImageCreate(W, H, cases[c].color_space, IM_BYTE);
    REQUIRE(src != NULL);
    fill_pattern(src);

    int error = IM_ERR_NONE;
    const char* stage = "";
    imImage* loaded = round_trip(src, cases[c].format, cases[c].file, &error, &stage);

    if (!loaded && unwritable(cases[c].format, src, error))
    {
      imImageDestroy(src);
      continue;
    }
    REQUIRE_MESSAGE(loaded != NULL,
                    "failed at " << std::string(stage) << ", error " << error);

    CHECK(loaded->width == W);
    CHECK(loaded->height == H);
    CHECK(loaded->data_type == IM_BYTE);

    /* Both store gray through a palette, as GIF does, so compare the
       intensity a sample resolves to rather than the raw byte. */
    if (imColorModeSpace(loaded->color_space) == IM_MAP &&
        imColorModeSpace(cases[c].color_space) == IM_GRAY)
    {
      const imbyte* src_data = (const imbyte*)src->data[0];
      const imbyte* dst_data = (const imbyte*)loaded->data[0];
      for (int i = 0; i < N; i++)
      {
        CAPTURE(i);
        REQUIRE(dst_data[i] < loaded->palette_count);
        imbyte r, g, b;
        imColorDecode(&r, &g, &b, loaded->palette[dst_data[i]]);
        CHECK((int)r == (int)src_data[i]);
      }
    }
    else
    {
      CHECK(imColorModeSpace(loaded->color_space) ==
            imColorModeSpace(cases[c].color_space));
      CHECK(max_difference(src, loaded) == 0);
    }

    imImageDestroy(src);
    imImageDestroy(loaded);
  }
}

TEST_CASE("PCX: an indexed image keeps its palette")
{
  imImage* src = imImageCreate(W, H, IM_MAP, IM_BYTE);
  REQUIRE(src != NULL);
  fill_map(src, 16);

  int error = IM_ERR_NONE;
  const char* stage = "";
  imImage* loaded = round_trip(src, "PCX", "rt_map.pcx", &error, &stage);

  if (!loaded && unwritable("PCX", src, error))
  {
    imImageDestroy(src);
    return;
  }
  REQUIRE_MESSAGE(loaded != NULL,
                  "failed at " << std::string(stage) << ", error " << error);

  const imbyte* src_idx = (const imbyte*)src->data[0];
  const imbyte* dst_idx = (const imbyte*)loaded->data[0];
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    REQUIRE(dst_idx[i] < loaded->palette_count);
    CHECK(loaded->palette[dst_idx[i]] == src->palette[src_idx[i]]);
  }

  imImageDestroy(src);
  imImageDestroy(loaded);
}
