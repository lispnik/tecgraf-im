/* Tests for the byte-order utilities in src/im_bin.cpp.
 *
 * These functions sit under every binary format reader, and they are pure
 * byte permutations, so they can be pinned exactly rather than approximately.
 *
 * See the header comment in test_attrib.cpp for the convention used when a
 * bug is being documented rather than fixed.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_binfile.h>

#include <string.h>
#include <string>

namespace {

/* A buffer with guard bytes on both sides, so a swap that walks outside its
   element range is caught by value rather than by luck. */
struct GuardedBuffer
{
  static const int GUARD = 32;
  static const unsigned char FILL = 0xA5;

  unsigned char store[GUARD * 2 + 256];
  int length;

  explicit GuardedBuffer(const unsigned char* bytes, int len)
    : length(len)
  {
    memset(store, FILL, sizeof(store));
    memcpy(store + GUARD, bytes, (size_t)len);
  }

  unsigned char* data() { return store + GUARD; }

  bool guards_intact() const
  {
    for (int i = 0; i < GUARD; i++)
    {
      if (store[i] != FILL) return false;
      if (store[GUARD + length + i] != FILL) return false;
    }
    return true;
  }

  bool matches(const unsigned char* expected) const
  {
    return memcmp(store + GUARD, expected, (size_t)length) == 0;
  }
};

} /* namespace */

TEST_CASE("imBinCPUByteOrder reports a known order, consistently")
{
  int order = imBinCPUByteOrder();
  CHECK((order == IM_LITTLEENDIAN || order == IM_BIGENDIAN));
  CHECK(imBinCPUByteOrder() == order);      /* cached value must not drift */

  /* Cross-check against a direct probe rather than trusting the function. */
  unsigned short w = 0x0001;
  int probed = (*(unsigned char*)&w == 0x01)? IM_LITTLEENDIAN: IM_BIGENDIAN;
  CHECK(order == probed);
}

TEST_CASE("imBinSwapBytes2/4/8 produce the exact expected permutation")
{
  SUBCASE("2-byte values")
  {
    const unsigned char in[]  = { 0x11,0x22,  0x33,0x44 };
    const unsigned char out[] = { 0x22,0x11,  0x44,0x33 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes2(buf.data(), 2);
    CHECK(buf.matches(out));
    CHECK(buf.guards_intact());
  }

  SUBCASE("4-byte values")
  {
    const unsigned char in[]  = { 0x11,0x22,0x33,0x44,  0xAA,0xBB,0xCC,0xDD };
    const unsigned char out[] = { 0x44,0x33,0x22,0x11,  0xDD,0xCC,0xBB,0xAA };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes4(buf.data(), 2);
    CHECK(buf.matches(out));
    CHECK(buf.guards_intact());
  }

  SUBCASE("8-byte values")
  {
    const unsigned char in[]  = { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08 };
    const unsigned char out[] = { 0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes8(buf.data(), 1);
    CHECK(buf.matches(out));
    CHECK(buf.guards_intact());
  }
}

TEST_CASE("swapping twice is the identity")
{
  unsigned char pattern[64];
  for (int i = 0; i < 64; i++) pattern[i] = (unsigned char)(i * 7 + 3);

  SUBCASE("size 2") {
    GuardedBuffer buf(pattern, 64);
    imBinSwapBytes2(buf.data(), 32);
    imBinSwapBytes2(buf.data(), 32);
    CHECK(buf.matches(pattern));
    CHECK(buf.guards_intact());
  }
  SUBCASE("size 4") {
    GuardedBuffer buf(pattern, 64);
    imBinSwapBytes4(buf.data(), 16);
    imBinSwapBytes4(buf.data(), 16);
    CHECK(buf.matches(pattern));
    CHECK(buf.guards_intact());
  }
  SUBCASE("size 8") {
    GuardedBuffer buf(pattern, 64);
    imBinSwapBytes8(buf.data(), 8);
    imBinSwapBytes8(buf.data(), 8);
    CHECK(buf.matches(pattern));
    CHECK(buf.guards_intact());
  }
}

TEST_CASE("a count of zero touches nothing")
{
  const unsigned char in[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

  GuardedBuffer b2(in, sizeof(in)); imBinSwapBytes2(b2.data(), 0);
  CHECK(b2.matches(in));
  GuardedBuffer b4(in, sizeof(in)); imBinSwapBytes4(b4.data(), 0);
  CHECK(b4.matches(in));
  GuardedBuffer b8(in, sizeof(in)); imBinSwapBytes8(b8.data(), 0);
  CHECK(b8.matches(in));
}

TEST_CASE("a negative count touches nothing")
{
  /* Regression: the loops were `while (count-- != 0)`, which for a negative
   * count never reaches zero on the way down -- it runs to integer wraparound,
   * some two billion iterations of writes marching off the end of the buffer.
   *
   * A negative count reaches these functions from imBinFile::Read/Write, whose
   * pCount is an unsigned long truncated into imBinSwapBytes' int parameter. */
  const unsigned char in[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

  SUBCASE("size 2") {
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes2(buf.data(), -1);
    CHECK(buf.matches(in));
    CHECK(buf.guards_intact());
  }
  SUBCASE("size 4") {
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes4(buf.data(), -1000);
    CHECK(buf.matches(in));
    CHECK(buf.guards_intact());
  }
  SUBCASE("size 8") {
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes8(buf.data(), -2147483647 - 1);   /* INT_MIN */
    CHECK(buf.matches(in));
    CHECK(buf.guards_intact());
  }
}

/* NDEBUG only: passing NULL violates the documented precondition, so the
   assert(data) guarding it fires in a build with asserts live. That is the
   assert doing its job -- the point of the case is the release behaviour
   behind it. See the note in test_datatype.cpp. */
#ifdef NDEBUG
TEST_CASE("NULL data is rejected rather than dereferenced")
{
  /* Regression: assert(data) was the only guard, and every shipped build
   * defines NDEBUG -- all four CI jobs build Release or RelWithDebInfo -- so
   * in practice a NULL reached the write loop unchecked. */
  imBinSwapBytes(NULL, 4, 2);
  imBinSwapBytes2(NULL, 4);
  imBinSwapBytes4(NULL, 4);
  imBinSwapBytes8(NULL, 4);
  CHECK(true);                              /* reaching here is the assertion */
}
#endif /* NDEBUG */

TEST_CASE("imBinSwapBytes dispatches on element size")
{
  const unsigned char in[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

  SUBCASE("size 1 is a no-op -- a single byte has no order")
  {
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 8, 1);
    CHECK(buf.matches(in));
    CHECK(buf.guards_intact());
  }

  SUBCASE("size 2 matches imBinSwapBytes2")
  {
    const unsigned char out[] = { 0x22,0x11,0x44,0x33,0x66,0x55,0x88,0x77 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 4, 2);
    CHECK(buf.matches(out));
  }

  SUBCASE("size 4 matches imBinSwapBytes4")
  {
    const unsigned char out[] = { 0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 2, 4);
    CHECK(buf.matches(out));
  }

  SUBCASE("size 8 matches imBinSwapBytes8")
  {
    const unsigned char out[] = { 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 1, 8);
    CHECK(buf.matches(out));
  }
}

TEST_CASE("imBinSwapBytes handles the 16-byte IM_CDOUBLE element")
{
  /* Regression: the switch had no case for 16, so imDataTypeSize(IM_CDOUBLE)
   * -- a size this library defines and hands out -- fell through to a silent
   * no-op and left the data in the file's byte order.
   *
   * IM_CDOUBLE is two doubles in sequence, so the correct conversion swaps
   * each 8-byte half independently; it does not reverse all 16 bytes. */
  REQUIRE(imDataTypeSize(IM_CDOUBLE) == 16);

  const unsigned char in[] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,     /* real */
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18 };   /* imaginary */
  const unsigned char out[] = {
    0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01,
    0x18,0x17,0x16,0x15,0x14,0x13,0x12,0x11 };

  GuardedBuffer buf(in, sizeof(in));
  imBinSwapBytes(buf.data(), 1, imDataTypeSize(IM_CDOUBLE));

  CHECK(buf.matches(out));
  CHECK(buf.guards_intact());
}

TEST_CASE("IM_CFLOAT must be swapped as two 4-byte reals, not one 8-byte value")
{
  /* imDataTypeSize(IM_CFLOAT) is 8, but an IM_CFLOAT is two independent
   * 4-byte floats -- so passing that size to imBinSwapBytes reverses all
   * eight bytes and transposes the real and imaginary parts.
   *
   * Element size alone cannot distinguish "one 8-byte double" from "two
   * 4-byte floats", and 8-byte doubles are the far more common case, so the
   * function keeps the documented meaning and the caller decomposes. This
   * case pins that idiom -- the one src/im_format_raw.cpp already uses under
   * the comment "treat complex as 2 real". */
  REQUIRE(imDataTypeSize(IM_CFLOAT) == 8);

  const unsigned char in[]  = { 0x11,0x22,0x33,0x44,  0xAA,0xBB,0xCC,0xDD };
  const unsigned char correct[] = { 0x44,0x33,0x22,0x11,  0xDD,0xCC,0xBB,0xAA };

  SUBCASE("the documented decomposition is correct")
  {
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 1 * 2, imDataTypeSize(IM_CFLOAT) / 2);
    CHECK(buf.matches(correct));
    CHECK(buf.guards_intact());
  }

  SUBCASE("passing the whole element size transposes real and imaginary")
  {
    /* Documenting the trap, so that anyone tempted to 'simplify' the
       decomposition away sees what it costs. */
    const unsigned char transposed[] = { 0xDD,0xCC,0xBB,0xAA,  0x44,0x33,0x22,0x11 };
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 1, imDataTypeSize(IM_CFLOAT));
    CHECK(buf.matches(transposed));
    CHECK_FALSE(buf.matches(correct));
  }
}

/* NDEBUG only: an unsupported size trips the assert(0 && "...") in the
   default arm of imBinSwapBytes. See the note in test_datatype.cpp. */
#ifdef NDEBUG
TEST_CASE("an unsupported element size leaves the data alone")
{
  /* Sizes that are not a scalar width cannot be swapped meaningfully. The
   * function has no way to report an error, so it must at least not corrupt
   * anything -- and must not silently succeed either. */
  const unsigned char in[] = { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 };

  for (int size = 0; size <= 7; size++)
  {
    if (size == 1 || size == 2 || size == 4) continue;
    GuardedBuffer buf(in, sizeof(in));
    imBinSwapBytes(buf.data(), 1, size);
    CHECK(buf.matches(in));
    CHECK(buf.guards_intact());
  }
}
#endif /* NDEBUG */

/* ------------------------------------------------------------------ *
 * Integration: the swap functions in their real setting.
 * ------------------------------------------------------------------ */

TEST_CASE("byte order survives a format round-trip")
{
  /* Unit tests pin the permutations; this pins that the format readers still
   * agree with the writers after any change to them. SGI is the case that
   * matters most -- it is a big-endian format, so on a little-endian host
   * every header field and every 16-bit sample goes through imBinSwapBytes*.
   *
   * Formats a build cannot write are reported and skipped rather than failed,
   * so this stays valid as add-ons are enabled or disabled. */
  struct Case { const char* format; const char* file; int data_type; };
  const Case cases[] = {
    { "SGI", "roundtrip_byte.sgi",   IM_BYTE   },
    { "SGI", "roundtrip_ushort.sgi", IM_USHORT },   /* 16-bit: the swap path */
    { "BMP", "roundtrip.bmp",        IM_BYTE   },
    { "TGA", "roundtrip.tga",        IM_BYTE   },
    { "RAS", "roundtrip.ras",        IM_BYTE   },
    { "PNM", "roundtrip.pnm",        IM_BYTE   },
  };

  const int width = 37, height = 23;   /* deliberately not multiples of 4 */
  const int total = width * height;

  for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++)
  {
    const Case& tc = cases[c];
    CAPTURE(tc.format);
    CAPTURE(tc.data_type);

    imImage* src = imImageCreate(width, height, IM_RGB, tc.data_type);
    REQUIRE(src != NULL);

    for (int p = 0; p < 3; p++)
      for (int i = 0; i < total; i++)
      {
        if (tc.data_type == IM_BYTE)
          ((imbyte**)src->data)[p][i] = (imbyte)((i * 7 + p * 31) & 0xFF);
        else
          ((imushort**)src->data)[p][i] = (imushort)((i * 313 + p * 7919) & 0xFFFF);
      }

    std::string path = std::string(IM_TEST_OUTPUT_DIR) + "/" + tc.file;

    int err = imFileImageSave(path.c_str(), tc.format, src);
    if (err != IM_ERR_NONE)
    {
      MESSAGE("skipping " << tc.format << ": save returned " << err);
      imImageDestroy(src);
      continue;
    }

    imImage* back = imFileImageLoad(path.c_str(), 0, &err);
    REQUIRE_MESSAGE(back != NULL, "load failed for " << tc.format);

    CHECK(back->width == width);
    CHECK(back->height == height);
    CHECK(back->data_type == tc.data_type);

    for (int p = 0; p < 3; p++)
      CHECK(memcmp(src->data[p], back->data[p],
                   (size_t)total * imDataTypeSize(tc.data_type)) == 0);

    imImageDestroy(src);
    imImageDestroy(back);
  }
}

TEST_CASE("imBinFile Read/Write reject a non-positive element size")
{
  /* Regression: pSizeOf is both the multiplier for the buffer size and the
   * divisor that converts bytes back to an element count.
   *
   *   - zero divided by zero. On x86-64 that is SIGFPE; on ARM64 the hardware
   *     defines it as 0, so the same call crashed on one platform and quietly
   *     returned a plausible short read on the other.
   *   - negative converted to a huge unsigned long in 'pCount * pSizeOf'.
   *
   * Both now report zero elements, which is what the return value means. */
  std::string path = std::string(IM_TEST_OUTPUT_DIR) + "/binfile_sizeof.bin";

  {
    imBinFile* out = imBinFileNew(path.c_str());
    REQUIRE(out != NULL);
    unsigned short values[8];
    for (int i = 0; i < 8; i++) values[i] = (unsigned short)(0x1000 + i);
    imBinFileWrite(out, values, 8, 2);
    imBinFileClose(out);
  }

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);

  /* Force the byte-swap branch on, so the guard is exercised on the whole
     path and not just the final division. */
  imBinFileByteOrder(in, imBinCPUByteOrder() == IM_LITTLEENDIAN
                         ? IM_BIGENDIAN : IM_LITTLEENDIAN);

  unsigned char buffer[64];
  memset(buffer, 0xC3, sizeof(buffer));

  SUBCASE("size 0 reads nothing")
  {
    CHECK(imBinFileRead(in, buffer, 8, 0) == 0);
    for (size_t i = 0; i < sizeof(buffer); i++)
      CHECK(buffer[i] == 0xC3);           /* buffer untouched */
  }

  SUBCASE("negative size reads nothing")
  {
    CHECK(imBinFileRead(in, buffer, 8, -2) == 0);
    for (size_t i = 0; i < sizeof(buffer); i++)
      CHECK(buffer[i] == 0xC3);
  }

  SUBCASE("a valid size still reads")
  {
    unsigned long got = imBinFileRead(in, buffer, 8, 2);
    CHECK(got == 8);
  }

  imBinFileClose(in);

  SUBCASE("writes with a bad size report nothing written")
  {
    imBinFile* out = imBinFileNew((path + ".w").c_str());
    REQUIRE(out != NULL);
    unsigned short values[4] = { 1, 2, 3, 4 };
    CHECK(imBinFileWrite(out, values, 4, 0) == 0);
    CHECK(imBinFileWrite(out, values, 4, -4) == 0);
    CHECK(imBinFileWrite(out, values, 4, 2) == 4);
    imBinFileClose(out);
  }
}

TEST_CASE("a negative element size cannot reach the memory module's memcpy")
{
  /* The sharpest consequence of an unguarded pSizeOf, and the one that is
   * observable on every architecture.
   *
   * pCount * pSizeOf is computed in unsigned long, so pSizeOf = -2 with
   * pCount = 8 wraps to (unsigned long)-16. imBinMemoryFile::ReadBuf clamps
   * with `lOffset + pSize > CurrentSize`, and that sum wraps back to 0 when
   * lOffset is 16 -- so the clamp is skipped and memcpy is asked to move
   * about 2^64 bytes. Before the guard this segfaulted; it must now report
   * zero elements and leave the destination alone.
   *
   * Note the plain-file case cannot be distinguished by return value on
   * ARM64, where division by zero is defined to produce 0 -- exactly what the
   * guard returns. That path is covered by UBSan in ci-sanitizers.yml. */
  static unsigned char store[64];
  memset(store, 0x5A, sizeof(store));

  imBinMemoryFileName mem;
  mem.buffer = store;
  mem.size = (int)sizeof(store);
  mem.reallocate = 0;

  int previous_module = imBinFileSetCurrentModule(IM_MEMFILE);

  imBinFile* file = imBinFileOpen((const char*)&mem);
  REQUIRE(file != NULL);

  imBinFileSeekTo(file, 16);          /* the offset that makes the clamp wrap */

  unsigned char dst[64];
  memset(dst, 0xC3, sizeof(dst));

  CHECK(imBinFileRead(file, dst, 8, -2) == 0);

  for (size_t i = 0; i < sizeof(dst); i++)
    CHECK(dst[i] == 0xC3);            /* destination untouched */

  imBinFileClose(file);
  imBinFileSetCurrentModule(previous_module);
}
