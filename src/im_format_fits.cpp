/** \file
 * \brief FITS - Flexible Image Transport System
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>

#include <vector>

#include <fitsio.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_fits.h"
#include "im_util.h"
#include "im_counter.h"

/* The astronomy community's interchange format, over NASA's cfitsio.

   A FITS file is a sequence of header/data units. Any of them may hold an
   N-dimensional array, and a 2D image is often not in the first -- a common
   layout is an empty primary HDU followed by image extensions -- so Open
   walks every HDU and lists each 2D plane it finds as one IM image:

     NAXIS = 2                 one gray image
     NAXIS = 3, NAXIS3 = 3     one RGB image, the planes as R, G, B
     NAXIS >= 3, otherwise     one gray image per plane

   Two things line up with IM without any work: FITS stores the bottom row
   first, which is IM's own order, and an RGB cube is planar, which is IM's
   own layout. So neither IM_TOPDOWN nor IM_PACKED appears here.

   Files are opened with fits_open_diskfile and created with
   fits_create_diskfile, not their plain counterparts: those parse cfitsio's
   extended file name syntax, where brackets select HDUs, a leading '!' means
   overwrite and "mem://" is not a file at all. A path is a path here. */

static const char* iFITSCompTable[5] =
{
  "NONE", "GZIP", "RICE", "HCOMPRESS", "PLIO"
};

struct iFITSImage
{
  int hdu;                     /* 1-based, as cfitsio counts */
  long plane;                  /* 0-based index over axes 3..N */
  int is_rgb;
};


/*******************************************************************/


class imFileFormatFITS: public imFileFormatBase
{
  fitsfile* fits;
  std::vector<iFITSImage> images;
  int current;                 /* index into images, from ReadImageInfo */
  int datatype;                /* cfitsio's T* code for the pixel buffer */
  int naxis;
  long naxes[9];

  int OpenFile(const char* file_name);
  void SetKeywordAttributes();

public:
  imFileFormatFITS(const imFormat* _iformat)
    : imFileFormatBase(_iformat), fits(NULL), current(-1), datatype(TBYTE),
      naxis(0) { memset(this->naxes, 0, sizeof(this->naxes)); }
  ~imFileFormatFITS() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatFITS: public imFormat
{
public:
  imFormatFITS()
    :imFormat("FITS",
              "Flexible Image Transport System",
              "*.fits;*.fit;*.fts;",
              iFITSCompTable,
              5,
              1)
    { extra = "cfitsio"; }
  ~imFormatFITS() {}

  imFileFormatBase* Create(void) const { return new imFileFormatFITS(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterFITS(void)
{
  imFormatRegister(new imFormatFITS());
}


/*******************************************************************/


static int iFITSError(int status)
{
  switch (status)
  {
  case 0:
    return IM_ERR_NONE;
  case FILE_NOT_OPENED:
  case FILE_NOT_CREATED:
    return IM_ERR_OPEN;
  case MEMORY_ALLOCATION:
    return IM_ERR_MEM;
  case DATA_COMPRESSION_ERR:
  case DATA_DECOMPRESSION_ERR:
    return IM_ERR_COMPRESS;
  default:
    return IM_ERR_ACCESS;
  }
}

int imFileFormatFITS::Open(const char* file_name)
{
  /* imFileOpen deletes a driver whose Open failed without calling Close, so
     every failure has to release what it got this far. */
  int error = OpenFile(file_name);
  if (error != IM_ERR_NONE)
    Close();
  return error;
}

int imFileFormatFITS::OpenFile(const char* file_name)
{
  int status = 0, hdu_count = 0;
  char signature[9];

  /* Every FITS file begins with this keyword, padded to column 9. Checked
     before cfitsio sees the file, which would otherwise try to parse every
     JPEG imFileOpen offers it. */
  FILE* file = fopen(file_name, "rb");
  if (!file)
    return IM_ERR_OPEN;
  size_t got = fread(signature, 1, sizeof(signature), file);
  fclose(file);
  if (got != sizeof(signature) || memcmp(signature, "SIMPLE  =", 9) != 0)
    return IM_ERR_FORMAT;

  if (fits_open_diskfile(&this->fits, file_name, READONLY, &status))
  {
    this->fits = NULL;
    return IM_ERR_FORMAT;
  }

  if (fits_get_num_hdus(this->fits, &hdu_count, &status))
    return iFITSError(status);

  for (int hdu = 1; hdu <= hdu_count; hdu++)
  {
    int type = 0, naxis = 0;
    long naxes[9] = { 0 };

    if (fits_movabs_hdu(this->fits, hdu, &type, &status))
      return iFITSError(status);

    /* Tile-compressed images live in binary tables, but cfitsio presents
       them as IMAGE_HDU, so they need no case of their own. */
    if (type != IMAGE_HDU)
      continue;

    if (fits_get_img_param(this->fits, 9, NULL, &naxis, naxes, &status))
    {
      /* More than nine axes. Not an image anyone means; skip it. */
      status = 0;
      continue;
    }

    if (naxis < 2 || naxes[0] <= 0 || naxes[1] <= 0)
      continue;

    if (naxis == 3 && naxes[2] == 3)
    {
      iFITSImage image = { hdu, 0, 1 };
      this->images.push_back(image);
      continue;
    }

    long planes = 1;
    for (int a = 2; a < naxis; a++)
      planes *= naxes[a];

    /* A cube of a million planes is a dataset, not a sequence of images;
       listing it plane by plane would only make imFileOpen slow. */
    if (planes <= 0 || planes > 65536)
      continue;

    for (long p = 0; p < planes; p++)
    {
      iFITSImage image = { hdu, p, 0 };
      this->images.push_back(image);
    }
  }

  if (this->images.empty())
    return IM_ERR_DATA;   /* a FITS file, but tables only */

  this->image_count = (int)this->images.size();
  strcpy(this->compression, "NONE");
  return IM_ERR_NONE;
}

int imFileFormatFITS::New(const char* file_name)
{
  int status = 0;

  /* fits_create_diskfile refuses to replace a file, which no other IM driver
     does. */
  remove(file_name);

  if (fits_create_diskfile(&this->fits, file_name, &status))
  {
    this->fits = NULL;
    return IM_ERR_OPEN;
  }

  this->image_count = 0;
  return IM_ERR_NONE;
}

void imFileFormatFITS::Close()
{
  if (this->fits)
  {
    int status = 0;
    fits_close_file(this->fits, &status);
    this->fits = NULL;
  }
}

void* imFileFormatFITS::Handle(int index)
{
  if (index == 0)
    return (void*)this->fits;
  return NULL;
}

/* The header keywords most often wanted alongside the pixels. Each becomes
   an attribute of the same name when present. */
void imFileFormatFITS::SetKeywordAttributes()
{
  static const char* strings[] = { "OBJECT", "TELESCOP", "INSTRUME", "OBSERVER",
                                   "DATE-OBS", "FILTER", "BUNIT" };
  static const char* numbers[] = { "EXPTIME", "EQUINOX", "AIRMASS" };

  imAttribTable* attrib_table = AttribTable();

  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
  {
    char value[FLEN_VALUE];
    int status = 0;
    if (fits_read_key(this->fits, TSTRING, strings[i], value, NULL, &status) == 0)
      attrib_table->Set(strings[i], IM_BYTE, (int)strlen(value) + 1, value);
  }

  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
  {
    double value = 0;
    int status = 0;
    if (fits_read_key(this->fits, TDOUBLE, numbers[i], &value, NULL, &status) == 0)
    {
      float f = (float)value;
      attrib_table->Set(numbers[i], IM_FLOAT, 1, &f);
    }
  }
}

int imFileFormatFITS::ReadImageInfo(int index)
{
  int status = 0, type = 0, equivalent = 0;

  if (index < 0 || index >= (int)this->images.size())
    return IM_ERR_DATA;

  const iFITSImage& image = this->images[(size_t)index];
  this->current = index;

  if (fits_movabs_hdu(this->fits, image.hdu, &type, &status) ||
      fits_get_img_param(this->fits, 9, NULL, &this->naxis, this->naxes, &status) ||
      fits_get_img_equivtype(this->fits, &equivalent, &status))
    return iFITSError(status);

  if (this->naxes[0] > 0x7FFFFFFF || this->naxes[1] > 0x7FFFFFFF)
    return IM_ERR_DATA;
  this->width = (int)this->naxes[0];
  this->height = (int)this->naxes[1];

  /* The equivalent type folds in BZERO and BSCALE: 16 bit data with
     BZERO = 32768 is unsigned, and any non-integer scaling makes it float.
     cfitsio applies the scaling as it reads into the type asked for. */
  switch (equivalent)
  {
  case BYTE_IMG:     this->file_data_type = IM_BYTE;   this->datatype = TBYTE;   break;
  case SBYTE_IMG:
  case SHORT_IMG:    this->file_data_type = IM_SHORT;  this->datatype = TSHORT;  break;
  case USHORT_IMG:   this->file_data_type = IM_USHORT; this->datatype = TUSHORT; break;
  case LONG_IMG:     this->file_data_type = IM_INT;    this->datatype = TINT;    break;
  case FLOAT_IMG:    this->file_data_type = IM_FLOAT;  this->datatype = TFLOAT;  break;
  /* IM has no 64 bit or unsigned 32 bit integer: double holds every
     unsigned 32 bit value exactly, and 64 bit ones to 53 bits. */
  case ULONG_IMG:
  case LONGLONG_IMG:
  case ULONGLONG_IMG:
  case DOUBLE_IMG:   this->file_data_type = IM_DOUBLE; this->datatype = TDOUBLE; break;
  default:
    return IM_ERR_DATA;
  }

  this->file_color_mode = image.is_rgb? IM_RGB: IM_GRAY;

  {
    /* From the HDU's own ZCMPTYPE keyword. fits_get_compression_type looks
       like the call for this and is not: it returns the setting for the
       next image *written* through this handle, which for a file opened to
       read is always none. */
    const char* name = "NONE";
    status = 0;
    if (fits_is_compressed_image(this->fits, &status))
    {
      char type_name[FLEN_VALUE] = "";
      status = 0;
      fits_read_key(this->fits, TSTRING, "ZCMPTYPE", type_name, NULL, &status);
      if (strncmp(type_name, "GZIP", 4) == 0)           name = "GZIP";
      else if (strcmp(type_name, "RICE_1") == 0 ||
               strcmp(type_name, "RICE_ONE") == 0)      name = "RICE";
      else if (strcmp(type_name, "HCOMPRESS_1") == 0)   name = "HCOMPRESS";
      else if (strcmp(type_name, "PLIO_1") == 0)        name = "PLIO";
    }
    strcpy(this->compression, name);
  }

  SetKeywordAttributes();

  return IM_ERR_NONE;
}

int imFileFormatFITS::ReadImageData(void* data)
{
  if (!this->fits || this->current < 0)
    return IM_ERR_ACCESS;

  const iFITSImage& image = this->images[(size_t)this->current];
  long fpixel[9];
  int count = imFileLineBufferCount(this);
  int lin = 0, plane = 0;

  imCounterTotal(this->counter, count, "Reading FITS...");

  for (int i = 0; i < count; i++)
  {
    int status = 0, anynul = 0;

    /* fpixel is 1-based, one coordinate per axis. Axes past the second
       select the plane: for RGB that is the colour plane IM is filling, for
       a cube the plane this image was listed as. */
    fpixel[0] = 1;
    fpixel[1] = lin + 1;
    long rest = image.is_rgb? plane: image.plane;
    for (int a = 2; a < this->naxis; a++)
    {
      fpixel[a] = (rest % this->naxes[a]) + 1;
      rest /= this->naxes[a];
    }

    if (fits_read_pix(this->fits, this->datatype, fpixel, this->width, NULL,
                      this->line_buffer, &anynul, &status))
      return iFITSError(status);

    imFileLineBufferRead(this, data, lin, plane);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;

    imFileLineBufferInc(this, &lin, &plane);
  }

  return IM_ERR_NONE;
}

int imFileFormatFITS::WriteImageInfo()
{
  int status = 0, bitpix;
  int color_space = imColorModeSpace(this->user_color_mode);

  if (color_space != IM_GRAY && color_space != IM_RGB)
    return IM_ERR_DATA;

  this->file_data_type = this->user_data_type;
  switch (this->file_data_type)
  {
  case IM_BYTE:   bitpix = BYTE_IMG;   this->datatype = TBYTE;   break;
  case IM_SHORT:  bitpix = SHORT_IMG;  this->datatype = TSHORT;  break;
  case IM_USHORT: bitpix = USHORT_IMG; this->datatype = TUSHORT; break;  /* 16 bit, BZERO 32768 */
  case IM_INT:    bitpix = LONG_IMG;   this->datatype = TINT;    break;
  case IM_FLOAT:  bitpix = FLOAT_IMG;  this->datatype = TFLOAT;  break;
  case IM_DOUBLE: bitpix = DOUBLE_IMG; this->datatype = TDOUBLE; break;
  default:
    return IM_ERR_DATA;
  }

  /* Planar and bottom row first are FITS's own layout, so no flags. Alpha
     has no FITS meaning and is dropped. */
  this->file_color_mode = color_space;

  if (this->compression[0] == 0)
    strcpy(this->compression, "NONE");

  int is_float = (this->file_data_type == IM_FLOAT || this->file_data_type == IM_DOUBLE);
  if (imStrEqual(this->compression, "GZIP"))
    fits_set_compression_type(this->fits, GZIP_1, &status);
  else if (imStrEqual(this->compression, "RICE") && !is_float)
    fits_set_compression_type(this->fits, RICE_1, &status);
  else if (!imStrEqual(this->compression, "NONE"))
    return IM_ERR_COMPRESS;

  /* cfitsio quantizes floating point before compressing unless told not
     to, which would make GZIP lossy. Level 0 keeps every bit. */
  if (is_float && !imStrEqual(this->compression, "NONE"))
    fits_set_quantize_level(this->fits, 0.0f, &status);

  long naxes[3] = { this->width, this->height, 3 };
  int naxis = (color_space == IM_RGB)? 3: 2;

  if (status || fits_create_img(this->fits, bitpix, naxis, naxes, &status))
    return iFITSError(status);

  this->naxis = naxis;
  this->naxes[0] = naxes[0];
  this->naxes[1] = naxes[1];
  this->naxes[2] = naxes[2];
  this->image_count = 1;

  return IM_ERR_NONE;
}

int imFileFormatFITS::WriteImageData(void* data)
{
  long fpixel[3];
  int count = imFileLineBufferCount(this);
  int lin = 0, plane = 0;

  imCounterTotal(this->counter, count, "Writing FITS...");

  for (int i = 0; i < count; i++)
  {
    int status = 0;

    imFileLineBufferWrite(this, data, lin, plane);

    fpixel[0] = 1;
    fpixel[1] = lin + 1;
    fpixel[2] = plane + 1;

    if (fits_write_pix(this->fits, this->datatype, fpixel, this->width,
                       this->line_buffer, &status))
      return iFITSError(status);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;

    imFileLineBufferInc(this, &lin, &plane);
  }

  return IM_ERR_NONE;
}

int imFormatFITS::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_GRAY && color_space != IM_RGB)
    return IM_ERR_DATA;

  if (data_type != IM_BYTE && data_type != IM_SHORT && data_type != IM_USHORT &&
      data_type != IM_INT && data_type != IM_FLOAT && data_type != IM_DOUBLE)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (imStrEqual(compression, "NONE") || imStrEqual(compression, "GZIP"))
    return IM_ERR_NONE;

  /* Rice is an integer coder; on floating point cfitsio would quantize. */
  if (imStrEqual(compression, "RICE") &&
      data_type != IM_FLOAT && data_type != IM_DOUBLE)
    return IM_ERR_NONE;

  return IM_ERR_COMPRESS;
}
