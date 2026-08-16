/* Tests for the smaller corners of libim that nothing else reaches:
 *
 *   src/im_compress.cpp   the zlib, liblzf and lz4 wrappers
 *   src/im_palette.cpp    the built-in palettes and the two lookups
 *   src/im_binfile.cpp    seeking, byte order and the text readers
 *   src/im_old.cpp        the compatibility layer, still exported
 *
 * These are small and independent enough that they were never going to be
 * covered incidentally by a format or process test, which is exactly why they
 * sat at nought.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_palette.h>
#include <im_binfile.h>
#include <im_old.h>

#include <stdio.h>
#include <string.h>
#include <string>

namespace {

std::string scratch(const char* name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

/* Compressible on purpose: a run-heavy pattern, so every codec has something
   to find and a round trip is a real exercise rather than a memcpy. */
void fill_compressible(unsigned char* data, int size)
{
  for (int i = 0; i < size; i++)
    data[i] = (unsigned char)((i / 16) & 0x3F);
}

} /* namespace */


/* ================================================================== *
 * Compression -- src/im_compress.cpp
 * ================================================================== */

TEST_CASE("compress: every codec round-trips its own output")
{
  /* All three are used by format drivers to store pixel data, so a wrapper
     that silently truncated would corrupt files rather than fail to write
     them. Each returns the byte count it produced, or 0 on failure. */
  const int SIZE = 4096;
  unsigned char src[SIZE];
  unsigned char packed[SIZE * 2];
  unsigned char back[SIZE];

  fill_compressible(src, SIZE);

  SUBCASE("zlib")
  {
    int packed_size = imCompressDataZ(src, SIZE, packed, sizeof(packed), 6);
    REQUIRE(packed_size > 0);
    CHECK(packed_size < SIZE);            /* it actually compressed */

    memset(back, 0, SIZE);
    int back_size = imCompressDataUnZ(packed, packed_size, back, SIZE);
    CHECK(back_size == SIZE);
    CHECK(memcmp(src, back, SIZE) == 0);
  }

  SUBCASE("lzf")
  {
    int packed_size = imCompressDataLZF(src, SIZE, packed, sizeof(packed));
    REQUIRE(packed_size > 0);
    CHECK(packed_size < SIZE);

    memset(back, 0, SIZE);
    int back_size = imCompressDataUnLZF(packed, packed_size, back, SIZE);
    CHECK(back_size == SIZE);
    CHECK(memcmp(src, back, SIZE) == 0);
  }

  SUBCASE("lz4")
  {
    int packed_size = imCompressDataLZ4(src, SIZE, packed, sizeof(packed));
    REQUIRE(packed_size > 0);
    CHECK(packed_size < SIZE);

    memset(back, 0, SIZE);
    int back_size = imCompressDataUnLZ4(packed, packed_size, back, SIZE);
    CHECK(back_size == SIZE);
    CHECK(memcmp(src, back, SIZE) == 0);
  }
}

TEST_CASE("compress: a destination too small fails rather than overflowing")
{
  /* The drivers size their output buffer from the input, so this is the path
     that runs whenever incompressible data grows instead of shrinking. What
     matters is that it reports failure and leaves the guard byte alone. */
  const int SIZE = 1024;
  unsigned char src[SIZE];
  unsigned char tiny[9];

  fill_compressible(src, SIZE);
  memset(tiny, 0xAB, sizeof(tiny));

  SUBCASE("zlib")
  {
    CHECK(imCompressDataZ(src, SIZE, tiny, 8, 6) == 0);
    CHECK(tiny[8] == 0xAB);
  }
  SUBCASE("lzf")
  {
    CHECK(imCompressDataLZF(src, SIZE, tiny, 8) == 0);
    CHECK(tiny[8] == 0xAB);
  }
  SUBCASE("lz4")
  {
    CHECK(imCompressDataLZ4(src, SIZE, tiny, 8) == 0);
    CHECK(tiny[8] == 0xAB);
  }
}

TEST_CASE("compress: decoding truncated input fails rather than reading on")
{
  const int SIZE = 2048;
  unsigned char src[SIZE];
  unsigned char packed[SIZE * 2];
  unsigned char back[SIZE];

  fill_compressible(src, SIZE);

  int packed_size = imCompressDataZ(src, SIZE, packed, sizeof(packed), 6);
  REQUIRE(packed_size > 16);

  /* Half a stream is not a stream. zlib and lz4 both carry enough structure
     to notice; the point is that neither walks off the end of the buffer. */
  CHECK(imCompressDataUnZ(packed, packed_size / 2, back, SIZE) == 0);

  packed_size = imCompressDataLZ4(src, SIZE, packed, sizeof(packed));
  REQUIRE(packed_size > 16);
  CHECK(imCompressDataUnLZ4(packed, packed_size / 2, back, SIZE) == 0);
}


/* ================================================================== *
 * Palettes -- src/im_palette.cpp
 * ================================================================== */

TEST_CASE("palette: the built-in palettes are full and start from black")
{
  /* Every generator returns 256 entries allocated with imPaletteNew, so the
     caller can index any byte value without checking. */
  struct { const char* name; long* (*make)(void); int black_first; } cases[] = {
    { "gray",          imPaletteGray,         1 },
    { "red",           imPaletteRed,          1 },
    { "green",         imPaletteGreen,        1 },
    { "blue",          imPaletteBlue,         1 },
    { "yellow",        imPaletteYellow,       1 },
    { "magenta",       imPaletteMagenta,      1 },
    { "cyan",          imPaletteCyan,         1 },
    { "hues",          imPaletteHues,         0 },
    { "blue ice",      imPaletteBlueIce,      0 },
    { "hot iron",      imPaletteHotIron,      0 },
    { "black body",    imPaletteBlackBody,    0 },
    { "high contrast", imPaletteHighContrast, 0 },
    { "linear",        imPaletteLinear,       0 },
    { "uniform",       imPaletteUniform,      0 },
    { "rainbow",       imPaletteRainbow,      0 },
  };

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    CAPTURE(cases[c].name);

    long* palette = cases[c].make();
    REQUIRE(palette != NULL);

    unsigned char r, g, b;

    if (cases[c].black_first)
    {
      /* The single-hue ramps run from black to full intensity. */
      imColorDecode(&r, &g, &b, palette[0]);
      CHECK((int)r == 0); CHECK((int)g == 0); CHECK((int)b == 0);
    }

    /* Nothing may be left uninitialised: a palette with a hole in it shows up
       as a run of identical entries at the end. */
    imColorDecode(&r, &g, &b, palette[255]);
    unsigned char r2, g2, b2;
    imColorDecode(&r2, &g2, &b2, palette[128]);
    CHECK((r != r2 || g != g2 || b != b2));

    imPaletteRelease(palette);
  }
}

TEST_CASE("palette: gray is the diagonal of the colour cube")
{
  long* palette = imPaletteGray();
  REQUIRE(palette != NULL);

  for (int i = 0; i < 256; i++)
  {
    CAPTURE(i);
    unsigned char r, g, b;
    imColorDecode(&r, &g, &b, palette[i]);
    CHECK((int)r == i);
    CHECK((int)g == i);
    CHECK((int)b == i);
  }

  imPaletteRelease(palette);
}

TEST_CASE("palette: an exact lookup respects its tolerance")
{
  long palette[4];
  palette[0] = imColorEncode(0, 0, 0);
  palette[1] = imColorEncode(100, 100, 100);
  palette[2] = imColorEncode(255, 0, 0);
  palette[3] = imColorEncode(10, 20, 30);

  /* Zero tolerance means exactly equal. */
  CHECK(imPaletteFindColor(palette, 4, imColorEncode(255, 0, 0), 0) == 2);
  CHECK(imPaletteFindColor(palette, 4, imColorEncode(10, 20, 30), 0) == 3);
  CHECK(imPaletteFindColor(palette, 4, imColorEncode(254, 0, 0), 0) == -1);

  /* A tolerance admits near misses, and only near ones. */
  CHECK(imPaletteFindColor(palette, 4, imColorEncode(254, 1, 1), 5) == 2);
  CHECK(imPaletteFindColor(palette, 4, imColorEncode(200, 0, 0), 5) == -1);
}

TEST_CASE("palette: the nearest lookup always finds something")
{
  long palette[3];
  palette[0] = imColorEncode(0, 0, 0);
  palette[1] = imColorEncode(255, 255, 255);
  palette[2] = imColorEncode(255, 0, 0);

  /* Unlike FindColor this cannot fail -- every colour has a nearest entry, so
     -1 is never a valid answer here. It used to be the only answer: the
     running minimum was seeded with (unsigned int)-1 into an int, so the
     comparison against a squared distance never held and nothing but an exact
     match was ever found. */
  CHECK(imPaletteFindNearest(palette, 3, imColorEncode(10, 10, 10)) == 0);
  CHECK(imPaletteFindNearest(palette, 3, imColorEncode(240, 240, 240)) == 1);
  CHECK(imPaletteFindNearest(palette, 3, imColorEncode(200, 20, 20)) == 2);

  /* An exact match must win over anything merely close. */
  CHECK(imPaletteFindNearest(palette, 3, imColorEncode(255, 0, 0)) == 2);
}

TEST_CASE("palette: duplicate copies the entries, not the pointer")
{
  long* original = imPaletteGray();
  REQUIRE(original != NULL);

  long* copy = imPaletteDuplicate(original, 256);
  REQUIRE(copy != NULL);
  CHECK(copy != original);
  CHECK(memcmp(original, copy, 256 * sizeof(long)) == 0);

  /* Writing through one must not disturb the other. */
  copy[10] = imColorEncode(1, 2, 3);
  CHECK(original[10] != copy[10]);

  imPaletteRelease(original);
  imPaletteRelease(copy);
}


/* ================================================================== *
 * Binary files -- src/im_binfile.cpp
 * ================================================================== */

TEST_CASE("binfile: values written come back in the same order")
{
  std::string path = scratch("binfile_roundtrip.bin");

  const unsigned short values[4] = { 0x0102, 0x0304, 0xFFFE, 0x0000 };
  unsigned short back[4] = { 0, 0, 0, 0 };

  imBinFile* out = imBinFileNew(path.c_str());
  REQUIRE(out != NULL);
  CHECK(imBinFileWrite(out, (void*)values, 4, 2) == 4);
  CHECK(imBinFileError(out) == 0);
  imBinFileClose(out);

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);
  CHECK(imBinFileSize(in) == 8);
  CHECK(imBinFileRead(in, back, 4, 2) == 4);
  CHECK(imBinFileError(in) == 0);
  imBinFileClose(in);

  for (int i = 0; i < 4; i++)
  {
    CAPTURE(i);
    CHECK(back[i] == values[i]);
  }
}

TEST_CASE("binfile: asking for the other byte order swaps on the way through")
{
  std::string path = scratch("binfile_order.bin");

  const unsigned short value = 0x0102;
  unsigned short back = 0;

  imBinFile* out = imBinFileNew(path.c_str());
  REQUIRE(out != NULL);
  imBinFileWrite(out, (void*)&value, 1, 2);
  imBinFileClose(out);

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);

  /* Flip to whichever order the host is not, and the bytes have to come back
     reversed -- that is the whole job of the setting. */
  int native = imBinFileByteOrder(in, IM_LITTLEENDIAN);
  imBinFileByteOrder(in, native == IM_LITTLEENDIAN ? IM_BIGENDIAN : IM_LITTLEENDIAN);
  imBinFileRead(in, &back, 1, 2);
  imBinFileClose(in);

  CHECK(back == 0x0201);
}

TEST_CASE("binfile: seeking lands where it says and reports the position")
{
  std::string path = scratch("binfile_seek.bin");

  unsigned char values[16];
  for (int i = 0; i < 16; i++)
    values[i] = (unsigned char)(i * 3);

  imBinFile* out = imBinFileNew(path.c_str());
  REQUIRE(out != NULL);
  imBinFileWrite(out, values, 16, 1);
  imBinFileClose(out);

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);

  unsigned char one = 0;

  imBinFileSeekTo(in, 5);
  CHECK(imBinFileTell(in) == 5);
  imBinFileRead(in, &one, 1, 1);
  CHECK((int)one == 15);

  /* Relative to where the last read left off, which is 6. */
  imBinFileSeekOffset(in, 2);
  CHECK(imBinFileTell(in) == 8);
  imBinFileRead(in, &one, 1, 1);
  CHECK((int)one == 24);

  /* Backwards from the end. */
  imBinFileSeekFrom(in, -1);
  imBinFileRead(in, &one, 1, 1);
  CHECK((int)one == 45);
  CHECK(imBinFileEndOfFile(in) != 0);

  imBinFileClose(in);
}

TEST_CASE("binfile: opening something that is not there fails cleanly")
{
  imBinFile* missing = imBinFileOpen(scratch("no_such_file.bin").c_str());
  CHECK(missing == NULL);
}

TEST_CASE("binfile: the text readers parse numbers and skip comments")
{
  /* PNM and other plain-text headers are read through these, so a parser
     that stopped at the wrong delimiter would misread the geometry. */
  std::string path = scratch("binfile_text.txt");

  FILE* f = fopen(path.c_str(), "wb");
  REQUIRE(f != NULL);
  fputs("# a comment line\n42 -7\n3.5\n", f);
  fclose(f);

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);

  CHECK(imBinFileSkipLine(in) != 0);

  int value = 0;
  CHECK(imBinFileReadInteger(in, &value) != 0);
  CHECK(value == 42);
  CHECK(imBinFileReadInteger(in, &value) != 0);
  CHECK(value == -7);

  double real = 0;
  CHECK(imBinFileReadReal(in, &real) != 0);
  CHECK(real == doctest::Approx(3.5));

  imBinFileClose(in);
}


/* ================================================================== *
 * The compatibility layer -- src/im_old.cpp
 * ================================================================== */

TEST_CASE("old: the colour encoders are the same ones under a different name")
{
  /* imEncodeColor and imDecodeColor are the pre-3.0 spellings, still
     exported. They have to agree with the current pair exactly, or a caller
     mixing the two eras gets its channels shuffled. */
  for (int i = 0; i < 256; i += 37)
  {
    CAPTURE(i);
    unsigned char r = (unsigned char)i;
    unsigned char g = (unsigned char)(255 - i);
    unsigned char b = (unsigned char)((i * 3) & 0xFF);

    CHECK(imEncodeColor(r, g, b) == imColorEncode(r, g, b));

    unsigned char dr, dg, db;
    imDecodeColor(&dr, &dg, &db, imEncodeColor(r, g, b));
    CHECK((int)dr == (int)r);
    CHECK((int)dg == (int)g);
    CHECK((int)db == (int)b);
  }
}

TEST_CASE("old: RGB to map and back preserves a small palette exactly")
{
  const int W = 8, H = 2, N = W * H;
  unsigned char red[N], green[N], blue[N], map[N];
  unsigned char br[N], bg[N], bb[N];
  long palette[256];

  const unsigned char cr[4] = { 255,   0,   0,  17 };
  const unsigned char cg[4] = {   0, 255,   0,  99 };
  const unsigned char cb[4] = {   0,   0, 255, 200 };

  for (int i = 0; i < N; i++)
  {
    red[i]   = cr[i % 4];
    green[i] = cg[i % 4];
    blue[i]  = cb[i % 4];
  }

  imRGB2Map(W, H, red, green, blue, map, 256, palette);
  imMap2RGB(W, H, map, 256, palette, br, bg, bb);

  /* Four colours into 256 slots has to be lossless, so this is an equality
     rather than a tolerance. */
  for (int i = 0; i < N; i++)
  {
    CAPTURE(i);
    CHECK((int)br[i] == (int)red[i]);
    CHECK((int)bg[i] == (int)green[i]);
    CHECK((int)bb[i] == (int)blue[i]);
  }
}


/* ================================================================== *
 * The pre-3.0 file API -- src/im_old.cpp and src/im_oldresize.c
 *
 * Still exported, still in im_old.h, and at 1.8% and 0% covered. It takes
 * separate R, G and B buffers and an integer format code rather than an
 * imImage, and it is the layer most likely to have rotted quietly: nothing
 * inside the library calls it, so only an external consumer would ever have
 * noticed.
 * ================================================================== */

TEST_CASE("old: an RGB image saves, identifies and loads back")
{
  const int w = 12, h = 8, n = w * h;
  unsigned char red[96], green[96], blue[96];
  unsigned char br[96], bg[96], bb[96];

  for (int i = 0; i < n; i++)
  {
    red[i]   = (unsigned char)((i * 7) & 0xFF);
    green[i] = (unsigned char)((i * 11 + 30) & 0xFF);
    blue[i]  = (unsigned char)((i * 13 + 60) & 0xFF);
  }

  std::string path = scratch("old_rgb.tga");

  /* IM_TGA is lossless and takes RGB directly, so the round trip is exact.
     The format code is the old enum, not a string. */
  REQUIRE(imSaveRGB(w, h, IM_TGA, red, green, blue, (char*)path.c_str()) == IM_ERR_NONE);

  SUBCASE("imFileFormat reports the format it was written as")
  {
    int format = -1;
    REQUIRE(imFileFormat((char*)path.c_str(), &format) == IM_ERR_NONE);
    CHECK(format == IM_TGA);
  }

  SUBCASE("and does so for every format the old API can write")
  {
    /* Checked across the whole table rather than one entry, because the
       mapping used to be wrong for all of them: every comparison in
       FormatNew2Old was negated, so a BMP came back as IM_GIF and everything
       else as IM_BMP. A single-format case would have caught it, but only by
       luck -- the one value that was accidentally right was IM_BMP, and any
       test that happened to pick BMP would have passed. */
    struct Case { int code; const char* ext; };
    const Case cases[] = {
      { IM_BMP, "bmp" }, { IM_PCX, "pcx" }, { IM_TIF, "tif" },
      { IM_RAS, "ras" }, { IM_SGI, "sgi" }, { IM_LED, "led" },
      { IM_TGA, "tga" },
    };

    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
    {
      CAPTURE(cases[c].ext);
      std::string p = scratch((std::string("old_fmt.") + cases[c].ext).c_str());

      int err = imSaveRGB(w, h, cases[c].code, red, green, blue, (char*)p.c_str());
      if (err != IM_ERR_NONE)
      {
        MESSAGE("skipping " << cases[c].ext << ": save error " << err);
        continue;
      }

      int format = -1;
      REQUIRE(imFileFormat((char*)p.c_str(), &format) == IM_ERR_NONE);

      /* The high byte carries the compression flag, so compare the format
         itself rather than the whole word. */
      CHECK((format & 0x00FF) == cases[c].code);
    }
  }

  SUBCASE("imImageInfo reports the geometry")
  {
    int iw = 0, ih = 0, type = -1, palette_count = -1;
    REQUIRE(imImageInfo((char*)path.c_str(), &iw, &ih, &type, &palette_count) == IM_ERR_NONE);
    CHECK(iw == w);
    CHECK(ih == h);
  }

  SUBCASE("imLoadRGB returns the samples unchanged")
  {
    REQUIRE(imLoadRGB((char*)path.c_str(), br, bg, bb) == IM_ERR_NONE);
    for (int i = 0; i < n; i++)
    {
      CAPTURE(i);
      CHECK((int)br[i] == (int)red[i]);
      CHECK((int)bg[i] == (int)green[i]);
      CHECK((int)bb[i] == (int)blue[i]);
    }
  }
}

TEST_CASE("old: an indexed image saves and loads back through its palette")
{
  const int w = 12, h = 8, n = w * h;
  unsigned char map[96], back[96];
  long palette[256], back_palette[256];

  for (int i = 0; i < 8; i++)
    palette[i] = imColorEncode((unsigned char)(i * 30 + 5),
                               (unsigned char)(255 - i * 20),
                               (unsigned char)(i * 11));
  for (int i = 0; i < n; i++)
    map[i] = (unsigned char)(i % 8);

  std::string path = scratch("old_map.bmp");
  REQUIRE(imSaveMap(w, h, IM_BMP, map, 8, palette, (char*)path.c_str()) == IM_ERR_NONE);

  REQUIRE(imLoadMap((char*)path.c_str(), back, back_palette) == IM_ERR_NONE);

  /* Indices may be renumbered by the writer, so compare the colours they
     resolve to rather than the raw index. */
  for (int i = 0; i < n; i++)
  {
    CAPTURE(i);
    CHECK(back_palette[back[i]] == palette[map[i]]);
  }
}

TEST_CASE("old: a missing file is reported rather than crashing")
{
  int format = -1;
  CHECK(imFileFormat((char*)scratch("old_absent.bmp").c_str(), &format) != IM_ERR_NONE);

  int w = 0, h = 0, type = 0, pc = 0;
  CHECK(imImageInfo((char*)scratch("old_absent.bmp").c_str(), &w, &h, &type, &pc) != IM_ERR_NONE);
}

TEST_CASE("old: RGB to gray uses a gray palette")
{
  const int w = 4, h = 2, n = w * h;
  unsigned char red[8]   = { 255,   0,   0, 255, 0, 128, 64, 200 };
  unsigned char green[8] = {   0, 255,   0, 255, 0, 128, 64, 200 };
  unsigned char blue[8]  = {   0,   0, 255, 255, 0, 128, 64, 200 };
  unsigned char gray_map[8];
  long grays[256];

  imRGB2Gray(w, h, red, green, blue, gray_map, grays);

  /* The result is indexed, and the palette it points into has to be gray --
     equal components -- or the "gray" in the name means nothing. */
  for (int i = 0; i < n; i++)
  {
    CAPTURE(i);
    unsigned char r, g, b;
    imColorDecode(&r, &g, &b, grays[gray_map[i]]);
    CHECK((int)r == (int)g);
    CHECK((int)g == (int)b);
  }

  /* And white has to come out brighter than black, whatever the weighting. */
  unsigned char rw, gw, bw, rb, gb, bb2;
  imColorDecode(&rw, &gw, &bw, grays[gray_map[3]]);   /* white  */
  imColorDecode(&rb, &gb, &bb2, grays[gray_map[4]]);  /* black  */
  CHECK((int)rw > (int)rb);
}

TEST_CASE("old: the legacy resize keeps the corners and the size")
{
  /* imResize and imStretch predate imProcessResize and work on a raw byte
     buffer. Both are nearest-neighbour-ish, so the corners are the property
     that has to hold whatever the sampling convention: a corner of the
     destination comes from a corner of the source. */
  const int sw = 8, sh = 6;
  unsigned char src[48];
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++)
      src[y*sw + x] = (unsigned char)(x * 10 + y * 3 + 1);

  SUBCASE("enlarging")
  {
    const int dw = 16, dh = 12;
    unsigned char dst[192];
    memset(dst, 0xEE, sizeof(dst));

    imResize(sw, sh, src, dw, dh, dst);

    CHECK((int)dst[0] == (int)src[0]);
    CHECK((int)dst[dw - 1] == (int)src[sw - 1]);
    CHECK((int)dst[(dh - 1)*dw] == (int)src[(sh - 1)*sw]);

    /* Every sample was written -- the sentinel must be gone. */
    int untouched = 0;
    for (int i = 0; i < dw*dh; i++)
      if (dst[i] == 0xEE) untouched++;
    CHECK(untouched == 0);
  }

  SUBCASE("reducing")
  {
    const int dw = 4, dh = 3;
    unsigned char dst[12];
    memset(dst, 0xEE, sizeof(dst));

    imResize(sw, sh, src, dw, dh, dst);

    CHECK((int)dst[0] == (int)src[0]);
    int untouched = 0;
    for (int i = 0; i < dw*dh; i++)
      if (dst[i] == 0xEE) untouched++;
    CHECK(untouched == 0);
  }

  SUBCASE("imStretch fills its target too")
  {
    const int dw = 10, dh = 10;
    unsigned char dst[100];
    memset(dst, 0xEE, sizeof(dst));

    imStretch(sw, sh, src, dw, dh, dst);

    int untouched = 0;
    for (int i = 0; i < dw*dh; i++)
      if (dst[i] == 0xEE) untouched++;
    CHECK(untouched == 0);
  }

  SUBCASE("the same size is a copy")
  {
    unsigned char dst[48];
    memset(dst, 0xEE, sizeof(dst));
    imResize(sw, sh, src, sw, sh, dst);
    CHECK(memcmp(dst, src, sizeof(src)) == 0);
  }
}
