/** \file
 * \brief WebP - Google WebP Image Format
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>
#include <assert.h>

#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/demux.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_webp.h"
#include "im_util.h"
#include "im_counter.h"

/* libwebp works on whole files in memory, never on a stream, so Open reads
   the file in and the write path encodes into memory and writes once.

   Still images go through the simple decode API. Animated ones need
   WebPAnimDecoder from libwebpdemux, which composites each frame onto the
   canvas -- WebPDecode refuses an animated file outright. */

#define IM_WEBP_DEFAULT_QUALITY 75   /* cwebp's own default */

static const char* iWebPCompTable[2] = { "VP8", "VP8L" };


/*******************************************************************/


class imFileFormatWebP: public imFileFormatBase
{
  uint8_t* file_data;          /* the whole file, as libwebp wants it */
  size_t file_size;

  WebPAnimDecoder* anim;       /* NULL for a still image */
  int anim_index;              /* index of the frame last decoded, -1 for none */
  int anim_target;             /* the frame ReadImageInfo selected */
  int canvas_width, canvas_height;

  int has_alpha;
  int is_lossless;             /* write side: VP8L */
  int channel_count;           /* of the line buffer: 1..4 */

  char* write_file_name;

public:
  imFileFormatWebP(const imFormat* _iformat)
    : imFileFormatBase(_iformat), file_data(NULL), file_size(0), anim(NULL),
      anim_index(-1), anim_target(0), canvas_width(0), canvas_height(0), has_alpha(0),
      is_lossless(0), channel_count(3), write_file_name(NULL) {}
  ~imFileFormatWebP() {}

  int Open(const char* file_name);
  int OpenFile(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatWebP: public imFormat
{
public:
  imFormatWebP()
    :imFormat("WEBP",
              "Google WebP Image Format",
              "*.webp;",
              iWebPCompTable,
              2,
              1)
    { extra = "libwebp"; }
  ~imFormatWebP() {}

  imFileFormatBase* Create(void) const { return new imFileFormatWebP(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterWebP(void)
{
  imFormatRegister(new imFormatWebP());
}


/*******************************************************************/


/* Reads the whole file, but only after the 12 byte RIFF header says it is a
   WebP. imFileOpen offers every file to every driver in turn, and slurping a
   large TIFF just to decline it would be a waste. */
static int iWebPReadFile(const char* file_name, uint8_t** data, size_t* size)
{
  unsigned char header[12];
  long length;
  FILE* file = fopen(file_name, "rb");

  if (!file)
    return IM_ERR_OPEN;

  if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WEBP", 4) != 0)
  {
    fclose(file);
    return IM_ERR_FORMAT;
  }

  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET) != 0)
  {
    fclose(file);
    return IM_ERR_ACCESS;
  }

  *data = (uint8_t*)malloc((size_t)length);
  if (!*data)
  {
    fclose(file);
    return IM_ERR_MEM;
  }

  if (fread(*data, 1, (size_t)length, file) != (size_t)length)
  {
    free(*data);
    *data = NULL;
    fclose(file);
    return IM_ERR_ACCESS;
  }

  fclose(file);
  *size = (size_t)length;
  return IM_ERR_NONE;
}

/* imFileOpen deletes a driver whose Open failed without calling Close, so
   every failure has to release what it got this far. */
int imFileFormatWebP::Open(const char* file_name)
{
  int error = OpenFile(file_name);
  if (error != IM_ERR_NONE)
    Close();
  return error;
}

int imFileFormatWebP::OpenFile(const char* file_name)
{
  WebPBitstreamFeatures features;
  int error;

  error = iWebPReadFile(file_name, &this->file_data, &this->file_size);
  if (error != IM_ERR_NONE)
    return error;

  if (WebPGetFeatures(this->file_data, this->file_size, &features) != VP8_STATUS_OK)
  {
    /* A RIFF/WEBP header around a bitstream libwebp cannot parse. Report it
       as not this format, so imFileOpen can still try the others. */
    return IM_ERR_FORMAT;
  }

  this->has_alpha = features.has_alpha;
  this->canvas_width = features.width;
  this->canvas_height = features.height;

  /* features.format is 1 for lossy, 2 for lossless, and 0 for an animation
     mixing the two -- reported as VP8, the one that may lose something. */
  strcpy(this->compression, (features.format == 2)? "VP8L": "VP8");

  if (features.has_animation)
  {
    WebPAnimDecoderOptions options;
    WebPAnimInfo info;
    WebPData webp_data;

    if (!WebPAnimDecoderOptionsInit(&options))
      return IM_ERR_ACCESS;   /* a header/library version mismatch */
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;

    webp_data.bytes = this->file_data;
    webp_data.size = this->file_size;

    this->anim = WebPAnimDecoderNew(&webp_data, &options);
    if (!this->anim || !WebPAnimDecoderGetInfo(this->anim, &info) ||
        info.frame_count < 1)
      return IM_ERR_FORMAT;

    this->canvas_width = (int)info.canvas_width;
    this->canvas_height = (int)info.canvas_height;
    this->image_count = (int)info.frame_count;
  }
  else
    this->image_count = 1;

  return IM_ERR_NONE;
}

int imFileFormatWebP::New(const char* file_name)
{
  /* libwebp encodes the complete image in one call, so there is nothing to
     open yet -- keep the name for WriteImageData. */
  this->write_file_name = (char*)malloc(strlen(file_name) + 1);
  if (!this->write_file_name)
    return IM_ERR_MEM;
  strcpy(this->write_file_name, file_name);

  /* Fail now rather than after the whole encode, the way a stream-based
     driver would have found out. */
  FILE* file = fopen(file_name, "wb");
  if (!file)
  {
    free(this->write_file_name);
    this->write_file_name = NULL;
    return IM_ERR_OPEN;
  }
  fclose(file);

  this->image_count = 1;
  return IM_ERR_NONE;
}

void imFileFormatWebP::Close()
{
  if (this->anim)
  {
    WebPAnimDecoderDelete(this->anim);
    this->anim = NULL;
  }

  if (this->file_data)
  {
    free(this->file_data);
    this->file_data = NULL;
  }

  if (this->write_file_name)
  {
    free(this->write_file_name);
    this->write_file_name = NULL;
  }
}

void* imFileFormatWebP::Handle(int index)
{
  (void)index;
  return NULL;
}

int imFileFormatWebP::ReadImageInfo(int index)
{
  if (index < 0 || index >= this->image_count)
    return IM_ERR_DATA;

  this->anim_target = index;

  this->width = this->canvas_width;
  this->height = this->canvas_height;
  if (this->width <= 0 || this->height <= 0)
    return IM_ERR_DATA;

  this->file_data_type = IM_BYTE;
  this->file_color_mode = IM_RGB;
  this->channel_count = 3;

  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count = 4;
  }

  this->file_color_mode |= IM_TOPDOWN | IM_PACKED;

  return IM_ERR_NONE;
}

int imFileFormatWebP::ReadImageData(void* data)
{
  uint8_t* pixels = NULL;   /* still image: owned here */
  const uint8_t* rows;      /* what the lines are copied from */
  int stride, count, lin = 0, plane = 0;
  int line_bytes = this->width * this->channel_count;

  if (!this->file_data)
    return IM_ERR_ACCESS;

  if (this->anim)
  {
    /* Frames depend on the ones before them, so reaching frame n means
       decoding 0..n. Going backwards starts over. */
    uint8_t* canvas = NULL;
    int timestamp;
    int target = this->anim_target;

    if (target <= this->anim_index)
    {
      WebPAnimDecoderReset(this->anim);
      this->anim_index = -1;
    }

    while (this->anim_index < target)
    {
      if (!WebPAnimDecoderGetNext(this->anim, &canvas, &timestamp))
        return IM_ERR_ACCESS;
      this->anim_index++;
    }

    /* Always RGBA, whatever the file holds. */
    rows = canvas;
    stride = this->width * 4;
  }
  else
  {
    size_t size;
    stride = line_bytes;
    size = (size_t)stride * (size_t)this->height;

    pixels = (uint8_t*)malloc(size);
    if (!pixels)
      return IM_ERR_MEM;

    uint8_t* result = this->has_alpha?
      WebPDecodeRGBAInto(this->file_data, this->file_size, pixels, size, stride):
      WebPDecodeRGBInto(this->file_data, this->file_size, pixels, size, stride);

    if (!result)
    {
      free(pixels);
      return IM_ERR_ACCESS;
    }

    rows = pixels;
  }

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Reading WebP...");

  for (int i = 0; i < count; i++)
  {
    const uint8_t* row = rows + (size_t)lin * (size_t)stride;

    if (this->anim && !this->has_alpha)
    {
      /* The animation canvas is RGBA even when no frame has alpha. */
      uint8_t* target = (uint8_t*)this->line_buffer;
      for (int x = 0; x < this->width; x++)
      {
        target[3*x + 0] = row[4*x + 0];
        target[3*x + 1] = row[4*x + 1];
        target[3*x + 2] = row[4*x + 2];
      }
    }
    else
      memcpy(this->line_buffer, row, (size_t)line_bytes);

    imFileLineBufferRead(this, data, lin, plane);

    if (!imCounterInc(this->counter))
    {
      free(pixels);
      return IM_ERR_COUNTER;
    }

    imFileLineBufferInc(this, &lin, &plane);
  }

  free(pixels);
  return IM_ERR_NONE;
}

int imFileFormatWebP::WriteImageInfo()
{
  imAttribTable* attrib_table = AttribTable();
  int color_space = imColorModeSpace(this->user_color_mode);
  int* lossless;

  /* No colour conversion happens on the write path (imFileCheckConversion
     refuses one), so gray and binary arrive as one channel and are expanded
     to RGB in WriteImageData. */
  this->file_data_type = IM_BYTE;
  this->file_color_mode = (color_space == IM_GRAY || color_space == IM_BINARY)?
                          color_space: IM_RGB;
  this->channel_count = (this->file_color_mode == IM_RGB)? 3: 1;

  this->has_alpha = imColorModeHasAlpha(this->user_color_mode);
  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count++;
  }

  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  if (this->width > WEBP_MAX_DIMENSION || this->height > WEBP_MAX_DIMENSION)
    return IM_ERR_DATA;

  if (this->compression[0] == 0 || imStrEqual(this->compression, "VP8"))
    this->is_lossless = 0;
  else if (imStrEqual(this->compression, "VP8L"))
    this->is_lossless = 1;
  else
    return IM_ERR_COMPRESS;

  lossless = (int*)attrib_table->Get("Lossless");
  if (lossless && *lossless)
    this->is_lossless = 1;

  strcpy(this->compression, this->is_lossless? "VP8L": "VP8");

  return IM_ERR_NONE;
}

int imFileFormatWebP::WriteImageData(void* data)
{
  imAttribTable* attrib_table = AttribTable();
  int out_channels = this->has_alpha? 4: 3;
  int in_channels = this->channel_count;
  int is_binary = (imColorModeSpace(this->file_color_mode) == IM_BINARY);
  int stride = this->width * out_channels;
  int count, lin = 0, plane = 0, ok, error = IM_ERR_NONE;
  uint8_t* pixels;
  int* quality;
  WebPConfig config;
  WebPPicture picture;
  WebPMemoryWriter writer;

  if (!this->write_file_name)
    return IM_ERR_ACCESS;

  pixels = (uint8_t*)malloc((size_t)stride * (size_t)this->height);
  if (!pixels)
    return IM_ERR_MEM;

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Writing WebP...");

  for (int i = 0; i < count; i++)
  {
    const imbyte* source = (const imbyte*)this->line_buffer;
    uint8_t* target = pixels + (size_t)lin * (size_t)stride;

    imFileLineBufferWrite(this, data, lin, plane);

    if (in_channels == out_channels)
      memcpy(target, source, (size_t)stride);
    else
    {
      /* One gray sample (plus alpha) becomes three equal ones. Binary data
         is 0 and 1, which would store as two near-black values. */
      for (int x = 0; x < this->width; x++)
      {
        imbyte value = source[x * in_channels];
        if (is_binary)
          value = value? 255: 0;

        target[x * out_channels + 0] = value;
        target[x * out_channels + 1] = value;
        target[x * out_channels + 2] = value;
        if (this->has_alpha)
          target[x * out_channels + 3] = source[x * in_channels + 1];
      }
    }

    if (!imCounterInc(this->counter))
    {
      free(pixels);
      return IM_ERR_COUNTER;
    }

    imFileLineBufferInc(this, &lin, &plane);
  }

  if (!WebPConfigInit(&config) || !WebPPictureInit(&picture))
  {
    free(pixels);
    return IM_ERR_ACCESS;   /* a header/library version mismatch */
  }

  quality = (int*)attrib_table->Get("Quality");
  {
    int value = quality? *quality: IM_WEBP_DEFAULT_QUALITY;
    if (value < 0) value = 0;
    if (value > 100) value = 100;

    /* For lossless, quality is effort rather than fidelity: how hard the
       encoder works on the size. Passing it through is what cwebp does. */
    config.quality = (float)value;
  }

  if (this->is_lossless)
  {
    config.lossless = 1;

    /* Without this libwebp rewrites the colour of fully transparent pixels to
       whatever compresses best, so "lossless" RGBA did not round-trip. */
    config.exact = 1;
  }

  if (!WebPValidateConfig(&config))
  {
    free(pixels);
    return IM_ERR_COMPRESS;
  }

  picture.width = this->width;
  picture.height = this->height;
  picture.use_argb = this->is_lossless;   /* lossless works on ARGB, lossy on YUV */

  ok = this->has_alpha? WebPPictureImportRGBA(&picture, pixels, stride):
                        WebPPictureImportRGB(&picture, pixels, stride);
  free(pixels);
  if (!ok)
  {
    WebPPictureFree(&picture);
    return IM_ERR_MEM;
  }

  WebPMemoryWriterInit(&writer);
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;

  if (!WebPEncode(&config, &picture))
  {
    error = (picture.error_code == VP8_ENC_ERROR_OUT_OF_MEMORY ||
             picture.error_code == VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY)?
            IM_ERR_MEM: IM_ERR_COMPRESS;
  }
  WebPPictureFree(&picture);

  if (error == IM_ERR_NONE)
  {
    FILE* file = fopen(this->write_file_name, "wb");
    if (!file)
      error = IM_ERR_OPEN;
    else
    {
      if (fwrite(writer.mem, 1, writer.size, file) != writer.size)
        error = IM_ERR_ACCESS;
      if (fclose(file) != 0)
        error = IM_ERR_ACCESS;
    }
  }

  WebPMemoryWriterClear(&writer);
  return error;
}

int imFormatWebP::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_GRAY &&
      color_space != IM_BINARY)
    return IM_ERR_DATA;

  if (data_type != IM_BYTE)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (!imStrEqual(compression, "VP8") && !imStrEqual(compression, "VP8L"))
    return IM_ERR_COMPRESS;

  return IM_ERR_NONE;
}
