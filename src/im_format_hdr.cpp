/** \file
 * \brief HDR - Radiance RGBE Image Format
 *
 * See Copyright Notice in im_lib.h
 */

#include "im_format.h"
#include "im_format_all.h"
#include "im_util.h"
#include "im_counter.h"

#include "im_binfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <math.h>

/* Greg Ward's RGBE: three 8 bit mantissas sharing one 8 bit exponent, so
   each pixel is 4 bytes covering a huge dynamic range at about 1% precision.

   A file is a text header ending in a blank line, a resolution string, then
   one record per scanline. A scanline is stored one of three ways, and
   readers must take all of them:

     new RLE   2 2 hi lo, then each of the four components run-length coded
               on its own. Only for widths 8..32767. What everyone writes.
     flat      plain 4 byte pixels.
     old RLE   flat, except a pixel 1 1 1 n repeats the previous one n times
               (n shifted left 8 more bits for each consecutive such pixel).

   The whole pixel area is read into memory at Open, which keeps the decoder
   a bounds-checked walk over a buffer rather than a byte-at-a-time stream. */

static const char* iHDRCompTable[2] =
{
  "NONE",
  "RLE"
};

#define IM_HDR_MAX_LINE 4096

class imFileFormatHDR: public imFileFormatBase
{
  imBinFile* handle;

  unsigned char* pixels;       /* everything after the resolution string */
  size_t pixels_size;
  size_t cursor;               /* read position in pixels */

  int is_xyz;
  int is_topdown;

  /* Parsed at Open, but imFileOpen clears width and height after Open
     returns, so they are kept here until ReadImageInfo. */
  int file_width, file_height;
  float exposure;              /* product of every EXPOSURE= line, 1 if none */

  int OpenFile(const char* file_name);
  int ReadScanline(unsigned char* rgbe);
  int WriteScanline(const unsigned char* rgbe, unsigned char* scratch);

public:
  imFileFormatHDR(const imFormat* _iformat)
    : imFileFormatBase(_iformat), handle(NULL), pixels(NULL), pixels_size(0),
      cursor(0), is_xyz(0), is_topdown(1), file_width(0), file_height(0),
      exposure(1.0f) {}
  ~imFileFormatHDR() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatHDR: public imFormat
{
public:
  imFormatHDR()
    :imFormat("HDR",
              "Radiance RGBE Image Format",
              "*.hdr;*.rgbe;",
              iHDRCompTable,
              2,
              0)
    {}
  ~imFormatHDR() {}

  imFileFormatBase* Create(void) const { return new imFileFormatHDR(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterHDR(void)
{
  imFormatRegister(new imFormatHDR());
}


/*******************************************************************/


/* One header line, without its newline. Returns 0 at end of file or on a
   line longer than any real header has, which in a binary file that merely
   starts with "#?" is the likely outcome. */
static int iHDRReadLine(imBinFile* handle, char* line)
{
  int size = 0;
  unsigned char c;

  for (;;)
  {
    imBinFileRead(handle, &c, 1, 1);
    if (imBinFileError(handle))
      return 0;

    if (c == '\n')
      break;

    if (size == IM_HDR_MAX_LINE - 1)
      return 0;

    line[size++] = (char)c;
  }

  line[size] = 0;
  return 1;
}

/* Radiance's own conversion, which centres each mantissa in its bucket.
   Ward's widely copied rgbe.c drops the + 0.5; the difference is half a
   step, well inside the format's 1% precision either way. */
static void iHDRDecode(const unsigned char* rgbe, float* rgb)
{
  if (rgbe[3] == 0)
  {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    return;
  }

  double f = ldexp(1.0, (int)rgbe[3] - (128 + 8));
  rgb[0] = (float)((rgbe[0] + 0.5) * f);
  rgb[1] = (float)((rgbe[1] + 0.5) * f);
  rgb[2] = (float)((rgbe[2] + 0.5) * f);
}

static unsigned char iHDRMantissa(float value, double scale)
{
  /* RGBE has no sign. Negative values, and the NaN that fails both tests,
     become 0 rather than wrapping. */
  if (!(value > 0.0f))
    return 0;

  double m = value * scale;
  return (unsigned char)((m > 255.0)? 255: m);
}

static void iHDREncode(const float* rgb, unsigned char* rgbe)
{
  float v = rgb[0];
  if (rgb[1] > v) v = rgb[1];
  if (rgb[2] > v) v = rgb[2];

  /* Below the smallest exponent, and NaN, store black. */
  if (!(v >= 1e-32f))
  {
    rgbe[0] = rgbe[1] = rgbe[2] = rgbe[3] = 0;
    return;
  }

  /* Past the largest exponent: clamp to the brightest representable value
     instead of letting e + 128 wrap round to a tiny one. Tested before frexp,
     which gives no usable exponent for infinity. */
  int e;
  if (v >= 1.7e38f)
  {
    rgbe[0] = rgbe[1] = rgbe[2] = 255;
    rgbe[3] = 255;
    return;
  }

  double scale = frexp(v, &e) * 256.0 / v;
  if (e > 127)
  {
    rgbe[0] = rgbe[1] = rgbe[2] = 255;
    rgbe[3] = 255;
    return;
  }

  rgbe[0] = iHDRMantissa(rgb[0], scale);
  rgbe[1] = iHDRMantissa(rgb[1], scale);
  rgbe[2] = iHDRMantissa(rgb[2], scale);
  rgbe[3] = (unsigned char)(e + 128);
}

int imFileFormatHDR::Open(const char* file_name)
{
  /* imFileOpen deletes a driver whose Open failed without calling Close, so
     every failure has to release what it got this far. */
  int error = OpenFile(file_name);
  if (error != IM_ERR_NONE)
    Close();
  return error;
}

int imFileFormatHDR::OpenFile(const char* file_name)
{
  char line[IM_HDR_MAX_LINE];
  char y_sign, y_axis, x_sign, x_axis;
  int has_format = 0;

  this->handle = imBinFileOpen(file_name);
  if (!this->handle)
    return IM_ERR_OPEN;

  /* "#?RADIANCE" from Radiance itself, "#?RGBE" from Ward's rgbe.c, and other
     programs put their own name after the "#?". Only those two bytes are the
     signature. */
  {
    unsigned char sig[2];
    imBinFileRead(this->handle, sig, 2, 1);
    if (imBinFileError(this->handle) || sig[0] != '#' || sig[1] != '?')
      return IM_ERR_FORMAT;
  }

  if (!iHDRReadLine(this->handle, line))
    return IM_ERR_FORMAT;

  for (;;)
  {
    if (!iHDRReadLine(this->handle, line))
      return IM_ERR_FORMAT;

    if (line[0] == 0)
      break;                      /* the blank line ending the header */

    if (strncmp(line, "FORMAT=", 7) == 0)
    {
      if (strcmp(line + 7, "32-bit_rle_rgbe") == 0)
        this->is_xyz = 0;
      else if (strcmp(line + 7, "32-bit_rle_xyze") == 0)
        this->is_xyz = 1;
      else
        return IM_ERR_DATA;      /* a Radiance file, but not pixels we know */
      has_format = 1;
    }
    else if (strncmp(line, "EXPOSURE=", 9) == 0)
    {
      /* Cumulative by definition: each program that rescales the pixels
         appends its own line. */
      double value = atof(line + 9);
      if (value > 0)
        this->exposure *= (float)value;
    }
  }
  (void)has_format;              /* absent means RGBE, the original default */

  if (!iHDRReadLine(this->handle, line))
    return IM_ERR_FORMAT;

  /* Radiance allows all eight orientations. Two are in use: "-Y h +X w",
     which is top-down and what everything writes, and its bottom-up twin.
     The six that mirror or transpose are refused rather than read wrongly. */
  if (sscanf(line, "%c%c %d %c%c %d", &y_sign, &y_axis, &this->file_height,
             &x_sign, &x_axis, &this->file_width) != 6 ||
      y_axis != 'Y' || x_axis != 'X' || x_sign != '+' ||
      (y_sign != '-' && y_sign != '+'))
    return IM_ERR_DATA;

  if (this->file_width <= 0 || this->file_height <= 0)
    return IM_ERR_DATA;

  this->is_topdown = (y_sign == '-');

  {
    unsigned long size = imBinFileSize(this->handle);
    unsigned long position = imBinFileTell(this->handle);
    if (size <= position)
      return IM_ERR_ACCESS;      /* a header and no pixels */

    this->pixels_size = (size_t)(size - position);
    this->pixels = (unsigned char*)malloc(this->pixels_size);
    if (!this->pixels)
      return IM_ERR_MEM;

    imBinFileRead(this->handle, this->pixels, (unsigned long)this->pixels_size, 1);
    if (imBinFileError(this->handle))
      return IM_ERR_ACCESS;
  }

  imBinFileClose(this->handle);
  this->handle = NULL;

  /* Reported from the first scanline, which is what decides it in practice:
     a writer that uses the new RLE uses it everywhere it can. */
  if (this->pixels_size >= 4 && this->pixels[0] == 2 && this->pixels[1] == 2 &&
      this->file_width >= 8 && this->file_width <= 0x7FFF)
    strcpy(this->compression, "RLE");
  else
    strcpy(this->compression, "NONE");

  this->image_count = 1;

  return IM_ERR_NONE;
}

int imFileFormatHDR::New(const char* file_name)
{
  this->handle = imBinFileNew(file_name);
  if (!this->handle)
    return IM_ERR_OPEN;

  this->image_count = 1;
  return IM_ERR_NONE;
}

void imFileFormatHDR::Close()
{
  if (this->handle)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
  }

  if (this->pixels)
  {
    free(this->pixels);
    this->pixels = NULL;
  }
}

void* imFileFormatHDR::Handle(int index)
{
  if (index == 0)
    return (void*)this->handle;   /* NULL once reading has loaded the pixels */
  return NULL;
}

int imFileFormatHDR::ReadImageInfo(int index)
{
  if (index != 0)
    return IM_ERR_DATA;

  this->width = this->file_width;
  this->height = this->file_height;
  this->file_data_type = IM_FLOAT;
  this->file_color_mode = (this->is_xyz? IM_XYZ: IM_RGB) | IM_PACKED;
  if (this->is_topdown)
    this->file_color_mode |= IM_TOPDOWN;

  this->cursor = 0;

  imAttribTable* attrib_table = AttribTable();
  if (this->exposure != 1.0f)
    attrib_table->Set("Exposure", IM_FLOAT, 1, &this->exposure);

  return IM_ERR_NONE;
}

/* Decodes the next scanline into width RGBE pixels, whichever of the three
   encodings it uses. Every read is checked against the buffer: a truncated
   or hostile file ends in IM_ERR_ACCESS, never a read past the end. */
int imFileFormatHDR::ReadScanline(unsigned char* rgbe)
{
  const unsigned char* data = this->pixels;
  size_t end = this->pixels_size;
  size_t at = this->cursor;
  int width = this->width;

  if (at + 4 > end)
    return IM_ERR_ACCESS;

  if (width >= 8 && width <= 0x7FFF && data[at] == 2 && data[at + 1] == 2 &&
      (data[at + 2] & 0x80) == 0)
  {
    if (((data[at + 2] << 8) | data[at + 3]) != width)
      return IM_ERR_ACCESS;     /* a scanline claiming a different width */
    at += 4;

    for (int c = 0; c < 4; c++)
    {
      int x = 0;
      while (x < width)
      {
        if (at >= end)
          return IM_ERR_ACCESS;

        int count = data[at++];
        if (count > 128)
        {
          count -= 128;
          if (x + count > width || at >= end)
            return IM_ERR_ACCESS;

          unsigned char value = data[at++];
          for (int i = 0; i < count; i++)
            rgbe[4 * (x++) + c] = value;
        }
        else
        {
          if (count == 0 || x + count > width || at + (size_t)count > end)
            return IM_ERR_ACCESS;

          for (int i = 0; i < count; i++)
            rgbe[4 * (x++) + c] = data[at++];
        }
      }
    }
  }
  else
  {
    /* Flat, possibly with old-style runs. */
    int x = 0, shift = 0;
    while (x < width)
    {
      if (at + 4 > end)
        return IM_ERR_ACCESS;

      const unsigned char* pixel = data + at;
      at += 4;

      if (pixel[0] == 1 && pixel[1] == 1 && pixel[2] == 1)
      {
        /* A run needs a pixel before it to repeat. Shifts beyond 16 would
           overflow and no real file has a run that long. */
        long count = (long)pixel[3] << shift;
        if (x == 0 || shift > 16 || x + count > width)
          return IM_ERR_ACCESS;

        for (long i = 0; i < count; i++, x++)
          memcpy(rgbe + 4 * x, rgbe + 4 * (x - 1), 4);
        shift += 8;
      }
      else
      {
        memcpy(rgbe + 4 * x, pixel, 4);
        x++;
        shift = 0;
      }
    }
  }

  this->cursor = at;
  return IM_ERR_NONE;
}

int imFileFormatHDR::ReadImageData(void* data)
{
  if (!this->pixels)
    return IM_ERR_ACCESS;

  unsigned char* rgbe = (unsigned char*)malloc((size_t)this->width * 4);
  if (!rgbe)
    return IM_ERR_MEM;

  imCounterTotal(this->counter, this->height, "Reading HDR...");

  for (int lin = 0; lin < this->height; lin++)
  {
    int error = ReadScanline(rgbe);
    if (error != IM_ERR_NONE)
    {
      free(rgbe);
      return error;
    }

    float* line = (float*)this->line_buffer;
    for (int x = 0; x < this->width; x++)
      iHDRDecode(rgbe + 4 * x, line + 3 * x);

    imFileLineBufferRead(this, data, lin, 0);

    if (!imCounterInc(this->counter))
    {
      free(rgbe);
      return IM_ERR_COUNTER;
    }
  }

  free(rgbe);
  return IM_ERR_NONE;
}

int imFileFormatHDR::WriteImageInfo()
{
  int color_space = imColorModeSpace(this->user_color_mode);

  if (this->user_data_type != IM_FLOAT)
    return IM_ERR_DATA;
  this->file_data_type = IM_FLOAT;

  /* Gray arrives as one channel, since the write path does no colour
     conversion, and is widened to RGB per scanline. */
  if (color_space == IM_GRAY)
    this->file_color_mode = IM_GRAY;
  else if (color_space == IM_XYZ)
    this->file_color_mode = IM_XYZ | IM_PACKED;
  else if (color_space == IM_RGB)
    this->file_color_mode = IM_RGB | IM_PACKED;
  else
    return IM_ERR_DATA;

  this->file_color_mode |= IM_TOPDOWN;
  this->is_xyz = (color_space == IM_XYZ);

  if (this->compression[0] == 0 || imStrEqual(this->compression, "RLE"))
    strcpy(this->compression, "RLE");
  else if (!imStrEqual(this->compression, "NONE"))
    return IM_ERR_COMPRESS;

  imBinFilePrintf(this->handle, "#?RADIANCE\n");
  imBinFilePrintf(this->handle, "# Written by the IM library\n");
  imBinFilePrintf(this->handle, "FORMAT=%s\n",
                  this->is_xyz? "32-bit_rle_xyze": "32-bit_rle_rgbe");
  imBinFilePrintf(this->handle, "\n");
  imBinFilePrintf(this->handle, "-Y %d +X %d\n", this->height, this->width);

  if (imBinFileError(this->handle))
    return IM_ERR_ACCESS;

  return IM_ERR_NONE;
}

/* The new RLE, one component at a time: a run of 4 or more identical bytes
   becomes (128 + n, value), and everything between runs goes out as literal
   blocks of at most 128. The same scheme as Ward's RLE_WriteBytes. */
static size_t iHDREncodeComponent(const unsigned char* rgbe, int width, int c,
                                  unsigned char* out)
{
  size_t size = 0;
  int x = 0;

  while (x < width)
  {
    /* Find the next run of at least 4. */
    int run_start = x, run_count = 0;
    while (run_start < width)
    {
      run_count = 1;
      while (run_start + run_count < width && run_count < 127 &&
             rgbe[4 * (run_start + run_count) + c] == rgbe[4 * run_start + c])
        run_count++;
      if (run_count >= 4)
        break;
      run_start += run_count;
    }
    if (run_count < 4)
      run_start = width;

    /* Literals up to it. */
    while (x < run_start)
    {
      int count = run_start - x;
      if (count > 128) count = 128;
      out[size++] = (unsigned char)count;
      for (int i = 0; i < count; i++)
        out[size++] = rgbe[4 * (x + i) + c];
      x += count;
    }

    if (run_start < width)
    {
      out[size++] = (unsigned char)(128 + run_count);
      out[size++] = rgbe[4 * run_start + c];
      x = run_start + run_count;
    }
  }

  return size;
}

int imFileFormatHDR::WriteScanline(const unsigned char* rgbe, unsigned char* scratch)
{
  int width = this->width;

  if (!imStrEqual(this->compression, "RLE") || width < 8 || width > 0x7FFF)
  {
    imBinFileWrite(this->handle, (void*)rgbe, (unsigned long)width * 4, 1);
    return imBinFileError(this->handle)? IM_ERR_ACCESS: IM_ERR_NONE;
  }

  size_t size = 0;
  scratch[size++] = 2;
  scratch[size++] = 2;
  scratch[size++] = (unsigned char)(width >> 8);
  scratch[size++] = (unsigned char)(width & 0xFF);

  for (int c = 0; c < 4; c++)
    size += iHDREncodeComponent(rgbe, width, c, scratch + size);

  imBinFileWrite(this->handle, scratch, (unsigned long)size, 1);
  return imBinFileError(this->handle)? IM_ERR_ACCESS: IM_ERR_NONE;
}

int imFileFormatHDR::WriteImageData(void* data)
{
  int is_gray = (imColorModeSpace(this->file_color_mode) == IM_GRAY);
  int width = this->width;

  /* Worst case for the RLE is one count byte per 128 literals, per
     component, plus the 4 byte marker. */
  unsigned char* rgbe = (unsigned char*)malloc((size_t)width * 4);
  unsigned char* scratch = (unsigned char*)malloc((size_t)width * 4 + (size_t)(width / 128 + 2) * 4 + 4);
  if (!rgbe || !scratch)
  {
    free(rgbe);
    free(scratch);
    return IM_ERR_MEM;
  }

  imCounterTotal(this->counter, this->height, "Writing HDR...");

  for (int lin = 0; lin < this->height; lin++)
  {
    imFileLineBufferWrite(this, data, lin, 0);

    const float* line = (const float*)this->line_buffer;
    for (int x = 0; x < width; x++)
    {
      if (is_gray)
      {
        float gray[3] = { line[x], line[x], line[x] };
        iHDREncode(gray, rgbe + 4 * x);
      }
      else
        iHDREncode(line + 3 * x, rgbe + 4 * x);
    }

    int error = WriteScanline(rgbe, scratch);
    if (error != IM_ERR_NONE)
    {
      free(rgbe);
      free(scratch);
      return error;
    }

    if (!imCounterInc(this->counter))
    {
      free(rgbe);
      free(scratch);
      return IM_ERR_COUNTER;
    }
  }

  free(rgbe);
  free(scratch);
  return IM_ERR_NONE;
}

int imFormatHDR::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_XYZ && color_space != IM_GRAY)
    return IM_ERR_DATA;

  if (imColorModeHasAlpha(color_mode))
    return IM_ERR_DATA;

  if (data_type != IM_FLOAT)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (!imStrEqual(compression, "RLE") && !imStrEqual(compression, "NONE"))
    return IM_ERR_COMPRESS;

  return IM_ERR_NONE;
}
