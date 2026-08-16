/** \file
 * \brief HEIF/AVIF - ISO Base Media File Format Images
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>
#include <assert.h>

#include <libheif/heif.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_heif.h"
#include "im_util.h"
#include "im_counter.h"

/* One include is enough for every libheif release: <libheif/heif.h> is an
   umbrella header. 1.20 split the API into heif_decoding.h, heif_encoding.h
   and friends, but kept the umbrella including all of them, so this source
   builds unchanged against 1.17 through current.

   1.17 is the floor because heif_brand2_avif, which iHEIFFileIsAVIF needs,
   arrived in 1.16 and 1.17.6 is what the oldest CI runner ships; see the
   longer note in CMakeLists.txt. Note also that
   distributions increasingly ship libheif's codecs as separate plugins
   (Ubuntu 24.04 does), so a libheif that links fine can still have no
   encoder at run time -- WriteImageInfo reports that as IM_ERR_COMPRESS. */

#define IM_HEIF_DEFAULT_QUALITY 80

/* HEVC (Main 12) and AV1 both stop at 12 bits per sample, so IM_USHORT data
   is written at 12 and scaled, rather than refused outright the way the JPEG
   driver refuses anything but IM_BYTE. The read path scales 10/12 bit files
   up to the full 16 bit range, and this is the matching step down. */
#define IM_HEIF_MAX_BITS 12

static const char* iHEIFCompTable[1] = { "HEVC" };
static const char* iAVIFCompTable[1] = { "AV1" };


/*******************************************************************/


class imFileFormatHEIF: public imFileFormatBase
{
  heif_context* ctx;
  heif_image_handle* handle;   /* the image selected by ReadImageInfo */
  heif_image* image;           /* decoded pixels, or the image being written */
  heif_encoder* encoder;

  char* write_file_name;       /* New() cannot write yet: libheif encodes the
                                  whole image at once, so the name is kept
                                  until WriteImageData has the pixels. */
  int is_avif;                 /* selects the encoder and the compression name */
  int bits_per_sample;         /* of the file, 8..16 */
  int has_alpha;
  int channel_count;           /* interleaved samples per pixel */

  int iSelectImage(int index);

public:
  imFileFormatHEIF(const imFormat* _iformat, int _is_avif)
    : imFileFormatBase(_iformat), ctx(NULL), handle(NULL), image(NULL),
      encoder(NULL), write_file_name(NULL), is_avif(_is_avif),
      bits_per_sample(8), has_alpha(0), channel_count(3) {}
  ~imFileFormatHEIF() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatHEIF: public imFormat
{
public:
  imFormatHEIF()
    :imFormat("HEIF",
              "HEIF/HEIC Image File Format",
              "*.heic;*.heif;",
              iHEIFCompTable,
              1,
              1)
    { extra = "libheif"; }
  ~imFormatHEIF() {}

  imFileFormatBase* Create(void) const { return new imFileFormatHEIF(this, 0); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

class imFormatAVIF: public imFormat
{
public:
  imFormatAVIF()
    :imFormat("AVIF",
              "AV1 Image File Format",
              "*.avif;",
              iAVIFCompTable,
              1,
              1)
    { extra = "libheif"; }
  ~imFormatAVIF() {}

  imFileFormatBase* Create(void) const { return new imFileFormatHEIF(this, 1); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterHEIF(void)
{
  imFormatRegister(new imFormatHEIF());
  imFormatRegister(new imFormatAVIF());
}


/*******************************************************************/


/* libheif reports errors as a struct, never a code we can hand straight back
   to IM, so the mapping is explicit. The textual message is always defined. */
static int iHEIFError(struct heif_error err)
{
  switch (err.code)
  {
  case heif_error_Ok:
    return IM_ERR_NONE;
  case heif_error_Input_does_not_exist:
    return IM_ERR_OPEN;
  case heif_error_Invalid_input:
  case heif_error_Unsupported_filetype:
    return IM_ERR_FORMAT;
  /* Everything the codec itself refuses, not just what libheif calls an
     unsupported feature. A codec that cannot encode what it was handed, or
     whose plugin will not load, is a compression failure from IM's point of
     view -- lumping those into the default arm reported them as IM_ERR_ACCESS
     and made a codec limitation indistinguishable from an I/O problem.

     Found from CI rather than by reading: vcpkg's x265 is an 8-bit build, and
     asking it for the 12-bit encode IM_USHORT requires failed on Windows with
     one of these rather than with Unsupported_feature, so the 12-bit test
     could not tell "this codec was built without high bit depth" from a real
     defect. */
  case heif_error_Unsupported_feature:
  case heif_error_Decoder_plugin_error:
  case heif_error_Encoder_plugin_error:
  case heif_error_Encoding_error:
  case heif_error_Plugin_loading_error:
    return IM_ERR_COMPRESS;
  case heif_error_Memory_allocation_error:
    return IM_ERR_MEM;
  default:
    return IM_ERR_ACCESS;
  }
}

/* Reads the ftyp brand so the compression can be reported accurately even
   when a file is opened through the other driver -- an .avif handed to the
   HEIF driver still reports AV1. Falls back to the driver's own default when
   the header is too short or unrecognised. */
static int iHEIFFileIsAVIF(const char* file_name, int default_is_avif)
{
  unsigned char header[16];
  size_t read_count;
  FILE* file = fopen(file_name, "rb");

  if (!file)
    return default_is_avif;

  read_count = fread(header, 1, sizeof(header), file);
  fclose(file);

  if (read_count < 12)
    return default_is_avif;

  if (heif_read_main_brand(header, (int)read_count) == heif_brand2_avif)
    return 1;

  return 0;
}

int imFileFormatHEIF::iSelectImage(int index)
{
  heif_item_id* ids;
  struct heif_error err;
  int count;

  if (this->handle)
  {
    heif_image_handle_release(this->handle);
    this->handle = NULL;
  }

  /* Index 0 is the primary image rather than the first in file order: for a
     Live Photo or a burst the primary is the one a viewer shows. */
  if (index == 0)
  {
    err = heif_context_get_primary_image_handle(this->ctx, &this->handle);
    return iHEIFError(err);
  }

  count = heif_context_get_number_of_top_level_images(this->ctx);
  if (index < 0 || index >= count)
    return IM_ERR_DATA;

  ids = (heif_item_id*)malloc(sizeof(heif_item_id) * (size_t)count);
  if (!ids)
    return IM_ERR_MEM;

  heif_context_get_list_of_top_level_image_IDs(this->ctx, ids, count);
  err = heif_context_get_image_handle(this->ctx, ids[index], &this->handle);
  free(ids);

  return iHEIFError(err);
}

int imFileFormatHEIF::Open(const char* file_name)
{
  struct heif_error err;
  int count;

  this->ctx = heif_context_alloc();
  if (!this->ctx)
    return IM_ERR_MEM;

  err = heif_context_read_from_file(this->ctx, file_name, NULL);
  if (err.code != heif_error_Ok)
  {
    heif_context_free(this->ctx);
    this->ctx = NULL;

    /* Report "not my format" so imFileOpen can go on trying other drivers,
       rather than failing the open outright. */
    return IM_ERR_FORMAT;
  }

  count = heif_context_get_number_of_top_level_images(this->ctx);
  if (count < 1)
  {
    heif_context_free(this->ctx);
    this->ctx = NULL;
    return IM_ERR_FORMAT;
  }

  this->is_avif = iHEIFFileIsAVIF(file_name, this->is_avif);
  strcpy(this->compression, this->is_avif? "AV1": "HEVC");
  this->image_count = count;

  return IM_ERR_NONE;
}

int imFileFormatHEIF::New(const char* file_name)
{
  this->ctx = heif_context_alloc();
  if (!this->ctx)
    return IM_ERR_MEM;

  /* libheif encodes and writes the complete image in one call, so there is
     nothing to open yet -- keep the name for WriteImageData. */
  this->write_file_name = (char*)malloc(strlen(file_name) + 1);
  if (!this->write_file_name)
  {
    heif_context_free(this->ctx);
    this->ctx = NULL;
    return IM_ERR_MEM;
  }
  strcpy(this->write_file_name, file_name);

  strcpy(this->compression, this->is_avif? "AV1": "HEVC");
  this->image_count = 1;

  return IM_ERR_NONE;
}

void imFileFormatHEIF::Close()
{
  if (this->image)
  {
    heif_image_release(this->image);
    this->image = NULL;
  }

  if (this->handle)
  {
    heif_image_handle_release(this->handle);
    this->handle = NULL;
  }

  if (this->encoder)
  {
    heif_encoder_release(this->encoder);
    this->encoder = NULL;
  }

  if (this->ctx)
  {
    heif_context_free(this->ctx);
    this->ctx = NULL;
  }

  if (this->write_file_name)
  {
    free(this->write_file_name);
    this->write_file_name = NULL;
  }
}

void* imFileFormatHEIF::Handle(int index)
{
  if (index == 0)
    return (void*)this->ctx;
  else if (index == 1)
    return (void*)this->handle;
  else if (index == 2)
    return (void*)this->image;
  else
    return NULL;
}

int imFileFormatHEIF::ReadImageInfo(int index)
{
  int error, luma_bits;

  error = iSelectImage(index);
  if (error != IM_ERR_NONE)
    return error;

  this->width = heif_image_handle_get_width(this->handle);
  this->height = heif_image_handle_get_height(this->handle);
  if (this->width <= 0 || this->height <= 0)
    return IM_ERR_DATA;

  luma_bits = heif_image_handle_get_luma_bits_per_pixel(this->handle);
  if (luma_bits <= 0)
    luma_bits = 8;                 /* unknown: assume the common case */
  if (luma_bits > 16)
    return IM_ERR_DATA;

  this->bits_per_sample = luma_bits;
  this->file_data_type = (luma_bits > 8)? IM_USHORT: IM_BYTE;
  this->has_alpha = heif_image_handle_has_alpha_channel(this->handle);

  if (this->image)
  {
    heif_image_release(this->image);
    this->image = NULL;
  }

  /* Decoding happens here rather than in ReadImageData, because deciding
     between gray and RGB is what requires it.
     heif_image_handle_get_chroma_bits_per_pixel reports 8 for monochrome and
     colour alike, so it cannot discriminate;
     heif_image_handle_get_preferred_decoding_colorspace can, but is not in
     every libheif this driver supports. Asking for a monochrome decode and
     seeing whether the conversion is refused works on all of them.

     An image with alpha always takes the RGB path, so the alpha arrives
     interleaved with the colour rather than in a separate plane this driver
     would have to fetch on its own. */
  if (!this->has_alpha)
  {
    struct heif_error mono = heif_decode_image(this->handle, &this->image,
                                               heif_colorspace_monochrome,
                                               heif_chroma_monochrome, NULL);
    if (mono.code == heif_error_Ok)
    {
      this->file_color_mode = IM_GRAY;
      this->channel_count = 1;
      this->file_color_mode |= IM_TOPDOWN;
      strcpy(this->compression, this->is_avif? "AV1": "HEVC");
      return IM_ERR_NONE;
    }

    this->image = NULL;   /* a failed decode leaves nothing to release */
  }

  this->file_color_mode = IM_RGB;
  this->channel_count = 3;

  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count++;
  }

  {
    enum heif_chroma chroma;
    if (this->file_data_type == IM_USHORT)
      chroma = this->has_alpha? heif_chroma_interleaved_RRGGBBAA_LE
                              : heif_chroma_interleaved_RRGGBB_LE;
    else
      chroma = this->has_alpha? heif_chroma_interleaved_RGBA
                              : heif_chroma_interleaved_RGB;

    struct heif_error err = heif_decode_image(this->handle, &this->image,
                                              heif_colorspace_RGB, chroma, NULL);
    if (err.code != heif_error_Ok)
    {
      this->image = NULL;
      return iHEIFError(err);
    }
  }

  /* libheif hands back interleaved samples, top row first. IM_PACKED makes
     imFileLineBufferRead do the de-interleaving into IM'''s planar image. */
  this->file_color_mode |= IM_TOPDOWN | IM_PACKED;

  strcpy(this->compression, this->is_avif? "AV1": "HEVC");

  return IM_ERR_NONE;
}

int imFileFormatHEIF::ReadImageData(void* data)
{
  const uint8_t* pixels;
  int stride = 0, count, line_bytes, lin = 0, plane = 0;
  int is_gray = (imColorModeSpace(this->file_color_mode) == IM_GRAY);

  /* ReadImageInfo already decoded: it had to, to tell gray from colour. */
  if (!this->image)
    return IM_ERR_ACCESS;

  pixels = heif_image_get_plane_readonly(this->image,
                                         is_gray? heif_channel_Y
                                                : heif_channel_interleaved,
                                         &stride);
  if (!pixels || stride <= 0)
    return IM_ERR_ACCESS;

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Reading HEIF...");

  line_bytes = this->width * this->channel_count *
               ((this->file_data_type == IM_USHORT)? 2: 1);

  /* A short stride would mean libheif and this driver disagree about the
     layout; copying line_bytes from it would read past each row. */
  if (stride < line_bytes)
    return IM_ERR_ACCESS;

  for (int i = 0; i < count; i++)
  {
    memcpy(this->line_buffer, pixels + (size_t)lin * (size_t)stride,
           (size_t)line_bytes);

    /* libheif gives samples in the file's own depth (10 bit values sit in
       0..1023), while IM_USHORT means the full 16 bit range. Scale so that
       an 8, 10, 12 and 16 bit file all look alike once loaded. */
    if (this->file_data_type == IM_USHORT && this->bits_per_sample < 16)
    {
      int shift = 16 - this->bits_per_sample;
      imushort* samples = (imushort*)this->line_buffer;
      int sample_count = this->width * this->channel_count;
      for (int s = 0; s < sample_count; s++)
        samples[s] = (imushort)(samples[s] << shift);
    }

    imFileLineBufferRead(this, data, lin, plane);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;

    imFileLineBufferInc(this, &lin, &plane);
  }

  return IM_ERR_NONE;
}

int imFileFormatHEIF::WriteImageInfo()
{
  struct heif_error err;
  imAttribTable* attrib_table = AttribTable();
  int color_space = imColorModeSpace(this->user_color_mode);
  int* quality;
  int* lossless;

  this->file_data_type = this->user_data_type;

  if (color_space == IM_GRAY || color_space == IM_BINARY)
  {
    this->file_color_mode = IM_GRAY;
    this->channel_count = 1;
  }
  else
  {
    this->file_color_mode = IM_RGB;
    this->channel_count = 3;
  }

  this->has_alpha = imColorModeHasAlpha(this->user_color_mode);
  if (this->has_alpha)
  {
    this->file_color_mode |= IM_ALPHA;
    this->channel_count++;
  }

  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  this->bits_per_sample = (this->file_data_type == IM_USHORT)? IM_HEIF_MAX_BITS: 8;

  err = heif_context_get_encoder_for_format(this->ctx,
                                            this->is_avif? heif_compression_AV1
                                                         : heif_compression_HEVC,
                                            &this->encoder);
  if (err.code != heif_error_Ok)
  {
    /* The container is supported but no encoder for this codec was compiled
       in or found as a plugin -- a decode-only libheif build. */
    this->encoder = NULL;
    return IM_ERR_COMPRESS;
  }

  lossless = (int*)attrib_table->Get("Lossless");
  if (lossless && *lossless)
  {
    /* Subsampled chroma defeats the whole point: the default 4:2:0 discards
       three quarters of the colour-difference samples before the encoder sees
       them, and a "lossless" RGB round-trip came back off by as much as 75 of
       255. Ask for 4:4:4 and tolerate a refusal -- a third-party encoder
       plugin may not offer it, and gray has no chroma to subsample. */
    if (imColorModeSpace(this->file_color_mode) != IM_GRAY)
      heif_encoder_set_parameter_string(this->encoder, "chroma", "444");

    if (this->is_avif)
    {
      /* aom refuses libheif's lossless flag outright: it reports "Only
         --enable_chroma_deltaq=0 can be used with --lossless=1", and the
         encode fails. Pinning the quantiser range to zero reaches the same
         place through settings aom does accept. */
      heif_encoder_set_parameter_integer(this->encoder, "min-q", 0);
      heif_encoder_set_parameter_integer(this->encoder, "max-q", 0);
    }
    else
    {
      err = heif_encoder_set_lossless(this->encoder, 1);
      if (err.code != heif_error_Ok)
        return IM_ERR_COMPRESS;
    }
  }
  else
  {
    quality = (int*)attrib_table->Get("Quality");
    int value = quality? *quality: IM_HEIF_DEFAULT_QUALITY;
    if (value < 0) value = 0;
    if (value > 100) value = 100;

    err = heif_encoder_set_lossy_quality(this->encoder, value);
    if (err.code != heif_error_Ok)
      return IM_ERR_COMPRESS;
  }

  strcpy(this->compression, this->is_avif? "AV1": "HEVC");

  return IM_ERR_NONE;
}

int imFileFormatHEIF::WriteImageData(void* data)
{
  struct heif_error err;
  enum heif_colorspace colorspace;
  enum heif_chroma chroma;
  heif_image_handle* out_handle = NULL;
  uint8_t* pixels;
  int stride = 0, count, line_bytes, lin = 0, plane = 0;
  int is_gray = (imColorModeSpace(this->file_color_mode) == IM_GRAY);

  if (!this->encoder)
    return IM_ERR_COMPRESS;

  if (is_gray)
  {
    colorspace = heif_colorspace_monochrome;
    chroma = heif_chroma_monochrome;
  }
  else
  {
    colorspace = heif_colorspace_RGB;
    if (this->file_data_type == IM_USHORT)
      chroma = this->has_alpha? heif_chroma_interleaved_RRGGBBAA_LE
                              : heif_chroma_interleaved_RRGGBB_LE;
    else
      chroma = this->has_alpha? heif_chroma_interleaved_RGBA
                              : heif_chroma_interleaved_RGB;
  }

  err = heif_image_create(this->width, this->height, colorspace, chroma,
                          &this->image);
  if (err.code != heif_error_Ok)
    return iHEIFError(err);

  err = heif_image_add_plane(this->image,
                             is_gray? heif_channel_Y: heif_channel_interleaved,
                             this->width, this->height, this->bits_per_sample);
  if (err.code != heif_error_Ok)
    return iHEIFError(err);

  pixels = heif_image_get_plane(this->image,
                                is_gray? heif_channel_Y
                                       : heif_channel_interleaved,
                                &stride);
  if (!pixels || stride <= 0)
    return IM_ERR_ACCESS;

  count = imFileLineBufferCount(this);
  imCounterTotal(this->counter, count, "Writing HEIF...");

  line_bytes = this->width * this->channel_count *
               ((this->file_data_type == IM_USHORT)? 2: 1);

  if (stride < line_bytes)
    return IM_ERR_ACCESS;

  for (int i = 0; i < count; i++)
  {
    imFileLineBufferWrite(this, data, lin, plane);

    if (this->file_data_type == IM_USHORT && this->bits_per_sample < 16)
    {
      /* IM_USHORT spans the full 16 bit range; the file holds 12. Scale down
         here so the read path's matching shift-up returns the value to where
         it started (to within the bits the format cannot carry). */
      int shift = 16 - this->bits_per_sample;
      const imushort* source = (const imushort*)this->line_buffer;
      imushort* target = (imushort*)(pixels + (size_t)lin * (size_t)stride);
      int sample_count = this->width * this->channel_count;
      for (int s = 0; s < sample_count; s++)
        target[s] = (imushort)(source[s] >> shift);
    }
    else
      memcpy(pixels + (size_t)lin * (size_t)stride, this->line_buffer,
             (size_t)line_bytes);

    if (!imCounterInc(this->counter))
      return IM_ERR_COUNTER;

    imFileLineBufferInc(this, &lin, &plane);
  }

  err = heif_context_encode_image(this->ctx, this->image, this->encoder,
                                  NULL, &out_handle);
  if (out_handle)
    heif_image_handle_release(out_handle);
  if (err.code != heif_error_Ok)
    return iHEIFError(err);

  err = heif_context_write_to_file(this->ctx, this->write_file_name);
  if (err.code != heif_error_Ok)
    return iHEIFError(err);

  this->image_count = 1;

  return IM_ERR_NONE;
}

/* Shared by both drivers: the container differs, the pixel constraints do
   not. Only the accepted compression name is per-format. */
static int iHEIFCanWrite(const char* compression, int color_mode, int data_type,
                         const char* codec)
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_GRAY &&
      color_space != IM_BINARY)
    return IM_ERR_DATA;

  if (data_type != IM_BYTE && data_type != IM_USHORT)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  if (!imStrEqual(compression, codec))
    return IM_ERR_COMPRESS;

  return IM_ERR_NONE;
}

int imFormatHEIF::CanWrite(const char* compression, int color_mode, int data_type) const
{
  return iHEIFCanWrite(compression, color_mode, data_type, "HEVC");
}

int imFormatAVIF::CanWrite(const char* compression, int color_mode, int data_type) const
{
  return iHEIFCanWrite(compression, color_mode, data_type, "AV1");
}
