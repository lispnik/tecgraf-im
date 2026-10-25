/** \file
 * \brief CAMRAW - Digital Camera Raw Files
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>

#include <libraw/libraw.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_camraw.h"
#include "im_util.h"
#include "im_counter.h"

/* Read-only, through LibRaw's C API: one call identifies the camera and
   unpacks the sensor data, another demosaics, white balances and converts it
   to output RGB, and a third hands it back as an interleaved bitmap.

   That processing is not cheap and its result is what decides the size --
   half_size halves it, and the camera's orientation can swap width and
   height -- so it runs in ReadImageInfo, the way the HEIF driver decodes
   there to tell gray from colour.

   The format is called CAMRAW because IM already has a "RAW" driver, for
   headerless sample data, which is a different thing entirely. */

static const char* iRawCompTable[1] =
{
  "NONE"
};


/*******************************************************************/


class imFileFormatCAMRAW: public imFileFormatBase
{
  libraw_data_t* raw;
  libraw_processed_image_t* bitmap;
  int channel_count;

public:
  imFileFormatCAMRAW(const imFormat* _iformat)
    : imFileFormatBase(_iformat), raw(NULL), bitmap(NULL), channel_count(3) {}
  ~imFileFormatCAMRAW() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatCAMRAW: public imFormat
{
public:
  imFormatCAMRAW()
    :imFormat("CAMRAW",
              "Digital Camera Raw Files",
              /* imFormatInfo strcpy's this into the caller's buffer, which
                 im.h says need hold no more than 50 characters, and imlua's
                 is 50 bytes on the stack. So the commonest only: LibRaw
                 identifies a file by its content, not by its name. */
              "*.dng;*.cr2;*.cr3;*.nef;*.arw;*.raf;*.orf;*.rw2;",
              iRawCompTable,
              1,
              0)
    { extra = "LibRaw"; }
  ~imFormatCAMRAW() {}

  imFileFormatBase* Create(void) const { return new imFileFormatCAMRAW(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterCAMRAW(void)
{
  imFormatRegister(new imFormatCAMRAW());
}


/*******************************************************************/


int imFileFormatCAMRAW::Open(const char* file_name)
{
  this->raw = libraw_init(0);
  if (!this->raw)
    return IM_ERR_MEM;

  /* LibRaw identifies the camera from the file's contents, not its name, and
     reports anything it does not recognise as unsupported -- which is "not
     my format" to imFileOpen, so the other drivers still get a turn. */
  int result = libraw_open_file(this->raw, file_name);
  if (result != LIBRAW_SUCCESS)
  {
    libraw_close(this->raw);
    this->raw = NULL;

    if (result == LIBRAW_UNSUFFICIENT_MEMORY)
      return IM_ERR_MEM;
    return IM_ERR_FORMAT;
  }

  strcpy(this->compression, "NONE");
  this->image_count = 1;
  return IM_ERR_NONE;
}

int imFileFormatCAMRAW::New(const char* file_name)
{
  (void)file_name;
  return IM_ERR_FORMAT;   /* read only */
}

void imFileFormatCAMRAW::Close()
{
  if (this->bitmap)
  {
    libraw_dcraw_clear_mem(this->bitmap);
    this->bitmap = NULL;
  }

  if (this->raw)
  {
    libraw_close(this->raw);
    this->raw = NULL;
  }
}

void* imFileFormatCAMRAW::Handle(int index)
{
  if (index == 0)
    return (void*)this->raw;
  return NULL;
}

static void iRawSetString(imAttribTable* table, const char* name, const char* value)
{
  if (value && value[0])
    table->Set(name, IM_BYTE, (int)strlen(value) + 1, value);
}

static void iRawSetFloat(imAttribTable* table, const char* name, float value)
{
  if (value > 0)
    table->Set(name, IM_FLOAT, 1, &value);
}

int imFileFormatCAMRAW::ReadImageInfo(int index)
{
  if (index != 0 || !this->raw)
    return IM_ERR_DATA;

  imAttribTable* attrib_table = AttribTable();

  /* Processing choices, read from attributes the caller may have set
     between imFileOpen and imFileReadImageInfo. The defaults are the ones
     that give a viewable picture: 16 bits, the camera's own white balance,
     sRGB primaries and the standard gamma curve. */
  libraw_output_params_t* params = &this->raw->params;
  params->output_bps = 16;
  params->use_camera_wb = 1;
  params->output_color = 1;          /* sRGB */

  int* linear = (int*)attrib_table->Get("RawLinear");
  if (linear && *linear)
  {
    /* Scene-linear: no tone curve and no automatic brightening, for anyone
       doing photometry or their own processing. */
    params->gamm[0] = 1.0;
    params->gamm[1] = 1.0;
    params->no_auto_bright = 1;
  }

  int* half = (int*)attrib_table->Get("RawHalfSize");
  if (half && *half)
    params->half_size = 1;           /* one pixel per 2x2 CFA block, no demosaic */

  if (this->bitmap)
  {
    libraw_dcraw_clear_mem(this->bitmap);
    this->bitmap = NULL;
  }

  int result = libraw_unpack(this->raw);
  if (result == LIBRAW_SUCCESS)
    result = libraw_dcraw_process(this->raw);
  if (result != LIBRAW_SUCCESS)
    return (result == LIBRAW_UNSUFFICIENT_MEMORY)? IM_ERR_MEM: IM_ERR_ACCESS;

  int error = 0;
  this->bitmap = libraw_dcraw_make_mem_image(this->raw, &error);
  if (!this->bitmap)
    return (error == LIBRAW_UNSUFFICIENT_MEMORY)? IM_ERR_MEM: IM_ERR_ACCESS;

  if (this->bitmap->type != LIBRAW_IMAGE_BITMAP ||
      (this->bitmap->colors != 1 && this->bitmap->colors != 3) ||
      (this->bitmap->bits != 8 && this->bitmap->bits != 16))
    return IM_ERR_DATA;

  this->width = this->bitmap->width;
  this->height = this->bitmap->height;
  this->channel_count = this->bitmap->colors;
  this->file_data_type = (this->bitmap->bits == 16)? IM_USHORT: IM_BYTE;

  /* A monochrome sensor (Leica Monochrom, converted cameras) has no CFA and
     comes back with one colour. */
  this->file_color_mode = (this->channel_count == 1)? IM_GRAY: (IM_RGB | IM_PACKED);
  this->file_color_mode |= IM_TOPDOWN;

  /* The same names the JPEG driver gives these EXIF tags, so code reading a
     camera's metadata does not care which file it came from. */
  iRawSetString(attrib_table, "Make", this->raw->idata.make);
  iRawSetString(attrib_table, "Model", this->raw->idata.model);
  iRawSetFloat(attrib_table, "ISOSpeedRatings", this->raw->other.iso_speed);
  iRawSetFloat(attrib_table, "ExposureTime", this->raw->other.shutter);
  iRawSetFloat(attrib_table, "FNumber", this->raw->other.aperture);
  iRawSetFloat(attrib_table, "FocalLength", this->raw->other.focal_len);

  return IM_ERR_NONE;
}

int imFileFormatCAMRAW::ReadImageData(void* data)
{
  if (!this->bitmap)
    return IM_ERR_ACCESS;

  size_t line_bytes = (size_t)this->width * (size_t)this->channel_count *
                      (size_t)(this->bitmap->bits / 8);

  /* data_size is LibRaw's own account of the buffer; never trust the
     geometry alone to stay inside it. */
  if ((size_t)this->bitmap->data_size < line_bytes * (size_t)this->height)
    return IM_ERR_DATA;

  int count = imFileLineBufferCount(this);
  int lin = 0, plane = 0;
  imCounterTotal(this->counter, count, "Reading Camera Raw...");

  for (int i = 0; i < count; i++)
  {
    /* 16 bit samples are in the machine's own byte order. */
    memcpy(this->line_buffer, this->bitmap->data + (size_t)lin * line_bytes,
           line_bytes);

    imFileLineBufferRead(this, data, lin, plane);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;

    imFileLineBufferInc(this, &lin, &plane);
  }

  return IM_ERR_NONE;
}

int imFileFormatCAMRAW::WriteImageInfo()
{
  return IM_ERR_FORMAT;
}

int imFileFormatCAMRAW::WriteImageData(void* data)
{
  (void)data;
  return IM_ERR_FORMAT;
}

int imFormatCAMRAW::CanWrite(const char* compression, int color_mode, int data_type) const
{
  (void)compression;
  (void)color_mode;
  (void)data_type;
  return IM_ERR_FORMAT;   /* read only */
}
