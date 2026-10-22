/** \file
 * \brief QOI - Quite OK Image Format
 *
 * See Copyright Notice in im_lib.h
 */

#include "im_format.h"
#include "im_format_all.h"
#include "im_util.h"
#include "im_counter.h"

#include "im_binfile.h"

#include <stdlib.h>
#include <string.h>
#include <memory.h>

/* Dominic Szablewski's QOI, implemented here from the one-page specification
   (https://qoiformat.org/qoi-specification.pdf) rather than vendored: the
   whole codec is six chunk types and a 64 entry colour cache, smaller than the
   glue a library would need.

   The pixel stream does not restart per scanline -- runs, the cache and the
   previous pixel all carry across row boundaries -- so the decoder and encoder
   keep their state in the driver between lines. Reading loads the file into
   memory, which keeps the decoder a bounds-checked walk over a buffer. */

static const char* iQOICompTable[1] =
{
  "QOI"
};

#define QOI_OP_INDEX 0x00   /* 00xxxxxx */
#define QOI_OP_DIFF  0x40   /* 01xxxxxx */
#define QOI_OP_LUMA  0x80   /* 10xxxxxx */
#define QOI_OP_RUN   0xC0   /* 11xxxxxx */
#define QOI_OP_RGB   0xFE
#define QOI_OP_RGBA  0xFF
#define QOI_MASK_2   0xC0

#define QOI_HEADER_SIZE 14
#define QOI_PADDING_SIZE 8

/* The spec's own limit, chosen so that width * height * 4 fits in 32 bits
   with room to spare. */
#define QOI_PIXELS_MAX 400000000UL

struct iQOIPixel
{
  unsigned char r, g, b, a;
};

static int iQOIEqual(const iQOIPixel& p, const iQOIPixel& q)
{
  return p.r == q.r && p.g == q.g && p.b == q.b && p.a == q.a;
}

static int iQOIHash(const iQOIPixel& p)
{
  return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) % 64;
}

class imFileFormatQOI: public imFileFormatBase
{
  imBinFile* handle;

  unsigned char* stream;       /* reading: the chunk data, header stripped */
  size_t stream_size;
  size_t cursor;

  int file_width, file_height; /* imFileOpen clears width/height after Open */
  int channels;                /* 3 or 4, from the header or the image */
  unsigned char colorspace;

  /* Codec state, carried across scanlines. */
  iQOIPixel index[64];
  iQOIPixel previous;
  int run;

  void ResetState();
  int DecodeLine(unsigned char* out);
  int EncodeLine(const unsigned char* in, int is_last_line, unsigned char* out, size_t* size);

public:
  imFileFormatQOI(const imFormat* _iformat)
    : imFileFormatBase(_iformat), handle(NULL), stream(NULL), stream_size(0),
      cursor(0), file_width(0), file_height(0), channels(4), colorspace(0),
      run(0) { ResetState(); }
  ~imFileFormatQOI() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatQOI: public imFormat
{
public:
  imFormatQOI()
    :imFormat("QOI",
              "Quite OK Image Format",
              "*.qoi;",
              iQOICompTable,
              1,
              0)
    {}
  ~imFormatQOI() {}

  imFileFormatBase* Create(void) const { return new imFileFormatQOI(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterQOI(void)
{
  imFormatRegister(new imFormatQOI());
}


/*******************************************************************/


static unsigned long iQOIReadBE32(const unsigned char* p)
{
  return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
         ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void iQOIWriteBE32(unsigned char* p, unsigned long v)
{
  p[0] = (unsigned char)(v >> 24);
  p[1] = (unsigned char)(v >> 16);
  p[2] = (unsigned char)(v >> 8);
  p[3] = (unsigned char)v;
}

void imFileFormatQOI::ResetState()
{
  memset(this->index, 0, sizeof(this->index));
  this->previous.r = this->previous.g = this->previous.b = 0;
  this->previous.a = 255;
  this->run = 0;
}

int imFileFormatQOI::Open(const char* file_name)
{
  unsigned char header[QOI_HEADER_SIZE];

  this->handle = imBinFileOpen(file_name);
  if (!this->handle)
    return IM_ERR_OPEN;

  imBinFileRead(this->handle, header, QOI_HEADER_SIZE, 1);
  if (imBinFileError(this->handle) || memcmp(header, "qoif", 4) != 0)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
    return IM_ERR_FORMAT;
  }

  unsigned long width = iQOIReadBE32(header + 4);
  unsigned long height = iQOIReadBE32(header + 8);
  this->channels = header[12];
  this->colorspace = header[13];

  /* The header is all there is to go on: a zero, a dimension past the spec's
     limit, or a channel count other than 3 or 4 is not a QOI file. */
  if (width == 0 || height == 0 || height >= QOI_PIXELS_MAX / width ||
      (this->channels != 3 && this->channels != 4) || this->colorspace > 1)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
    return IM_ERR_DATA;
  }

  this->file_width = (int)width;
  this->file_height = (int)height;

  unsigned long size = imBinFileSize(this->handle);
  if (size <= QOI_HEADER_SIZE)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
    return IM_ERR_ACCESS;
  }

  this->stream_size = (size_t)(size - QOI_HEADER_SIZE);
  this->stream = (unsigned char*)malloc(this->stream_size);
  if (!this->stream)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
    return IM_ERR_MEM;
  }

  imBinFileRead(this->handle, this->stream, (unsigned long)this->stream_size, 1);
  int failed = imBinFileError(this->handle);
  imBinFileClose(this->handle);
  this->handle = NULL;
  if (failed)
  {
    free(this->stream);
    this->stream = NULL;
    return IM_ERR_ACCESS;
  }

  strcpy(this->compression, "QOI");
  this->image_count = 1;
  return IM_ERR_NONE;
}

int imFileFormatQOI::New(const char* file_name)
{
  this->handle = imBinFileNew(file_name);
  if (!this->handle)
    return IM_ERR_OPEN;

  strcpy(this->compression, "QOI");
  this->image_count = 1;
  return IM_ERR_NONE;
}

void imFileFormatQOI::Close()
{
  if (this->handle)
  {
    imBinFileClose(this->handle);
    this->handle = NULL;
  }

  if (this->stream)
  {
    free(this->stream);
    this->stream = NULL;
  }
}

void* imFileFormatQOI::Handle(int index)
{
  if (index == 0)
    return (void*)this->handle;   /* NULL once reading has loaded the data */
  return NULL;
}

int imFileFormatQOI::ReadImageInfo(int index)
{
  if (index != 0)
    return IM_ERR_DATA;

  this->width = this->file_width;
  this->height = this->file_height;
  this->file_data_type = IM_BYTE;
  this->file_color_mode = IM_RGB | IM_PACKED | IM_TOPDOWN;
  if (this->channels == 4)
    this->file_color_mode |= IM_ALPHA;

  /* 0 is sRGB with linear alpha, 1 is all channels linear. Informational
     only: the spec says it does not change how the pixels decode. */
  imAttribTable* attrib_table = AttribTable();
  attrib_table->Set("QOIColorSpace", IM_BYTE, 1, &this->colorspace);

  return IM_ERR_NONE;
}

/* One scanline of pixels, written as channels bytes each. Every read is
   checked against the buffer, so a truncated or hostile file ends in
   IM_ERR_ACCESS instead of a read past the end. */
int imFileFormatQOI::DecodeLine(unsigned char* out)
{
  const unsigned char* data = this->stream;
  size_t end = this->stream_size;
  iQOIPixel px = this->previous;

  for (int x = 0; x < this->width; x++)
  {
    if (this->run > 0)
      this->run--;
    else
    {
      if (this->cursor >= end)
        return IM_ERR_ACCESS;

      int b1 = data[this->cursor++];

      if (b1 == QOI_OP_RGB)
      {
        if (this->cursor + 3 > end)
          return IM_ERR_ACCESS;
        px.r = data[this->cursor++];
        px.g = data[this->cursor++];
        px.b = data[this->cursor++];
      }
      else if (b1 == QOI_OP_RGBA)
      {
        if (this->cursor + 4 > end)
          return IM_ERR_ACCESS;
        px.r = data[this->cursor++];
        px.g = data[this->cursor++];
        px.b = data[this->cursor++];
        px.a = data[this->cursor++];
      }
      else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX)
        px = this->index[b1];
      else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF)
      {
        /* Unsigned char arithmetic wraps, as the spec requires. */
        px.r = (unsigned char)(px.r + ((b1 >> 4) & 0x03) - 2);
        px.g = (unsigned char)(px.g + ((b1 >> 2) & 0x03) - 2);
        px.b = (unsigned char)(px.b + ( b1       & 0x03) - 2);
      }
      else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA)
      {
        if (this->cursor >= end)
          return IM_ERR_ACCESS;
        int b2 = data[this->cursor++];
        int vg = (b1 & 0x3F) - 32;
        px.r = (unsigned char)(px.r + vg - 8 + ((b2 >> 4) & 0x0F));
        px.g = (unsigned char)(px.g + vg);
        px.b = (unsigned char)(px.b + vg - 8 + (b2 & 0x0F));
      }
      else
      {
        /* QOI_OP_RUN. The run includes this pixel, so the counter keeps the
           remainder. */
        this->run = (b1 & 0x3F);
      }

      this->index[iQOIHash(px)] = px;
    }

    out[0] = px.r;
    out[1] = px.g;
    out[2] = px.b;
    if (this->channels == 4)
      out[3] = px.a;
    out += this->channels;
  }

  this->previous = px;
  return IM_ERR_NONE;
}

int imFileFormatQOI::ReadImageData(void* data)
{
  if (!this->stream)
    return IM_ERR_ACCESS;

  ResetState();
  this->cursor = 0;

  imCounterTotal(this->counter, this->height, "Reading QOI...");

  for (int lin = 0; lin < this->height; lin++)
  {
    int error = DecodeLine((unsigned char*)this->line_buffer);
    if (error != IM_ERR_NONE)
      return error;

    imFileLineBufferRead(this, data, lin, 0);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;
  }

  return IM_ERR_NONE;
}

int imFileFormatQOI::WriteImageInfo()
{
  int color_space = imColorModeSpace(this->user_color_mode);

  if (this->user_data_type != IM_BYTE)
    return IM_ERR_DATA;
  this->file_data_type = IM_BYTE;

  if (this->compression[0] != 0 && !imStrEqual(this->compression, "QOI"))
    return IM_ERR_COMPRESS;
  strcpy(this->compression, "QOI");

  if ((unsigned long)this->height >= QOI_PIXELS_MAX / (unsigned long)this->width)
    return IM_ERR_DATA;

  /* Gray and binary arrive as one channel, since the write path does no
     colour conversion, and are widened to RGB in EncodeLine's caller. */
  if (color_space == IM_GRAY || color_space == IM_BINARY)
    this->file_color_mode = color_space;
  else if (color_space == IM_RGB)
    this->file_color_mode = IM_RGB;
  else
    return IM_ERR_DATA;

  this->channels = 3;
  if (imColorModeHasAlpha(this->user_color_mode))
  {
    this->file_color_mode |= IM_ALPHA;
    this->channels = 4;
  }

  this->file_color_mode |= IM_TOPDOWN;
  if (imColorModeSpace(this->file_color_mode) == IM_RGB || this->channels == 4)
    this->file_color_mode |= IM_PACKED;

  unsigned char header[QOI_HEADER_SIZE];
  memcpy(header, "qoif", 4);
  iQOIWriteBE32(header + 4, (unsigned long)this->width);
  iQOIWriteBE32(header + 8, (unsigned long)this->height);
  header[12] = (unsigned char)this->channels;
  header[13] = 0;   /* sRGB with linear alpha: what an 8 bit image usually is */

  imBinFileWrite(this->handle, header, QOI_HEADER_SIZE, 1);
  if (imBinFileError(this->handle))
    return IM_ERR_ACCESS;

  return IM_ERR_NONE;
}

/* Encodes one scanline of RGBA pixels, continuing the stream from the line
   before. A run open at the end of a line stays open into the next one, and
   is flushed only by a different pixel, the 62 pixel limit or the last pixel
   of the image -- the same chunks the reference encoder emits. */
int imFileFormatQOI::EncodeLine(const unsigned char* in, int is_last_line,
                                unsigned char* out, size_t* size)
{
  size_t n = 0;

  for (int x = 0; x < this->width; x++)
  {
    iQOIPixel px;
    px.r = in[4 * x + 0];
    px.g = in[4 * x + 1];
    px.b = in[4 * x + 2];
    px.a = in[4 * x + 3];

    int is_last = is_last_line && (x == this->width - 1);

    if (iQOIEqual(px, this->previous))
    {
      this->run++;
      if (this->run == 62 || is_last)
      {
        out[n++] = (unsigned char)(QOI_OP_RUN | (this->run - 1));
        this->run = 0;
      }
      continue;
    }

    if (this->run > 0)
    {
      out[n++] = (unsigned char)(QOI_OP_RUN | (this->run - 1));
      this->run = 0;
    }

    int hash = iQOIHash(px);
    if (iQOIEqual(this->index[hash], px))
      out[n++] = (unsigned char)(QOI_OP_INDEX | hash);
    else
    {
      this->index[hash] = px;

      if (px.a == this->previous.a)
      {
        /* Differences as signed chars, wrapping, as the spec defines them. */
        signed char vr = (signed char)(px.r - this->previous.r);
        signed char vg = (signed char)(px.g - this->previous.g);
        signed char vb = (signed char)(px.b - this->previous.b);
        signed char vg_r = (signed char)(vr - vg);
        signed char vg_b = (signed char)(vb - vg);

        if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2)
          out[n++] = (unsigned char)(QOI_OP_DIFF | ((vr + 2) << 4) | ((vg + 2) << 2) | (vb + 2));
        else if (vg_r > -9 && vg_r < 8 && vg > -33 && vg < 32 && vg_b > -9 && vg_b < 8)
        {
          out[n++] = (unsigned char)(QOI_OP_LUMA | (vg + 32));
          out[n++] = (unsigned char)(((vg_r + 8) << 4) | (vg_b + 8));
        }
        else
        {
          out[n++] = QOI_OP_RGB;
          out[n++] = px.r;
          out[n++] = px.g;
          out[n++] = px.b;
        }
      }
      else
      {
        out[n++] = QOI_OP_RGBA;
        out[n++] = px.r;
        out[n++] = px.g;
        out[n++] = px.b;
        out[n++] = px.a;
      }
    }

    this->previous = px;
  }

  *size = n;
  return IM_ERR_NONE;
}

int imFileFormatQOI::WriteImageData(void* data)
{
  int width = this->width;
  int space = imColorModeSpace(this->file_color_mode);
  int has_alpha = imColorModeHasAlpha(this->file_color_mode);
  int in_channels = ((space == IM_RGB)? 3: 1) + (has_alpha? 1: 0);

  /* RGBA is the worst case: 5 bytes a pixel. */
  unsigned char* rgba = (unsigned char*)malloc((size_t)width * 4);
  unsigned char* chunks = (unsigned char*)malloc((size_t)width * 5);
  if (!rgba || !chunks)
  {
    free(rgba);
    free(chunks);
    return IM_ERR_MEM;
  }

  ResetState();
  imCounterTotal(this->counter, this->height, "Writing QOI...");

  for (int lin = 0; lin < this->height; lin++)
  {
    imFileLineBufferWrite(this, data, lin, 0);

    const unsigned char* line = (const unsigned char*)this->line_buffer;
    for (int x = 0; x < width; x++)
    {
      const unsigned char* p = line + x * in_channels;
      if (space == IM_RGB)
      {
        rgba[4 * x + 0] = p[0];
        rgba[4 * x + 1] = p[1];
        rgba[4 * x + 2] = p[2];
      }
      else
      {
        /* Binary is 0 and 1; stored as is it would be two near-blacks. */
        unsigned char v = (space == IM_BINARY)? (p[0]? 255: 0): p[0];
        rgba[4 * x + 0] = rgba[4 * x + 1] = rgba[4 * x + 2] = v;
      }

      /* Without alpha every pixel is opaque, matching the decoder's initial
         previous pixel, so a 3 channel file never needs QOI_OP_RGBA. */
      rgba[4 * x + 3] = has_alpha? p[in_channels - 1]: 255;
    }

    size_t size = 0;
    EncodeLine(rgba, lin == this->height - 1, chunks, &size);

    if (size)
      imBinFileWrite(this->handle, chunks, (unsigned long)size, 1);
    if (imBinFileError(this->handle))
    {
      free(rgba);
      free(chunks);
      return IM_ERR_ACCESS;
    }

    if (!imCounterInc(this->counter))
    {
      free(rgba);
      free(chunks);
      return IM_ERR_COUNTER;
    }
  }

  free(rgba);
  free(chunks);

  static const unsigned char padding[QOI_PADDING_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1 };
  imBinFileWrite(this->handle, (void*)padding, QOI_PADDING_SIZE, 1);
  if (imBinFileError(this->handle))
    return IM_ERR_ACCESS;

  return IM_ERR_NONE;
}

int imFormatQOI::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_GRAY && color_space != IM_BINARY)
    return IM_ERR_DATA;

  if (data_type != IM_BYTE)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (!imStrEqual(compression, "QOI"))
    return IM_ERR_COMPRESS;

  return IM_ERR_NONE;
}
