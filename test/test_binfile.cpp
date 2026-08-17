/* Tests for src/im_binfile.cpp -- the I/O layer every format driver reads
 * and writes through.
 *
 * The default module, IM_RAWFILE, was already well exercised: every format
 * test in this suite goes through it. The other three shipped modules were
 * not reached at all, and neither was the module-selection API itself. That is
 * what these cases are for, and the pattern from the rest of this suite held
 * again -- code nothing had ever run contained defects, three of them:
 *
 *   the memory module hung outright on a small reallocate factor
 *   imBinFileSetCurrentModule accepted a negative module and the next
 *     open called through a function pointer read from before the array
 *   imBinFileReadLine prefixed every line it returned with a zero byte
 *
 * All three are fixed; the cases that would have caught each say so where
 * they are. The last one is visible from outside this file, in the
 * Description attribute of any PNM or KRN file carrying a comment, so it is
 * pinned there too rather than only at this layer.
 *
 * A note on the memory module's fake file name: imBinFileOpen and
 * imBinFileNew take a const char*, and for IM_MEMFILE that pointer is really
 * an imBinMemoryFileName*. The casts below are what a caller has to write, not
 * an abuse of the API.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_util.h>
#include <im_image.h>
#include <im_binfile.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

/* Selects a module for the duration of a scope and puts the previous one back,
   so a case that fails partway cannot leave the module set for the next one --
   these are process-wide. */
struct ScopedModule
{
  int previous;

  explicit ScopedModule(int module)
  {
    previous = imBinFileSetCurrentModule(module);
    REQUIRE(previous != -1);
  }

  ~ScopedModule() { imBinFileSetCurrentModule(previous); }
};

/* A temporary path in the directory CTest runs in. */
std::string temp_path(const char* name)
{
  return std::string("test_binfile_") + name + ".tmp";
}

void write_file(const std::string& path, const void* data, size_t length)
{
  FILE* file = fopen(path.c_str(), "wb");
  REQUIRE(file != NULL);
  if (length) REQUIRE(fwrite(data, 1, length, file) == length);
  fclose(file);
}

} /* namespace */


/* ================================================================== *
 * Module selection
 * ================================================================== */

TEST_CASE("binfile: the module is selected and restored, and bounded")
{
  int original = imBinFileSetCurrentModule(IM_RAWFILE);
  REQUIRE(original != -1);

  SUBCASE("each shipped module can be selected")
  {
    /* IM_FILEHANDLE is deliberately absent: its file name is a system file
       handle rather than a path, so selecting it is fine but the open below
       would need a real descriptor. Selecting is what is checked here. */
    const int modules[] = { IM_RAWFILE, IM_STREAM, IM_MEMFILE, IM_SUBFILE };
    for (int i = 0; i < 4; i++)
    {
      CAPTURE(modules[i]);
      int previous = imBinFileSetCurrentModule(modules[i]);
      CHECK(previous != -1);
    }
  }

  SUBCASE("a module past the end is refused and changes nothing")
  {
    imBinFileSetCurrentModule(IM_STREAM);
    CHECK(imBinFileSetCurrentModule(9999) == -1);
    /* still IM_STREAM: the rejected call must not have moved it */
    CHECK(imBinFileSetCurrentModule(IM_RAWFILE) == IM_STREAM);
  }

  SUBCASE("a negative module is refused and changes nothing")
  {
    /* This one used to be stored. The index picks a function pointer out of
       iBinFileModule, so the next imBinFileOpen read from before the start of
       that array and called whatever it found -- a bus error, from nothing
       worse than a caller passing a variable that held -1.

       The bad value is not reachable to be tested for any more, which is the
       point; what is asserted is that it is refused, and that the refusal
       leaves the previous module in force rather than half-applying. */
    imBinFileSetCurrentModule(IM_STREAM);
    CHECK(imBinFileSetCurrentModule(-1) == -1);
    CHECK(imBinFileSetCurrentModule(-999) == -1);
    CHECK(imBinFileSetCurrentModule(IM_RAWFILE) == IM_STREAM);
  }

  SUBCASE("so -1 means refused and nothing else")
  {
    /* A module is never -1, so the sentinel is unambiguous. Before the bound
       was fixed, -1 could be a module, and then a successful call returning
       "the previous module was -1" was indistinguishable from a failure. */
    for (int module = IM_RAWFILE; module <= IM_SUBFILE; module++)
    {
      CAPTURE(module);
      CHECK(imBinFileSetCurrentModule(module) != -1);
    }
  }

  imBinFileSetCurrentModule(original);
}


/* ================================================================== *
 * IM_MEMFILE -- a buffer that behaves like a file
 * ================================================================== */

TEST_CASE("binfile: the memory module round-trips through a caller's buffer")
{
  ScopedModule module(IM_MEMFILE);

  unsigned char buffer[64];
  memset(buffer, 0, sizeof(buffer));

  imBinMemoryFileName name;
  name.buffer = buffer;
  name.size = (int)sizeof(buffer);
  name.reallocate = 0.0f;         /* the buffer is mine, do not move it */

  imBinFile* file = imBinFileNew((const char*)&name);
  REQUIRE(file != NULL);

  unsigned int values[4] = { 1u, 2u, 3u, 0x04050607u };
  CHECK(imBinFileWrite(file, values, 4, 4) == 4);
  CHECK(imBinFileError(file) == 0);
  CHECK(imBinFileTell(file) == 16);
  CHECK(imBinFileSize(file) == 16);       /* what was written, not the capacity */

  /* It wrote into the caller's buffer and not a copy of it. */
  CHECK(memcmp(buffer, values, 16) == 0);

  imBinFileSeekTo(file, 0);
  CHECK(imBinFileTell(file) == 0);
  CHECK(imBinFileEndOfFile(file) == 0);

  unsigned int back[4] = { 0, 0, 0, 0 };
  CHECK(imBinFileRead(file, back, 4, 4) == 4);
  CHECK(imBinFileError(file) == 0);
  for (int i = 0; i < 4; i++)
  {
    CAPTURE(i);
    CHECK(back[i] == values[i]);
  }
  CHECK(imBinFileEndOfFile(file) == 1);

  imBinFileClose(file);
}

TEST_CASE("binfile: the memory module seeks the way the others do")
{
  ScopedModule module(IM_MEMFILE);

  unsigned char buffer[16];
  for (int i = 0; i < 16; i++) buffer[i] = (unsigned char)i;

  imBinMemoryFileName name;
  name.buffer = buffer;
  name.size = 16;
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileOpen((const char*)&name);
  REQUIRE(file != NULL);
  CHECK(imBinFileSize(file) == 16);   /* opened, so the whole buffer is content */

  unsigned char value = 0;

  SUBCASE("SeekTo is absolute")
  {
    imBinFileSeekTo(file, 5);
    CHECK(imBinFileTell(file) == 5);
    CHECK(imBinFileRead(file, &value, 1, 1) == 1);
    CHECK((int)value == 5);
  }
  SUBCASE("SeekOffset is relative to where it is")
  {
    imBinFileSeekTo(file, 4);
    imBinFileSeekOffset(file, 3);
    CHECK(imBinFileTell(file) == 7);
    CHECK(imBinFileRead(file, &value, 1, 1) == 1);
    CHECK((int)value == 7);

    imBinFileSeekOffset(file, -5);    /* backwards, the case a reader needs */
    CHECK(imBinFileTell(file) == 3);
  }
  SUBCASE("SeekFrom is relative to the end")
  {
    imBinFileSeekFrom(file, -4);
    CHECK(imBinFileTell(file) == 12);
    CHECK(imBinFileRead(file, &value, 1, 1) == 1);
    CHECK((int)value == 12);
  }
  SUBCASE("seeking outside the buffer is an error and does not move")
  {
    imBinFileSeekTo(file, 8);
    imBinFileSeekOffset(file, -100);
    CHECK(imBinFileError(file) != 0);
    CHECK(imBinFileTell(file) == 8);

    imBinFileSeekTo(file, 8);
    imBinFileSeekOffset(file, 100);
    CHECK(imBinFileError(file) != 0);
    CHECK(imBinFileTell(file) == 8);
  }
  SUBCASE("reading past the end is a short read, not a walk off the buffer")
  {
    imBinFileSeekTo(file, 12);
    unsigned char into[16];
    memset(into, 0xCC, sizeof(into));
    unsigned long got = imBinFileRead(file, into, 16, 1);
    CHECK(got == 4);                  /* only what was there */
    CHECK(imBinFileError(file) != 0);
    for (int i = 0; i < 4; i++) CHECK((int)into[i] == 12 + i);
    CHECK((int)into[4] == 0xCC);      /* and nothing beyond it was touched */
  }

  imBinFileClose(file);
}

TEST_CASE("binfile: the memory module grows the buffer when asked to")
{
  ScopedModule module(IM_MEMFILE);

  SUBCASE("with no factor it refuses to grow and reports a short write")
  {
    unsigned char buffer[8];
    imBinMemoryFileName name;
    name.buffer = buffer;
    name.size = 8;
    name.reallocate = 0.0f;

    imBinFile* file = imBinFileNew((const char*)&name);
    REQUIRE(file != NULL);

    unsigned char data[16];
    memset(data, 0xEE, sizeof(data));
    CHECK(imBinFileWrite(file, data, 16, 1) == 8);
    CHECK(imBinFileError(file) != 0);
    CHECK(imBinFileSize(file) == 8);
    CHECK(name.buffer == buffer);      /* the caller's buffer was not moved */

    imBinFileClose(file);
  }

  SUBCASE("a NULL buffer is allocated to the size given")
  {
    imBinMemoryFileName name;
    name.buffer = NULL;
    name.size = 32;
    name.reallocate = 1.0f;

    imBinFile* file = imBinFileNew((const char*)&name);
    REQUIRE(file != NULL);
    REQUIRE(name.buffer != NULL);

    unsigned char data[200];
    for (int i = 0; i < 200; i++) data[i] = (unsigned char)(i & 0xFF);

    CHECK(imBinFileWrite(file, data, 200, 1) == 200);
    CHECK(imBinFileError(file) == 0);
    CHECK(imBinFileSize(file) == 200);
    CHECK(name.size >= 200);           /* it grew, and said so */
    CHECK(memcmp(name.buffer, data, 200) == 0);

    imBinFileClose(file);
    imBinMemoryRelease(name.buffer);   /* the documented way to free it */
  }

  SUBCASE("a factor too small to add a byte still terminates")
  {
    /* This hung forever. The step is the factor times the ORIGINAL size,
       which the loop does not change, so any factor below 1/size truncated to
       zero and the loop spun with nothing to stop it. A four-byte buffer needs
       only a factor under 0.25 to reach it, and the header invites a
       fractional factor by describing the growth as size += reallocate*size.

       There is no way to assert "does not hang" -- reaching the assertions
       below at all is the result. */
    imBinMemoryFileName name;
    name.buffer = NULL;
    name.size = 4;
    name.reallocate = 0.1f;            /* 0.1 * 4 truncates to 0 */

    imBinFile* file = imBinFileNew((const char*)&name);
    REQUIRE(file != NULL);

    unsigned char data[8];
    memset(data, 0x22, sizeof(data));
    CHECK(imBinFileWrite(file, data, 8, 1) == 8);
    CHECK(imBinFileError(file) == 0);
    CHECK(imBinFileSize(file) == 8);
    CHECK(memcmp(name.buffer, data, 8) == 0);

    imBinFileClose(file);
    imBinMemoryRelease(name.buffer);
  }

  SUBCASE("a negative factor is treated as no growth rather than as a size")
  {
    /* Out of contract, and it used to cast a negative float to unsigned long
       to compute the step. Now it simply does not grow. */
    unsigned char buffer[8];
    imBinMemoryFileName name;
    name.buffer = buffer;
    name.size = 8;
    name.reallocate = -2.0f;

    imBinFile* file = imBinFileNew((const char*)&name);
    REQUIRE(file != NULL);

    unsigned char data[16];
    memset(data, 0x33, sizeof(data));
    CHECK(imBinFileWrite(file, data, 16, 1) == 8);
    CHECK(imBinFileError(file) != 0);
    CHECK(name.buffer == buffer);

    imBinFileClose(file);
  }
}

TEST_CASE("binfile: the memory module corrects byte order like the others")
{
  /* The byte-order machinery lives in the base class, so it should apply
     whichever module is underneath. Nothing had checked that for this one. */
  ScopedModule module(IM_MEMFILE);

  unsigned char buffer[8];
  memset(buffer, 0, sizeof(buffer));

  imBinMemoryFileName name;
  name.buffer = buffer;
  name.size = 8;
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileNew((const char*)&name);
  REQUIRE(file != NULL);

  int cpu_order = imBinCPUByteOrder();
  int other = (cpu_order == IM_LITTLEENDIAN)? IM_BIGENDIAN: IM_LITTLEENDIAN;
  CHECK(imBinFileByteOrder(file, other) == cpu_order);

  unsigned int value = 0x01020304u;

  /* Snapshot before the write, because the write is documented to consume it:
     "the function will not make a temporary copy of the values to invert the
     byte order, so after the call the values will be invalid". */
  unsigned char before[4];
  memcpy(before, &value, 4);

  CHECK(imBinFileWrite(file, &value, 1, 4) == 1);

  /* The buffer holds the bytes in the opposite order to the way the CPU had
     them. */
  for (int i = 0; i < 4; i++)
  {
    CAPTURE(i);
    CHECK((int)buffer[i] == (int)before[3 - i]);
  }

  /* And the caller's variable really was swapped in place rather than copied.
     Asserted rather than merely avoided: it is the sharpest edge on this API,
     it only bites when the file order differs from the CPU's, and a change
     that started copying would make the warning in the header wrong. */
  CHECK(value == 0x04030201u);

  /* Reading back with the same setting undoes it, into a fresh variable. */
  imBinFileSeekTo(file, 0);
  unsigned int back = 0;
  CHECK(imBinFileRead(file, &back, 1, 4) == 1);
  CHECK(back == 0x01020304u);

  imBinFileClose(file);
}


/* ================================================================== *
 * IM_STREAM -- the same operations over an ANSI C stream
 * ================================================================== */

TEST_CASE("binfile: the stream module reads and writes a real file")
{
  ScopedModule module(IM_STREAM);
  std::string path = temp_path("stream");

  unsigned int values[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };

  imBinFile* out = imBinFileNew(path.c_str());
  REQUIRE(out != NULL);
  CHECK(imBinFileWrite(out, values, 4, 4) == 4);
  CHECK(imBinFileError(out) == 0);
  imBinFileClose(out);

  imBinFile* in = imBinFileOpen(path.c_str());
  REQUIRE(in != NULL);
  CHECK(imBinFileSize(in) == 16);
  CHECK(imBinFileTell(in) == 0);       /* FileSize must put the position back */

  unsigned int back[4] = { 0, 0, 0, 0 };
  CHECK(imBinFileRead(in, back, 4, 4) == 4);
  for (int i = 0; i < 4; i++)
  {
    CAPTURE(i);
    CHECK(back[i] == values[i]);
  }

  SUBCASE("and seeks the three ways")
  {
    imBinFileSeekTo(in, 4);
    CHECK(imBinFileTell(in) == 4);
    imBinFileSeekOffset(in, 4);
    CHECK(imBinFileTell(in) == 8);
    imBinFileSeekFrom(in, -4);
    CHECK(imBinFileTell(in) == 12);

    unsigned int one = 0;
    CHECK(imBinFileRead(in, &one, 1, 4) == 1);
    CHECK(one == values[3]);
  }

  SUBCASE("end of file is reported only after a read has hit it")
  {
    /* Worth pinning because it is NOT the same rule the other modules use.
       This module reports feof, which a stream sets when a read comes up
       short, while the memory and raw-file modules compare the position
       against the size. So a caller that switches modules cannot rely on
       end-of-file meaning the same thing, and the difference only shows at
       exactly the last byte. */
    imBinFileSeekTo(in, 16);
    CHECK(imBinFileEndOfFile(in) == 0);      /* at the end, but nothing read */

    unsigned char value = 0;
    CHECK(imBinFileRead(in, &value, 1, 1) == 0);
    CHECK(imBinFileEndOfFile(in) == 1);      /* now it says so */
  }

  imBinFileClose(in);
  remove(path.c_str());
}

TEST_CASE("binfile: the stream module returns NULL for a file that is not there")
{
  ScopedModule module(IM_STREAM);

  /* fopen fails, and the module reports an error for a NULL handle, which is
     what imBinFileOpen checks before handing anything back. If it did not,
     every later call would assert on or dereference that handle. */
  imBinFile* file = imBinFileOpen("test_binfile_no_such_file_exists.tmp");
  CHECK(file == NULL);
}


/* ================================================================== *
 * IM_SUBFILE -- a file positioned inside another one
 * ================================================================== */

TEST_CASE("binfile: a subfile is offset from where the parent was left")
{
  std::string path = temp_path("sub");

  /* A header the parent reads, then a payload the subfile owns. */
  unsigned char content[24];
  for (int i = 0; i < 24; i++) content[i] = (unsigned char)(100 + i);
  write_file(path, content, sizeof(content));

  int original = imBinFileSetCurrentModule(IM_RAWFILE);
  REQUIRE(original != -1);

  imBinFile* parent = imBinFileOpen(path.c_str());
  REQUIRE(parent != NULL);

  /* Skip a notional 8-byte header. Where the parent sits at the moment the
     subfile is created is the subfile's origin. */
  imBinFileSeekTo(parent, 8);

  REQUIRE(imBinFileSetCurrentModule(IM_SUBFILE) != -1);
  imBinFile* sub = imBinFileOpen((const char*)parent);
  REQUIRE(sub != NULL);

  CHECK(imBinFileTell(sub) == 0);        /* the origin, in the subfile's terms */

  unsigned char value = 0;
  CHECK(imBinFileRead(sub, &value, 1, 1) == 1);
  CHECK((int)value == 108);              /* content[8], not content[0] */
  CHECK(imBinFileTell(sub) == 1);

  SUBCASE("and an absolute seek is relative to that origin")
  {
    imBinFileSeekTo(sub, 4);
    CHECK(imBinFileTell(sub) == 4);
    CHECK(imBinFileRead(sub, &value, 1, 1) == 1);
    CHECK((int)value == 112);            /* content[8+4] */

    /* The parent moved with it, since there is only one position underneath. */
    CHECK(imBinFileTell(parent) == 13);
  }

  SUBCASE("but the size is the parent's, not the subfile's")
  {
    /* Asymmetric, and not a defect to fix -- a subfile has no recorded end, so
       there is nothing else it could report. Pinned because it means the
       position and the size are in different coordinate systems, and
       Tell() == Size() is therefore not how a caller detects the end here. */
    CHECK(imBinFileSize(sub) == 24);
    CHECK(imBinFileSize(sub) == imBinFileSize(parent));
  }

  imBinFileClose(sub);
  imBinFileSetCurrentModule(IM_RAWFILE);
  imBinFileClose(parent);
  imBinFileSetCurrentModule(original);
  remove(path.c_str());
}


/* ================================================================== *
 * The text helpers, over a memory buffer
 * ================================================================== */

TEST_CASE("binfile: a line is read back without a leading zero byte")
{
  /* imBinFileReadLine used to store its loop variable before the first read,
     so what came back was a zero byte followed by the line, and *size counted
     it. Anything treating the result as a string saw an empty one.

     The two callers in the library, the PNM and KRN readers, hand the result
     straight to an attribute called Description, so the bug was visible from
     outside -- the case further down loads a file and checks it there. */
  ScopedModule module(IM_MEMFILE);

  char text[] = "first line\nsecond line\n";
  imBinMemoryFileName name;
  name.buffer = (unsigned char*)text;
  name.size = (int)strlen(text);
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileOpen((const char*)&name);
  REQUIRE(file != NULL);

  char line[64];
  memset(line, 0x7F, sizeof(line));
  int size = (int)sizeof(line);

  REQUIRE(imBinFileReadLine(file, line, &size) != 0);
  CHECK(std::string(line) == "first line");
  CHECK((int)line[0] != 0);                  /* the defect, stated directly */
  CHECK(size == 11);                         /* ten characters and the zero */
  CHECK(line[10] == 0);

  SUBCASE("and the next call gets the next line")
  {
    int second_size = (int)sizeof(line);
    REQUIRE(imBinFileReadLine(file, line, &second_size) != 0);
    CHECK(std::string(line) == "second line");
    CHECK(second_size == 12);
  }

  imBinFileClose(file);
}

TEST_CASE("binfile: a line survives either kind of line break")
{
  ScopedModule module(IM_MEMFILE);

  /* The reader has to cope with all three conventions, and the DOS one is the
     interesting case: it consumes the \n after a \r, and has to put back
     anything else it finds so the next read is not one byte short. */
  const char* inputs[3] = { "abc\ndef\n", "abc\r\ndef\r\n", "abc\rdef\r" };
  const char* names[3] = { "unix", "dos", "old mac" };

  for (int i = 0; i < 3; i++)
  {
    CAPTURE(std::string(names[i]));

    std::string text(inputs[i]);
    imBinMemoryFileName name;
    name.buffer = (unsigned char*)text.c_str();
    name.size = (int)text.size();
    name.reallocate = 0.0f;

    imBinFile* file = imBinFileOpen((const char*)&name);
    REQUIRE(file != NULL);

    char line[16];
    int size = (int)sizeof(line);
    REQUIRE(imBinFileReadLine(file, line, &size) != 0);
    CHECK(std::string(line) == "abc");

    size = (int)sizeof(line);
    REQUIRE(imBinFileReadLine(file, line, &size) != 0);
    CHECK(std::string(line) == "def");

    imBinFileClose(file);
  }
}

TEST_CASE("binfile: skipping a line lands on the next one")
{
  ScopedModule module(IM_MEMFILE);

  char text[] = "skip me\nkeep me\n";
  imBinMemoryFileName name;
  name.buffer = (unsigned char*)text;
  name.size = (int)strlen(text);
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileOpen((const char*)&name);
  REQUIRE(file != NULL);

  REQUIRE(imBinFileSkipLine(file) != 0);
  CHECK(imBinFileTell(file) == 8);

  char line[16];
  int size = (int)sizeof(line);
  REQUIRE(imBinFileReadLine(file, line, &size) != 0);
  CHECK(std::string(line) == "keep me");

  imBinFileClose(file);
}

TEST_CASE("binfile: numbers are read up to the first character that is not one")
{
  ScopedModule module(IM_MEMFILE);

  /* This is how the PNM header is parsed, so the separators a reader meets in
     practice -- spaces, newlines -- all have to work. */
  char text[] = "640 480\n-17 3.5 2.5e2 x";
  imBinMemoryFileName name;
  name.buffer = (unsigned char*)text;
  name.size = (int)strlen(text);
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileOpen((const char*)&name);
  REQUIRE(file != NULL);

  int width = 0, height = 0, negative = 0;
  REQUIRE(imBinFileReadInteger(file, &width) != 0);
  CHECK(width == 640);
  REQUIRE(imBinFileReadInteger(file, &height) != 0);
  CHECK(height == 480);
  REQUIRE(imBinFileReadInteger(file, &negative) != 0);
  CHECK(negative == -17);

  double real = 0.0;
  REQUIRE(imBinFileReadReal(file, &real) != 0);
  CHECK(real == doctest::Approx(3.5));

  SUBCASE("including one written in exponent form")
  {
    double exponent = 0.0;
    REQUIRE(imBinFileReadReal(file, &exponent) != 0);
    CHECK(exponent == doctest::Approx(250.0));
  }

  imBinFileClose(file);
}

TEST_CASE("binfile: running out of input is a failure, not a wrong answer")
{
  ScopedModule module(IM_MEMFILE);

  /* No line break and no separator, so both readers reach the end of the
     buffer still looking. The answer that matters is that they say they
     failed rather than returning what they happened to have. */
  char text[] = "12";
  imBinMemoryFileName name;
  name.buffer = (unsigned char*)text;
  name.size = 2;
  name.reallocate = 0.0f;

  SUBCASE("for a line")
  {
    imBinFile* file = imBinFileOpen((const char*)&name);
    REQUIRE(file != NULL);
    char line[16];
    int size = (int)sizeof(line);
    CHECK(imBinFileReadLine(file, line, &size) == 0);
    imBinFileClose(file);
  }

  SUBCASE("for an integer")
  {
    imBinFile* file = imBinFileOpen((const char*)&name);
    REQUIRE(file != NULL);
    int value = -12345;
    CHECK(imBinFileReadInteger(file, &value) == 0);
    imBinFileClose(file);
  }
}

TEST_CASE("binfile: printf writes formatted text and reports the length")
{
  ScopedModule module(IM_MEMFILE);

  unsigned char buffer[64];
  memset(buffer, 0, sizeof(buffer));

  imBinMemoryFileName name;
  name.buffer = buffer;
  name.size = (int)sizeof(buffer);
  name.reallocate = 0.0f;

  imBinFile* file = imBinFileNew((const char*)&name);
  REQUIRE(file != NULL);

  unsigned long written = imBinFilePrintf(file, "P5\n%d %d\n%d\n", 640, 480, 255);
  CHECK(written == 15);                /* no terminator, per the header */
  CHECK(imBinFileSize(file) == 15);
  CHECK(memcmp(buffer, "P5\n640 480\n255\n", 15) == 0);
  CHECK(buffer[15] == 0);              /* and nothing written past it */

  imBinFileClose(file);
}


/* ================================================================== *
 * The same defect, seen from outside this layer
 * ================================================================== */

TEST_CASE("binfile: a PNM comment reaches the Description attribute intact")
{
  /* The reason the leading zero byte mattered. imFileFormatPNM hands
     imBinFileReadLine's output straight to an attribute, so every commented
     PNM produced a Description that began with a zero -- length 17 for a
     15-character comment, and an empty string to anything that read it as
     one. Asserted here rather than only at the layer below because this is
     where a user would notice, and because it is the path the two callers in
     the library actually take. */
  /* Named .pgm rather than .tmp: format detection tries the drivers in turn,
     and letting libtiff be asked first puts a complaint on stderr that has
     nothing to do with this case. */
  std::string path = "test_binfile_comment.pgm";

  const char* header = "P5\n# a comment here\n4 2\n255\n";
  unsigned char content[64];
  size_t header_length = strlen(header);
  memcpy(content, header, header_length);
  for (size_t i = 0; i < 8; i++) content[header_length + i] = (unsigned char)(i * 30);
  write_file(path, content, header_length + 8);

  int error = IM_ERR_NONE;
  imImage* image = imFileImageLoad(path.c_str(), 0, &error);
  REQUIRE_MESSAGE(image != NULL, "load failed with error " << error);
  CHECK(image->width == 4);
  CHECK(image->height == 2);

  int data_type = 0, size = 0;
  const void* description = imImageGetAttribute(image, "Description", &data_type, &size);
  REQUIRE(description != NULL);

  const char* text = (const char*)description;
  CHECK((int)text[0] != 0);
  CHECK(std::string(text) == " a comment here");
  CHECK(size == 16);              /* fifteen characters and the zero */

  imImageDestroy(image);
  remove(path.c_str());
}
