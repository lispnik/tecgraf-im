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
