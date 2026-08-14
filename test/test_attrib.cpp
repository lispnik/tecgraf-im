/* Tests for the attribute table / array in src/im_attrib.cpp.
 *
 * Every case here asserts real behaviour; none are currently inverted. The
 * convention for a bug you are not fixing yet is to write the case asserting
 * the *correct* behaviour and decorate it `* doctest::should_fail()`, so the
 * suite stays green while the bug is open and the case starts reporting a
 * failure the moment someone fixes it -- the signal to delete the decorator,
 * not to re-invert it. Cases below marked "Regression:" were written that way
 * first and de-inverted once the fix landed.
 */

#include "doctest/doctest.h"

#include <im.h>
#include <im_attrib.h>

#include <string.h>
#include <string>

/* ------------------------------------------------------------------ *
 * Baseline behaviour -- these must pass today and keep passing.
 * ------------------------------------------------------------------ */

TEST_CASE("imAttribTable: set/get round-trip preserves type, count and data")
{
  imAttribTable table(0);   /* 0 selects the default hash size of 101 */
  const int values[3] = { 7, 8, 9 };

  table.Set("triple", IM_INT, 3, values);
  CHECK(table.Count() == 1);

  int data_type = -1, count = -1;
  const void* data = table.Get("triple", &data_type, &count);

  REQUIRE(data != NULL);
  CHECK(data_type == IM_INT);
  CHECK(count == 3);
  CHECK(memcmp(data, values, sizeof(values)) == 0);
  CHECK(data != (const void*)values);          /* must be a private copy */
}

TEST_CASE("imAttribTable: Get returns NULL for a name that was never set")
{
  imAttribTable table(0);
  CHECK(table.Get("absent") == NULL);

  table.Set("present", IM_INT, 1, NULL);
  CHECK(table.Get("absent") == NULL);
}

TEST_CASE("imAttribTable: NULL data zero-fills the allocation")
{
  imAttribTable table(0);
  table.Set("zeros", IM_INT, 4, NULL);

  int count = 0;
  const int* data = (const int*)table.Get("zeros", NULL, &count);

  REQUIRE(data != NULL);
  REQUIRE(count == 4);
  for (int i = 0; i < count; i++)
    CHECK(data[i] == 0);
}

TEST_CASE("imAttribTable: setting an existing name replaces without growing Count")
{
  imAttribTable table(0);
  const int first = 1, second = 2;

  table.Set("key", IM_INT, 1, &first);
  table.Set("key", IM_INT, 1, &second);

  CHECK(table.Count() == 1);
  CHECK(table.GetInteger("key") == 2);
}

TEST_CASE("imAttribTable: UnSet removes only the named attribute")
{
  imAttribTable table(0);
  table.Set("a", IM_INT, 1, NULL);
  table.Set("b", IM_INT, 1, NULL);

  table.UnSet("a");

  CHECK(table.Count() == 1);
  CHECK(table.Get("a") == NULL);
  CHECK(table.Get("b") != NULL);

  table.UnSet("does-not-exist");            /* must be a harmless no-op */
  CHECK(table.Count() == 1);
}

TEST_CASE("imAttribTable: names colliding in the same bucket stay independent")
{
  /* A 1-bucket table forces every name onto the same chain, exercising the
   * first-node / mid-chain branches in Set and UnSet. */
  imAttribTable table(1);
  const int a = 10, b = 20, c = 30;

  table.Set("first",  IM_INT, 1, &a);
  table.Set("second", IM_INT, 1, &b);
  table.Set("third",  IM_INT, 1, &c);
  REQUIRE(table.Count() == 3);

  CHECK(table.GetInteger("first")  == 10);
  CHECK(table.GetInteger("second") == 20);
  CHECK(table.GetInteger("third")  == 30);

  table.UnSet("second");                    /* mid-chain removal */
  CHECK(table.Count() == 2);
  CHECK(table.Get("second") == NULL);
  CHECK(table.GetInteger("first") == 10);
  CHECK(table.GetInteger("third") == 30);

  table.UnSet("third");                     /* head-of-chain removal */
  CHECK(table.Count() == 1);
  CHECK(table.GetInteger("first") == 10);
}

TEST_CASE("imAttribTable: count -1 on IM_BYTE means NUL-terminated string")
{
  imAttribTable table(0);
  table.Set("unit", IM_BYTE, -1, "DPI");

  int count = 0;
  const char* data = (const char*)table.Get("unit", NULL, &count);

  REQUIRE(data != NULL);
  CHECK(count == 4);                        /* 3 chars + terminator */
  CHECK(strcmp(data, "DPI") == 0);
  CHECK(strcmp(table.GetString("unit"), "DPI") == 0);
}

TEST_CASE("imAttribTable: GetString rejects byte data with no terminator")
{
  imAttribTable table(0);
  const char raw[3] = { 'a', 'b', 'c' };    /* deliberately unterminated */

  table.Set("raw", IM_BYTE, 3, raw);
  CHECK(table.GetString("raw") == NULL);
  CHECK(table.GetString("missing") == NULL);
}

TEST_CASE("imAttribTable: numeric accessors reject out-of-range indices")
{
  imAttribTable table(0);
  const int values[2] = { 5, 6 };
  table.Set("pair", IM_INT, 2, values);

  CHECK(table.GetInteger("pair", 0) == 5);
  CHECK(table.GetInteger("pair", 1) == 6);
  CHECK(table.GetInteger("pair", 2) == 0);      /* past the end */
  CHECK(table.GetInteger("pair", -1) == 0);
  CHECK(table.GetInteger("absent", 0) == 0);

  CHECK(table.GetReal("pair", 0) == doctest::Approx(5.0));
  CHECK(table.GetReal("pair", 2) == doctest::Approx(0.0));
}

TEST_CASE("imAttribTable: RemoveAll empties the table but keeps it usable")
{
  imAttribTable table(0);
  table.Set("a", IM_INT, 1, NULL);
  table.Set("b", IM_INT, 1, NULL);

  table.RemoveAll();
  CHECK(table.Count() == 0);
  CHECK(table.Get("a") == NULL);

  table.Set("c", IM_INT, 1, NULL);          /* still writable afterwards */
  CHECK(table.Count() == 1);
  CHECK(table.Get("c") != NULL);
}

TEST_CASE("imAttribTable: CopyFrom overwrites, MergeFrom preserves")
{
  imAttribTable src(0), dst(0);
  const int one = 1, two = 2;

  src.Set("shared", IM_INT, 1, &one);
  src.Set("only-in-src", IM_INT, 1, &one);
  dst.Set("shared", IM_INT, 1, &two);

  SUBCASE("CopyFrom replaces colliding names")
  {
    dst.CopyFrom(src);
    CHECK(dst.GetInteger("shared") == 1);
    CHECK(dst.GetInteger("only-in-src") == 1);
  }

  SUBCASE("MergeFrom keeps the existing value")
  {
    dst.MergeFrom(src);
    CHECK(dst.GetInteger("shared") == 2);
    CHECK(dst.GetInteger("only-in-src") == 1);
  }
}

/* ------------------------------------------------------------------ *
 * Known upstream bugs. See the review notes above each case.
 * ------------------------------------------------------------------ */

TEST_CASE("imAttribArray: RemoveAll clears the slots but keeps the array usable")
{
  /* Regression: imAttribArrayCreate stores the array *capacity* in the same
   * `count` field that imAttribTable uses for its element count, and
   * imAttribTableRemoveAll used to zero it unconditionally. That left every
   * imAttribArraySet failing its index guard and every imAttribArrayGet
   * returning NULL -- silently, and permanently. */
  imAttribArray array(4);
  const int first = 42, second = 43;

  array.Set(0, "alpha", IM_INT, 1, &first);
  array.Set(2, "gamma", IM_INT, 1, &first);
  REQUIRE(array.Get(0) != NULL);
  REQUIRE(array.Get(2) != NULL);

  array.RemoveAll();

  CHECK(array.Count() == 4);                /* capacity survives */
  CHECK(array.Get(0) == NULL);              /* contents do not */
  CHECK(array.Get(2) == NULL);

  /* Every slot must be writable again, not just the ones used before. */
  for (int i = 0; i < 4; i++)
  {
    array.Set(i, "beta", IM_INT, 1, &second);
    const int* data = (const int*)array.Get(i);
    REQUIRE(data != NULL);
    CHECK(*data == 43);
  }

  array.RemoveAll();                        /* and it must survive a second round */
  CHECK(array.Count() == 4);
  array.Set(3, "delta", IM_INT, 1, &first);
  CHECK(array.Get(3) != NULL);
}

TEST_CASE("imAttribTable: RemoveAll still zeroes Count for a hash table")
{
  /* The array fix must not change table semantics: for imAttribTable, Count
   * really is the element count and RemoveAll really should zero it. */
  imAttribTable table(0);
  table.Set("a", IM_INT, 1, NULL);
  table.Set("b", IM_INT, 1, NULL);
  REQUIRE(table.Count() == 2);

  table.RemoveAll();
  CHECK(table.Count() == 0);
}

TEST_CASE("imAttribTable: an unusable count or type yields no attribute")
{
  /* Regression: imAttribNode used to compute `int size = count * elem_size`
   * with no validation, then malloc/memcpy through the result.
   *
   *   - count == -1 with any type other than IM_BYTE (and IM_BYTE with NULL
   *     data, which misses the string shorthand) produced a negative size,
   *     so malloc returned NULL and the memset wrote through it. Both used to
   *     segfault; they were the two probes in test_attrib_isolated.cpp.
   *   - an out-of-range data_type reached imDataTypeSize, which asserts but
   *     then indexes its table anyway -- an out-of-bounds read in a release
   *     build.
   *
   * All of these must now leave the attribute absent rather than crash. */
  imAttribTable table(0);

  SUBCASE("count -1 on a non-BYTE type")
  {
    table.Set("x", IM_INT, -1, NULL);
    CHECK(table.Get("x") == NULL);
  }

  SUBCASE("count -1 on IM_BYTE with NULL data misses the string shorthand")
  {
    table.Set("x", IM_BYTE, -1, NULL);
    CHECK(table.Get("x") == NULL);
  }

  SUBCASE("an arbitrary negative count")
  {
    table.Set("x", IM_INT, -12345, NULL);
    CHECK(table.Get("x") == NULL);
  }

  SUBCASE("a data_type below the valid range")
  {
    table.Set("x", IM_BYTE - 1, 4, NULL);
    CHECK(table.Get("x") == NULL);
  }

  SUBCASE("a data_type above the valid range")
  {
    table.Set("x", IM_CDOUBLE + 1, 4, NULL);
    CHECK(table.Get("x") == NULL);
  }

  SUBCASE("count 0 stores nothing")
  {
    table.Set("x", IM_INT, 0, NULL);
    CHECK(table.Get("x") == NULL);
  }
}

TEST_CASE("imAttribTable: a stored count never exceeds its allocation")
{
  /* The heart of the overflow bug: the node used to keep the caller's count
   * even when the (wrapped) size computation had under-allocated, so every
   * reader walked off the end. count is now assigned only after the matching
   * allocation succeeds.
   *
   * The historical trigger -- 2^30 IM_INT elements, whose int product wraps
   * to 0 -- is not reproduced here: with the size computed in size_t it is a
   * legitimate 4 GB request, and allocating and zeroing that in CI is not
   * worth the wall clock. A modest count exercises the same invariant, and
   * reading the last element makes it meaningful under ASan. */
  imAttribTable table(0);
  const int element_count = 4096;

  table.Set("block", IM_INT, element_count, NULL);

  int data_type = -1, count = -1;
  const int* data = (const int*)table.Get("block", &data_type, &count);

  REQUIRE(data != NULL);
  CHECK(data_type == IM_INT);
  REQUIRE(count == element_count);

  /* Every index the node advertises must be inside the allocation. */
  CHECK(data[0] == 0);
  CHECK(data[count - 1] == 0);
  CHECK(table.GetInteger("block", count - 1) == 0);
}

TEST_CASE("imAttribArray: CopyFrom preserves slot positions in a sparse array")
{
  /* Regression: imAttribArrayCopyFrom used to route through
   * imAttribTableForEach, which hands its callback a sequential counter
   * rather than the slot an attribute was found in. Right for a hash table,
   * where position carries no meaning; wrong for an index-addressed array,
   * which was silently compacted -- entries at slots 0 and 3 landed at 0
   * and 1. */
  imAttribArray src(4), dst(4);
  const int first = 10, last = 40;

  src.Set(0, "first", IM_INT, 1, &first);
  src.Set(3, "last",  IM_INT, 1, &last);     /* deliberately sparse */

  dst.CopyFrom(src);

  char name[IM_ATTRIB_MAXNAME + 1] = { 0 };
  int data_type = -1, count = -1;

  const int* slot0 = (const int*)dst.Get(0, name, &data_type, &count);
  REQUIRE(slot0 != NULL);
  CHECK(*slot0 == 10);
  CHECK(strcmp(name, "first") == 0);
  CHECK(data_type == IM_INT);
  CHECK(count == 1);

  const int* slot3 = (const int*)dst.Get(3, name);
  REQUIRE(slot3 != NULL);
  CHECK(*slot3 == 40);
  CHECK(strcmp(name, "last") == 0);

  /* The gaps stay gaps -- nothing was shifted down into them. */
  CHECK(dst.Get(1) == NULL);
  CHECK(dst.Get(2) == NULL);

  /* And the copy is independent of the source. */
  CHECK(slot0 != src.Get(0));
}

TEST_CASE("imAttribArray: out-of-range indices are rejected, not dereferenced")
{
  /* Regression: imAttribArrayGet checked only for an empty array before
   * indexing hash_table[index], so an out-of-range index read past the slot
   * array and dereferenced whatever it found. imAttribArraySet had the
   * mirror-image hole: its guard tested `index >= count` only, so a negative
   * index passed and was written through. */
  imAttribArray array(2);
  const int value = 7;
  array.Set(0, "alpha", IM_INT, 1, &value);

  SUBCASE("Get below the array")
  {
    CHECK(array.Get(-1) == NULL);
    CHECK(array.Get(-100000) == NULL);
  }

  SUBCASE("Get past the end")
  {
    CHECK(array.Get(2) == NULL);
    CHECK(array.Get(100000) == NULL);
  }

  SUBCASE("Set outside the array is a no-op, not a stray write")
  {
    const int other = 9;
    array.Set(-1, "below", IM_INT, 1, &other);
    array.Set(2,  "above", IM_INT, 1, &other);
    array.Set(100000, "way-above", IM_INT, 1, &other);

    /* The in-range slot is untouched by any of that. */
    const int* slot0 = (const int*)array.Get(0);
    REQUIRE(slot0 != NULL);
    CHECK(*slot0 == 7);
    CHECK(array.Get(1) == NULL);
  }
}

TEST_CASE("imAttribArray: long names are truncated to IM_ATTRIB_MAXNAME")
{
  /* Regression: imAttribArrayGet did an unbounded strcpy into the caller's
   * buffer, and nothing capped the length of a stored name -- which can come
   * from a file, e.g. a TIFF custom-tag name via src/im_format_tiff.cpp.
   * Names are now truncated when stored, so IM_ATTRIB_MAXNAME+1 bytes is
   * always enough to receive one, which is what the header documents. */
  imAttribArray array(1);
  const int value = 1;

  std::string long_name(IM_ATTRIB_MAXNAME + 100, 'x');
  array.Set(0, long_name.c_str(), IM_INT, 1, &value);

  /* A canary after the buffer catches a copy that runs past the documented
   * size; the buffer itself is exactly what the header asks for. */
  struct { char name[IM_ATTRIB_MAXNAME + 1]; char canary[16]; } buf;
  memset(&buf, 0xAA, sizeof(buf));

  REQUIRE(array.Get(0, buf.name) != NULL);

  CHECK(strlen(buf.name) == IM_ATTRIB_MAXNAME);
  for (size_t i = 0; i < sizeof(buf.canary); i++)
    CHECK((unsigned char)buf.canary[i] == 0xAA);
}

TEST_CASE("imAttribArray: a short name writes only what it needs")
{
  /* The bounded copy must not pad out to IM_ATTRIB_MAXNAME either: callers
   * throughout this tree pass modest fixed-size buffers, and an
   * strncpy-style pad would overflow every one of them. */
  imAttribArray array(1);
  const int value = 1;
  array.Set(0, "short", IM_INT, 1, &value);

  struct { char name[8]; char canary[16]; } buf;
  memset(&buf, 0xAA, sizeof(buf));

  REQUIRE(array.Get(0, buf.name) != NULL);

  CHECK(strcmp(buf.name, "short") == 0);
  for (size_t i = 0; i < sizeof(buf.canary); i++)
    CHECK((unsigned char)buf.canary[i] == 0xAA);
}
