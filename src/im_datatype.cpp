/** \file
 * \brief Data Type Utilities
 *
 * See Copyright Notice in im_lib.h
 */


#include "im.h"
#include "im_util.h"

#include <assert.h>

typedef struct _iTypeInfo
{
  int size;
  unsigned long max;
  long min;
  const char* name;
} iTypeInfo;

static iTypeInfo iTypeInfoTable[] =  
{//size  max          min             name
  {1,    255,         0,              "byte"}, 
  {2,    32767,       -32767-1,       "short"},
  {2,    65535,       0,              "ushort"},
  {4,    2147483647,  -2147483647-1,  "int"},
  {4,    0,           0,              "float"}, 
  {8,    0,           0,              "double"}, 
  {8,    0,           0,              "cfloat"},
  {16,   0,           0,              "cdouble"}
};

/* Every accessor below used to assert and then index the table anyway.
   assert() is compiled out of every shipped build, so an out-of-range type
   became an out-of-bounds read that returned whatever happened to sit next to
   the table -- and callers acted on it. A bogus element size under-allocates a
   buffer; a size that lands on zero reaches a division.

   Out-of-range types are not hypothetical: they arrive from caller-supplied
   attributes, e.g. the RAW format's "DataType". The asserts are kept so a
   debug build still fails loudly at the point of the mistake. */
static int iDataTypeValid(int data_type)
{
  return (data_type >= IM_BYTE && data_type <= IM_CDOUBLE);
}

const char* imDataTypeName(int data_type)
{
  assert(iDataTypeValid(data_type));

  /* Never NULL: callers feed this straight to strcat and lua_pushstring. */
  if (!iDataTypeValid(data_type))
    return "(invalid)";

  return iTypeInfoTable[data_type].name;
}

int imDataTypeSize(int data_type)
{
  assert(iDataTypeValid(data_type));
  assert(sizeof(int) == 4);

  /* 0 means "no usable size". Callers that size an allocation or divide by
     this must check it; see iAttribDataSize in src/im_attrib.cpp. */
  if (!iDataTypeValid(data_type))
    return 0;

  return iTypeInfoTable[data_type].size;
}

unsigned long imDataTypeIntMax(int data_type)
{
  assert(iDataTypeValid(data_type));

  if (!iDataTypeValid(data_type))
    return 0;

  return iTypeInfoTable[data_type].max;
}

long imDataTypeIntMin(int data_type)
{
  assert(iDataTypeValid(data_type));

  if (!iDataTypeValid(data_type))
    return 0;

  return iTypeInfoTable[data_type].min;
}
