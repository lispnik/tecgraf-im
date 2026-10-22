/* No driver may write memory it did not set.
 *
 * A byte a driver writes without having set it is whatever that memory held
 * before: on the stack, part of an earlier call's frame; on the heap, part of
 * a buffer the process freed. Either can be anything the process has handled
 * -- a password, a key, another image -- and it leaves the machine inside an
 * image file that looks entirely correct. Decoding such a file shows nothing:
 * the stray bytes are row padding, unused palette entries or flags, which a
 * reader skips or which happen to decode the same. So these compare the
 * files, not the images.
 *
 * Every case -- each format, compression and kind of image a driver will
 * write -- is written twice with the memory under it filled differently, and
 * the two files must be identical byte for byte. A byte the driver set is the
 * same both times; a byte it did not set comes from the fill, and differs.
 *
 *   - The stack is filled here, in one process: with 0x00 before one write
 *     and 0xAA before the other.
 *   - The heap cannot be, not reliably. An allocator need not hand a driver
 *     the block this file just freed -- macOS clears small blocks as they are
 *     freed, and its newer allocator keeps apart the blocks of different
 *     callers -- so a heap filled from here may never reach the driver. The
 *     allocator's own fill does reach it, but it fills every allocation in a
 *     process with the same byte. So CTest writes every case in two
 *     processes, one with the allocator filling new blocks (MallocScribble
 *     on macOS -- MallocPreScribble alone does nothing there as of macOS 26
 *     -- and MALLOC_PERTURB_ on glibc) and one without, and a third
 *     compares them: the "uninit.heap" tests in CMakeLists.txt, which run the
 *     two cases below marked skip. Windows has no such fill, so there the
 *     heap comparison passes without proving anything.
 *
 * What these caught, all fixed with them:
 *   - imFileLineBufferInit left the line buffer as realloc returned it, and
 *     BMP, ICO, PCX and RAS write each line's padding from there (heap);
 *   - the PCX writer wrote a 256-entry palette from a stack array it filled
 *     only palette_count entries of (stack);
 *   - the GIF writer took "interlaced" from a field nothing set, so an image
 *     came out interlaced or not according to the heap (heap).
 *
 * Not caught here, but fixed with them: the SGI writer's RLE tables went out
 * as malloc left them, as placeholders rewritten only once every line had
 * been, so a write that failed part way left heap memory in the file.
 *
 * The images are 37x23 so that no line is a multiple of 2, 4 or 8 bytes, and
 * the indexed one uses 5 colours of 256 so that a palette always has unused
 * entries.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_image.h>
#include <im_util.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#define IM_TEST_NOINLINE __declspec(noinline)
#else
#define IM_TEST_NOINLINE __attribute__((noinline))
#endif

namespace {

const int W = 37;
const int H = 23;

std::string scratch(const std::string& name)
{
  return std::string(IM_TEST_OUTPUT_DIR) + "/" + name;
}

/* Fills the stack below the caller with PATTERN. A volatile store each byte:
   a memset into an array about to go out of scope is a dead store the
   optimiser may remove, and then nothing is filled at all. 256 KB is far more
   than any driver's frames and well inside the 1 MB Windows gives the main
   thread. */
IM_TEST_NOINLINE void soil_stack(unsigned char pattern)
{
  unsigned char buffer[256 * 1024];
  volatile unsigned char* p = buffer;
  for (size_t i = 0; i < sizeof(buffer); i++)
    p[i] = pattern;
}

std::vector<unsigned char> file_bytes(const std::string& path)
{
  std::vector<unsigned char> bytes;
  FILE* file = fopen(path.c_str(), "rb");
  if (!file)
    return bytes;
  unsigned char block[4096];
  size_t got;
  while ((got = fread(block, 1, sizeof(block), file)) > 0)
    bytes.insert(bytes.end(), block, block + got);
  fclose(file);
  return bytes;
}

struct Kind
{
  const char* name;
  int color_space;
  int data_type;
};

const Kind kinds[] = {
  {"rgb",    IM_RGB,    IM_BYTE},
  {"gray",   IM_GRAY,   IM_BYTE},
  {"map",    IM_MAP,    IM_BYTE},
  {"binary", IM_BINARY, IM_BYTE},
  {"gray16", IM_GRAY,   IM_USHORT},
};

imImage* make_image(const Kind& kind)
{
  imImage* image = imImageCreate(W, H, kind.color_space, kind.data_type);
  for (int p = 0; p < image->depth; p++)
  {
    for (int i = 0; i < W * H; i++)
    {
      int v = (i * 7 + p * 53);
      if (kind.color_space == IM_BINARY)
        ((imbyte**)image->data)[p][i] = (imbyte)((i / 3) & 1);
      else if (kind.color_space == IM_MAP)
        ((imbyte**)image->data)[p][i] = (imbyte)(i % 5);
      else if (kind.data_type == IM_BYTE)
        ((imbyte**)image->data)[p][i] = (imbyte)(v & 0xFF);
      else
        ((imushort**)image->data)[p][i] = (imushort)((v * 313) & 0xFFFF);
    }
  }

  if (kind.color_space == IM_MAP)
  {
    for (int c = 0; c < 5; c++)
      image->palette[c] = imColorEncode((imbyte)(c * 50), (imbyte)(255 - c * 40),
                                        (imbyte)(c * 20 + 7));
    image->palette_count = 5;
  }
  return image;
}

struct Case
{
  std::string format;
  std::string compression;
  const Kind* kind;
  std::string name;       /* a file name, unique to the case */
};

/* Every format, compression and kind this build will write. The same list,
   in the same order, in every process, which is what lets the heap
   comparison match one process's files with the other's. */
std::vector<Case> all_cases()
{
  char* formats[50];
  int format_count = 0;
  imFormatList(formats, &format_count);

  /* imFormatList and imFormatCompressions answer pointers into static
     buffers, so the names are copied before anything else can reuse them. */
  std::vector<std::string> format_names(formats, formats + format_count);

  std::vector<Case> cases;
  for (const std::string& format : format_names)
  {
    for (const Kind& kind : kinds)
    {
      char* comps[50];
      int comp_count = 0;
      if (imFormatCompressions(format.c_str(), comps, &comp_count,
                               kind.color_space, kind.data_type) != IM_ERR_NONE)
        continue;
      std::vector<std::string> compressions(comps, comps + comp_count);

      for (const std::string& compression : compressions)
      {
        if (imFormatCanWriteImage(format.c_str(), compression.c_str(),
                                  kind.color_space, kind.data_type) != IM_ERR_NONE)
          continue;
        std::string name = "uninit-" + format + "-" + compression + "-" + kind.name;
        for (char& c : name)
          if (c == ' ' || c == '/')
            c = '_';
        cases.push_back(Case{format, compression, &kind, name});
      }
    }
  }
  return cases;
}

/* Fills the stack with PATTERN and writes CASE's image to PATH. The filling
   is done from here, one frame above the save, so the stack it fills is the
   stack the save and the driver run on. */
int write_case(const Case& c, const std::string& path, unsigned char pattern)
{
  imImage* image = make_image(*c.kind);
  remove(path.c_str());
  soil_stack(pattern);

  int error;
  imFile* file = imFileNew(path.c_str(), c.format.c_str(), &error);
  if (file)
  {
    imFileSetInfo(file, c.compression.c_str());
    error = imFileSaveImage(file, image);
    imFileClose(file);
  }
  imImageDestroy(image);
  return error;
}

std::string first_difference(const std::vector<unsigned char>& a,
                             const std::vector<unsigned char>& b)
{
  char text[128];
  if (a.size() != b.size())
  {
    snprintf(text, sizeof(text), "sizes %zu and %zu", a.size(), b.size());
    return text;
  }
  for (size_t i = 0; i < a.size(); i++)
  {
    if (a[i] != b[i])
    {
      snprintf(text, sizeof(text), "byte %zu of %zu: 0x%02X and 0x%02X",
               i, a.size(), a[i], b[i]);
      return text;
    }
  }
  return "none";
}

/* Checks that two writes of C, at paths A and B, made the same file. */
void check_same(const Case& c, const std::string& a, const std::string& b)
{
  CAPTURE(c.format);
  CAPTURE(c.compression);
  std::string kind = c.kind->name;
  CAPTURE(kind);
  std::vector<unsigned char> bytes_a = file_bytes(a);
  std::vector<unsigned char> bytes_b = file_bytes(b);
  REQUIRE(!bytes_a.empty());
  INFO("first difference: " << first_difference(bytes_a, bytes_b));
  CHECK(bytes_a == bytes_b);
}

std::string directory_from(const char* variable)
{
  const char* value = getenv(variable);
  return value ? value : "";
}

} /* namespace */


TEST_CASE("uninit: no driver writes stack memory it did not set")
{
  std::vector<Case> cases = all_cases();
  REQUIRE(!cases.empty());

  int compared = 0;
  for (const Case& c : cases)
  {
    std::string zeros = scratch(c.name + "-00");
    std::string soiled = scratch(c.name + "-AA");
    int error0 = write_case(c, zeros, 0x00);
    int errorA = write_case(c, soiled, 0xAA);
    CHECK(error0 == errorA);
    if (error0 != IM_ERR_NONE)
      continue;
    check_same(c, zeros, soiled);
    compared++;
  }

  MESSAGE(compared << " format, compression and image combinations compared");
  CHECK(compared > 0);
}

/* The heap comparison's two writing halves: each run of this writes every
   case into $IM_TEST_UNINIT_DIR. CTest runs it twice, with and without the
   allocator's fill; see the top of the file. */
TEST_CASE("uninit: write every case for the heap comparison" * doctest::skip())
{
  std::string directory = directory_from("IM_TEST_UNINIT_DIR");
  REQUIRE_MESSAGE(!directory.empty(), "run by CTest, which sets IM_TEST_UNINIT_DIR");

  int written = 0;
  for (const Case& c : all_cases())
    if (write_case(c, directory + "/" + c.name, 0x00) == IM_ERR_NONE)
      written++;
  CHECK(written > 0);
}

TEST_CASE("uninit: no driver writes heap memory it did not set" * doctest::skip())
{
  std::string plain = directory_from("IM_TEST_UNINIT_PLAIN");
  std::string filled = directory_from("IM_TEST_UNINIT_FILLED");
  REQUIRE_MESSAGE((!plain.empty() && !filled.empty()),
                  "run by CTest, which sets IM_TEST_UNINIT_PLAIN and _FILLED");

  int compared = 0;
  for (const Case& c : all_cases())
  {
    std::string a = plain + "/" + c.name;
    std::string b = filled + "/" + c.name;
    bool in_a = !file_bytes(a).empty();
    bool in_b = !file_bytes(b).empty();
    CAPTURE(c.name);
    CHECK(in_a == in_b);
    if (!in_a || !in_b)
      continue;
    check_same(c, a, b);
    compared++;
  }

  MESSAGE(compared << " format, compression and image combinations compared");
  CHECK(compared > 0);
}
