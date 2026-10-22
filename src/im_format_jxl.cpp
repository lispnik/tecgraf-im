/** \file
 * \brief JXL - JPEG XL
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>
#include <assert.h>

#include <jxl/decode.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_jxl.h"
#include "im_util.h"
#include "im_counter.h"

/* 0.7 is the floor because it is what Ubuntu 24.04 ships, the oldest libjxl
   CI builds against. That rules out three conveniences added later, each
   avoided here on purpose:

     JxlEncoderDistanceFromQuality   0.9  -- iJXLDistance below copies it
     JxlDecoderGetICCProfileSize     changed signature in 0.9
     JxlDecoderCloseInput            not needed: the whole file is the input

   Like libwebp, libjxl works on memory rather than a stream, so the file is
   read whole and the encoded result written whole. */

#define IM_JXL_DEFAULT_QUALITY 90   /* distance 1.0, cjxl's default */

static const char* iJXLCompTable[1] = { "JXL" };


/*******************************************************************/


class imFileFormatJXL: public imFileFormatBase
{
  uint8_t* file_data;
  size_t file_size;

  JxlDecoder* decoder;
  void* runner;                /* JxlThreadParallelRunner, shared by both paths */
  JxlBasicInfo info;

  int has_alpha;
  int channel_count;           /* interleaved samples per pixel, 1..4 */

  char* write_file_name;

  int OpenFile(const char* file_name);
  JxlDataType iPixelType() const;
  int iSampleSize() const;

public:
  imFileFormatJXL(const imFormat* _iformat)
    : imFileFormatBase(_iformat), file_data(NULL), file_size(0), decoder(NULL),
      runner(NULL), has_alpha(0), channel_count(3), write_file_name(NULL)
    { memset(&this->info, 0, sizeof(this->info)); }
  ~imFileFormatJXL() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatJXL: public imFormat
{
public:
  imFormatJXL()
    :imFormat("JXL",
              "JPEG XL",
              "*.jxl;",
              iJXLCompTable,
              1,
              0)
    { extra = "libjxl"; }
  ~imFormatJXL() {}

  imFileFormatBase* Create(void) const { return new imFileFormatJXL(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterJXL(void)
{
  imFormatRegister(new imFormatJXL());
}


/*******************************************************************/


/* Quality 0-100 to Butteraugli distance, the same curve as libjxl's own
   JxlEncoderDistanceFromQuality (0.9+) and cjxl's -q: 90 is 1.0, 100 would
   be 0. Distance 0 is not a lossy setting libjxl accepts -- lossless is its
   own switch -- so the top of the range stops just short of it. */
static float iJXLDistance(int quality)
{
  float q = (float)quality;
  if (q >= 100.0f)
    return 0.1f;
  if (q >= 30.0f)
    return 0.1f + (100.0f - q) * 0.09f;
  return 53.0f / 3000.0f * q * q - 23.0f / 20.0f * q + 25.0f;
}

JxlDataType imFileFormatJXL::iPixelType() const
{
  switch (this->file_data_type)
  {
  case IM_USHORT: return JXL_TYPE_UINT16;
  case IM_FLOAT:  return JXL_TYPE_FLOAT;
  default:        return JXL_TYPE_UINT8;
  }
}

int imFileFormatJXL::iSampleSize() const
{
  return imDataTypeSize(this->file_data_type);
}

int imFileFormatJXL::Open(const char* file_name)
{
  /* imFileOpen deletes a driver whose Open failed without calling Close, so
     every failure has to release what it got this far. */
  int error = OpenFile(file_name);
  if (error != IM_ERR_NONE)
    Close();
  return error;
}

int imFileFormatJXL::OpenFile(const char* file_name)
{
  unsigned char header[12];
  size_t header_size;
  long length;
  JxlDecoderStatus status;
  FILE* file = fopen(file_name, "rb");

  if (!file)
    return IM_ERR_OPEN;

  /* Check the signature before reading the rest: imFileOpen offers every
     file to every driver, and slurping a large TIFF just to decline it would
     be a waste. 12 bytes covers both the bare codestream (FF 0A) and the
     container's signature box. */
  header_size = fread(header, 1, sizeof(header), file);
  {
    JxlSignature signature = JxlSignatureCheck(header, header_size);
    if (signature != JXL_SIG_CODESTREAM && signature != JXL_SIG_CONTAINER)
    {
      fclose(file);
      return IM_ERR_FORMAT;
    }
  }

  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET) != 0)
  {
    fclose(file);
    return IM_ERR_ACCESS;
  }

  this->file_data = (uint8_t*)malloc((size_t)length);
  if (!this->file_data)
  {
    fclose(file);
    return IM_ERR_MEM;
  }

  if (fread(this->file_data, 1, (size_t)length, file) != (size_t)length)
  {
    fclose(file);
    return IM_ERR_ACCESS;
  }
  fclose(file);
  this->file_size = (size_t)length;

  this->decoder = JxlDecoderCreate(NULL);
  if (!this->decoder)
    return IM_ERR_MEM;

  this->runner = JxlThreadParallelRunnerCreate(NULL,
                   JxlThreadParallelRunnerDefaultNumWorkerThreads());
  if (this->runner &&
      JxlDecoderSetParallelRunner(this->decoder, JxlThreadParallelRunner,
                                  this->runner) != JXL_DEC_SUCCESS)
    return IM_ERR_ACCESS;

  if (JxlDecoderSubscribeEvents(this->decoder,
                                JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE)
      != JXL_DEC_SUCCESS)
    return IM_ERR_ACCESS;

  if (JxlDecoderSetInput(this->decoder, this->file_data, this->file_size)
      != JXL_DEC_SUCCESS)
    return IM_ERR_ACCESS;

  /* Only as far as the header here; pixels are decoded in ReadImageData. */
  status = JxlDecoderProcessInput(this->decoder);
  if (status != JXL_DEC_BASIC_INFO)
    return IM_ERR_FORMAT;   /* the signature matched but the header did not */

  if (JxlDecoderGetBasicInfo(this->decoder, &this->info) != JXL_DEC_SUCCESS)
    return IM_ERR_FORMAT;

  strcpy(this->compression, "JXL");
  this->image_count = 1;

  return IM_ERR_NONE;
}

int imFileFormatJXL::New(const char* file_name)
{
  /* libjxl encodes the complete image in one call, so there is nothing to
     write yet -- keep the name for WriteImageData. */
  this->write_file_name = (char*)malloc(strlen(file_name) + 1);
  if (!this->write_file_name)
    return IM_ERR_MEM;
  strcpy(this->write_file_name, file_name);

  /* Fail now rather than after the whole encode. */
  FILE* file = fopen(file_name, "wb");
  if (!file)
  {
    free(this->write_file_name);
    this->write_file_name = NULL;
    return IM_ERR_OPEN;
  }
  fclose(file);

  strcpy(this->compression, "JXL");
  this->image_count = 1;
  return IM_ERR_NONE;
}

void imFileFormatJXL::Close()
{
  if (this->decoder)
  {
    JxlDecoderDestroy(this->decoder);
    this->decoder = NULL;
  }

  if (this->runner)
  {
    JxlThreadParallelRunnerDestroy(this->runner);
    this->runner = NULL;
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

void* imFileFormatJXL::Handle(int index)
{
  (void)index;
  return NULL;
}

int imFileFormatJXL::ReadImageInfo(int index)
{
  if (index != 0)
    return IM_ERR_DATA;

  /* The decoder applies the orientation, so for the four transposing ones
     (5..8) the pixels come out with width and height exchanged. */
  if (this->info.orientation >= JXL_ORIENT_TRANSPOSE)
  {
    this->width = (int)this->info.ysize;
    this->height = (int)this->info.xsize;
  }
  else
  {
    this->width = (int)this->info.xsize;
    this->height = (int)this->info.ysize;
  }
  if (this->width <= 0 || this->height <= 0)
    return IM_ERR_DATA;

  if (this->info.exponent_bits_per_sample > 0)
    this->file_data_type = IM_FLOAT;
  else if (this->info.bits_per_sample > 8)
    this->file_data_type = IM_USHORT;
  else
    this->file_data_type = IM_BYTE;

  if (this->info.num_color_channels == 1)
  {
    this->file_color_mode = IM_GRAY;
    this->channel_count = 1;
  }
  else
  {
    this->file_color_mode = IM_RGB;
    this->channel_count = 3;
  }

  this->has_alpha = (this->info.alpha_bits > 0);
  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count++;
  }

  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  return IM_ERR_NONE;
}

int imFileFormatJXL::ReadImageData(void* data)
{
  JxlPixelFormat format;
  JxlDecoderStatus status;
  uint8_t* pixels = NULL;
  size_t buffer_size = 0, line_bytes;
  int count, lin = 0, plane = 0, done = 0;

  if (!this->decoder)
    return IM_ERR_ACCESS;

  /* UINT16 output is scaled to the full 16 bit range whatever the file's own
     depth, so a 10 or 12 bit file needs no shift here, unlike HEIF. */
  format.num_channels = (uint32_t)this->channel_count;
  format.data_type = iPixelType();
  format.endianness = JXL_NATIVE_ENDIAN;
  format.align = 0;

  line_bytes = (size_t)this->width * (size_t)this->channel_count *
               (size_t)iSampleSize();

  while (!done)
  {
    status = JxlDecoderProcessInput(this->decoder);

    switch (status)
    {
    case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
      if (JxlDecoderImageOutBufferSize(this->decoder, &format, &buffer_size)
          != JXL_DEC_SUCCESS)
      {
        free(pixels);
        return IM_ERR_ACCESS;
      }

      /* With align 0 the rows are tight. Anything else would mean libjxl and
         this driver disagree about the geometry, and copying line_bytes per
         row would run off the end. */
      if (buffer_size != line_bytes * (size_t)this->height)
      {
        free(pixels);
        return IM_ERR_DATA;
      }

      free(pixels);
      pixels = (uint8_t*)malloc(buffer_size);
      if (!pixels)
        return IM_ERR_MEM;

      if (JxlDecoderSetImageOutBuffer(this->decoder, &format, pixels,
                                      buffer_size) != JXL_DEC_SUCCESS)
      {
        free(pixels);
        return IM_ERR_ACCESS;
      }
      break;

    case JXL_DEC_FULL_IMAGE:
      /* The first displayed frame. An animation has more, which this driver
         does not read. */
      done = 1;
      break;

    case JXL_DEC_NEED_MORE_INPUT:   /* the file was cut short */
    case JXL_DEC_SUCCESS:           /* finished without producing an image */
    case JXL_DEC_ERROR:
    default:
      free(pixels);
      return IM_ERR_ACCESS;
    }
  }

  if (!pixels)
    return IM_ERR_ACCESS;

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Reading JPEG XL...");

  for (int i = 0; i < count; i++)
  {
    memcpy(this->line_buffer, pixels + (size_t)lin * line_bytes, line_bytes);

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

int imFileFormatJXL::WriteImageInfo()
{
  int color_space = imColorModeSpace(this->user_color_mode);

  if (this->compression[0] != 0 && !imStrEqual(this->compression, "JXL"))
    return IM_ERR_COMPRESS;
  strcpy(this->compression, "JXL");

  /* No data type or colour conversion on the write path: the driver gets
     what the caller has, and binary is widened to gray in WriteImageData. */
  this->file_data_type = this->user_data_type;
  if (this->file_data_type != IM_BYTE && this->file_data_type != IM_USHORT &&
      this->file_data_type != IM_FLOAT)
    return IM_ERR_DATA;

  if (color_space == IM_GRAY || color_space == IM_BINARY)
  {
    this->file_color_mode = color_space;
    this->channel_count = 1;
  }
  else if (color_space == IM_RGB)
  {
    this->file_color_mode = IM_RGB;
    this->channel_count = 3;
  }
  else
    return IM_ERR_DATA;

  this->has_alpha = imColorModeHasAlpha(this->user_color_mode);
  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count++;
  }

  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  return IM_ERR_NONE;
}

int imFileFormatJXL::WriteImageData(void* data)
{
  imAttribTable* attrib_table = AttribTable();
  int is_binary = (imColorModeSpace(this->file_color_mode) == IM_BINARY);
  int is_float = (this->file_data_type == IM_FLOAT);
  int bits = imDataTypeSize(this->file_data_type) * 8;
  size_t line_bytes = (size_t)this->width * (size_t)this->channel_count *
                      (size_t)iSampleSize();
  size_t pixels_size = line_bytes * (size_t)this->height;
  int count, lin = 0, plane = 0, lossless = 0, quality = IM_JXL_DEFAULT_QUALITY;
  int error = IM_ERR_NONE;
  uint8_t* pixels;
  uint8_t* output = NULL;
  size_t output_size = 0;
  JxlEncoder* encoder = NULL;
  JxlEncoderFrameSettings* settings = NULL;
  JxlBasicInfo basic;
  JxlColorEncoding color;
  JxlPixelFormat format;

  if (!this->write_file_name)
    return IM_ERR_ACCESS;

  {
    int* attrib = (int*)attrib_table->Get("Lossless");
    if (attrib && *attrib)
      lossless = 1;

    attrib = (int*)attrib_table->Get("Quality");
    if (attrib)
      quality = *attrib;
    if (quality < 0) quality = 0;
    if (quality > 100) quality = 100;
  }

  pixels = (uint8_t*)malloc(pixels_size);
  if (!pixels)
    return IM_ERR_MEM;

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Writing JPEG XL...");

  for (int i = 0; i < count; i++)
  {
    uint8_t* target = pixels + (size_t)lin * line_bytes;

    imFileLineBufferWrite(this, data, lin, plane);
    memcpy(target, this->line_buffer, line_bytes);

    /* Binary data is 0 and 1. Stored as is, it would be two near-black
       grays; widen it so the file looks like what it is. */
    if (is_binary)
    {
      for (int x = 0; x < this->width; x++)
      {
        uint8_t* sample = target + (size_t)x * (size_t)this->channel_count;
        *sample = *sample? 255: 0;
      }
    }

    if (!imCounterInc(this->counter))
    {
      free(pixels);
      return IM_ERR_COUNTER;
    }

    imFileLineBufferInc(this, &lin, &plane);
  }

  encoder = JxlEncoderCreate(NULL);
  if (!encoder)
  {
    free(pixels);
    return IM_ERR_MEM;
  }

  this->runner = JxlThreadParallelRunnerCreate(NULL,
                   JxlThreadParallelRunnerDefaultNumWorkerThreads());
  if (this->runner)
    JxlEncoderSetParallelRunner(encoder, JxlThreadParallelRunner, this->runner);

  JxlEncoderInitBasicInfo(&basic);
  basic.xsize = (uint32_t)this->width;
  basic.ysize = (uint32_t)this->height;
  basic.bits_per_sample = (uint32_t)bits;
  basic.exponent_bits_per_sample = is_float? 8: 0;
  basic.num_color_channels = (this->channel_count >= 3)? 3: 1;
  basic.num_extra_channels = this->has_alpha? 1: 0;
  basic.alpha_bits = this->has_alpha? (uint32_t)bits: 0;
  basic.alpha_exponent_bits = (this->has_alpha && is_float)? 8: 0;

  /* Lossless requires the samples be kept in their own colour space rather
     than converted to XYB, which is what this flag means. */
  basic.uses_original_profile = lossless? JXL_TRUE: JXL_FALSE;

  /* Level 5, the default, stops at 16 bits per sample. 32 bit float needs
     level 10, and 0.7 refuses the basic info rather than raising it. */
  if (bits > 16 && JxlEncoderSetCodestreamLevel(encoder, 10) != JXL_ENC_SUCCESS)
    error = IM_ERR_DATA;

  if (error == IM_ERR_NONE &&
      JxlEncoderSetBasicInfo(encoder, &basic) != JXL_ENC_SUCCESS)
    error = IM_ERR_DATA;

  if (error == IM_ERR_NONE)
  {
    JxlColorEncodingSetToSRGB(&color, basic.num_color_channels == 1? JXL_TRUE: JXL_FALSE);
    if (JxlEncoderSetColorEncoding(encoder, &color) != JXL_ENC_SUCCESS)
      error = IM_ERR_DATA;
  }

  if (error == IM_ERR_NONE)
  {
    settings = JxlEncoderFrameSettingsCreate(encoder, NULL);
    if (!settings)
      error = IM_ERR_MEM;
    else if (lossless)
    {
      if (JxlEncoderSetFrameLossless(settings, JXL_TRUE) != JXL_ENC_SUCCESS)
        error = IM_ERR_COMPRESS;
    }
    else if (JxlEncoderSetFrameDistance(settings, iJXLDistance(quality))
             != JXL_ENC_SUCCESS)
      error = IM_ERR_COMPRESS;
  }

  if (error == IM_ERR_NONE)
  {
    format.num_channels = (uint32_t)this->channel_count;
    format.data_type = iPixelType();
    format.endianness = JXL_NATIVE_ENDIAN;
    format.align = 0;

    if (JxlEncoderAddImageFrame(settings, &format, pixels, pixels_size)
        != JXL_ENC_SUCCESS)
      error = IM_ERR_COMPRESS;
    JxlEncoderCloseInput(encoder);
  }

  free(pixels);
  pixels = NULL;

  if (error == IM_ERR_NONE)
  {
    size_t capacity = 65536;
    JxlEncoderStatus status;

    output = (uint8_t*)malloc(capacity);
    if (!output)
      error = IM_ERR_MEM;

    while (error == IM_ERR_NONE)
    {
      uint8_t* next = output + output_size;
      size_t avail = capacity - output_size;

      status = JxlEncoderProcessOutput(encoder, &next, &avail);
      output_size = (size_t)(next - output);

      if (status == JXL_ENC_SUCCESS)
        break;

      if (status != JXL_ENC_NEED_MORE_OUTPUT)
      {
        error = IM_ERR_COMPRESS;
        break;
      }

      capacity *= 2;
      uint8_t* grown = (uint8_t*)realloc(output, capacity);
      if (!grown)
      {
        error = IM_ERR_MEM;
        break;
      }
      output = grown;
    }
  }

  JxlEncoderDestroy(encoder);
  if (this->runner)
  {
    JxlThreadParallelRunnerDestroy(this->runner);
    this->runner = NULL;
  }

  if (error == IM_ERR_NONE)
  {
    FILE* file = fopen(this->write_file_name, "wb");
    if (!file)
      error = IM_ERR_OPEN;
    else
    {
      if (fwrite(output, 1, output_size, file) != output_size)
        error = IM_ERR_ACCESS;
      if (fclose(file) != 0)
        error = IM_ERR_ACCESS;
    }
  }

  free(output);
  return error;
}

int imFormatJXL::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_GRAY &&
      color_space != IM_BINARY)
    return IM_ERR_DATA;

  if (data_type != IM_BYTE && data_type != IM_USHORT && data_type != IM_FLOAT)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (!imStrEqual(compression, "JXL"))
    return IM_ERR_COMPRESS;

  return IM_ERR_NONE;
}
