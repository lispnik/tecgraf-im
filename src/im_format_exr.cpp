/** \file
 * \brief EXR - OpenEXR High Dynamic Range Image Format
 *
 * See Copyright Notice in im_lib.h
 */

#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <stdio.h>

#include <exception>
#include <new>
#include <string>
#include <vector>

#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfRgbaFile.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfCompression.h>
#include <Iex.h>
#include <half.h>

#include "im.h"
#include "im_file.h"
#include "im_format.h"
#include "im_format_exr.h"
#include "im_util.h"
#include "im_counter.h"

/* The C++ API rather than OpenEXRCore's C one: it has had the same shape
   from 3.1 -- Ubuntu 24.04's, the oldest CI builds against -- to current, and
   on read it converts between half, float and uint on its own, so every file
   reads into one float buffer whatever its channels hold. (It does not on
   write; see WriteImageData.)

   OpenEXR reports every failure by throwing. No exception may cross back into
   IM, which is C at heart, so each entry point that calls the library catches
   everything and maps it to an IM error code. */

static const char* iEXRCompTable[10] =
{
  "NONE", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB"
};

static const Imf::Compression iEXRCompValue[10] =
{
  Imf::NO_COMPRESSION, Imf::RLE_COMPRESSION, Imf::ZIPS_COMPRESSION,
  Imf::ZIP_COMPRESSION, Imf::PIZ_COMPRESSION, Imf::PXR24_COMPRESSION,
  Imf::B44_COMPRESSION, Imf::B44A_COMPRESSION, Imf::DWAA_COMPRESSION,
  Imf::DWAB_COMPRESSION
};

#define IM_EXR_DEFAULT_COMPRESSION "ZIP"


/*******************************************************************/


class imFileFormatEXR: public imFileFormatBase
{
  Imf::InputFile* input;
  std::string file_name;       /* kept for RgbaInputFile and OutputFile */

  /* Which channels become IM's planes. For a plain file the prefix is empty;
     for a layered one ("diffuse.R") it names the layer that was chosen. */
  std::string prefix;
  std::string gray_channel;    /* the channel read as gray, when is_gray */
  int is_gray;
  int has_alpha;
  int is_luma_chroma;          /* Y/RY/BY: read through RgbaInputFile */
  int channel_count;
  int write_half;

  int OpenFile(const char* name);
  int ChooseChannels(const Imf::ChannelList& channels);
  int ReadPixels(float* buffer);

public:
  imFileFormatEXR(const imFormat* _iformat)
    : imFileFormatBase(_iformat), input(NULL), is_gray(0), has_alpha(0),
      is_luma_chroma(0), channel_count(3), write_half(0) {}
  ~imFileFormatEXR() {}

  int Open(const char* file_name);
  int New(const char* file_name);
  void Close();
  void* Handle(int index);
  int ReadImageInfo(int index);
  int ReadImageData(void* data);
  int WriteImageInfo();
  int WriteImageData(void* data);
};

class imFormatEXR: public imFormat
{
public:
  imFormatEXR()
    :imFormat("EXR",
              "OpenEXR High Dynamic Range Image",
              "*.exr;",
              iEXRCompTable,
              10,
              0)
    { extra = "OpenEXR"; }
  ~imFormatEXR() {}

  imFileFormatBase* Create(void) const { return new imFileFormatEXR(this); }
  int CanWrite(const char* compression, int color_mode, int data_type) const;
};

void imFormatRegisterEXR(void)
{
  imFormatRegister(new imFormatEXR());
}


/*******************************************************************/


static const char* iEXRCompName(Imf::Compression compression)
{
  for (int i = 0; i < 10; i++)
    if (iEXRCompValue[i] == compression)
      return iEXRCompTable[i];

  /* A codec newer than this table (HTJ2K arrived in 3.4). It still reads --
     the library decodes it -- but there is no name here to report. */
  return "OTHER";
}

static int iEXRHas(const Imf::ChannelList& channels, const std::string& name)
{
  return channels.findChannel(name.c_str()) != NULL;
}

/* Picks the channels to read, preferring in turn:

     R G B [A]          an ordinary colour image
     Y RY BY [A]        luminance/chroma, which RgbaInputFile converts to RGB
     Y [A]              luminance only
     layer.R .G .B [A]  the first layer, in name order, holding a colour image
     a single channel   of any name, read as gray

   Anything else -- several unrelated channels, say, or a depth-only render
   pass -- is refused rather than guessed at. */
int imFileFormatEXR::ChooseChannels(const Imf::ChannelList& channels)
{
  this->prefix = "";
  this->gray_channel = "";
  this->is_luma_chroma = 0;

  if (iEXRHas(channels, "R") && iEXRHas(channels, "G") && iEXRHas(channels, "B"))
    this->is_gray = 0;
  else if (iEXRHas(channels, "Y") && iEXRHas(channels, "RY") && iEXRHas(channels, "BY"))
  {
    this->is_gray = 0;
    this->is_luma_chroma = 1;
  }
  else if (iEXRHas(channels, "Y"))
  {
    this->is_gray = 1;
    this->gray_channel = "Y";
  }
  else
  {
    int found = 0;
    for (Imf::ChannelList::ConstIterator i = channels.begin(); i != channels.end(); ++i)
    {
      std::string name = i.name();
      size_t length = name.size();
      if (length > 2 && name.compare(length - 2, 2, ".R") == 0)
      {
        std::string layer = name.substr(0, length - 1);   /* keeps the dot */
        if (iEXRHas(channels, layer + "G") && iEXRHas(channels, layer + "B"))
        {
          this->prefix = layer;
          this->is_gray = 0;
          found = 1;
          break;
        }
      }
    }

    if (!found)
    {
      Imf::ChannelList::ConstIterator first = channels.begin();
      if (first == channels.end())
        return IM_ERR_DATA;
      Imf::ChannelList::ConstIterator second = first;
      if (++second != channels.end())
        return IM_ERR_DATA;

      /* One channel, of whatever name: read it as gray, with no alpha --
         its name may well be "A", and then it is the image. */
      this->gray_channel = first.name();
      this->is_gray = 1;
    }
  }

  this->has_alpha = (this->gray_channel != this->prefix + "A") &&
                    iEXRHas(channels, this->prefix + "A");
  this->channel_count = (this->is_gray? 1: 3) + (this->has_alpha? 1: 0);

  /* Subsampled channels have no meaning in IM's one-sample-per-pixel planes,
     and only the luminance/chroma path knows how to fill them back in. */
  if (!this->is_luma_chroma)
  {
    std::vector<std::string> used;
    if (this->is_gray)
      used.push_back(this->gray_channel);
    else
    {
      used.push_back(this->prefix + "R");
      used.push_back(this->prefix + "G");
      used.push_back(this->prefix + "B");
    }
    if (this->has_alpha)
      used.push_back(this->prefix + "A");

    for (size_t u = 0; u < used.size(); u++)
    {
      const Imf::Channel* channel = channels.findChannel(used[u].c_str());
      if (channel->xSampling != 1 || channel->ySampling != 1)
        return IM_ERR_DATA;
    }
  }

  return IM_ERR_NONE;
}

int imFileFormatEXR::Open(const char* name)
{
  /* imFileOpen deletes a driver whose Open failed without calling Close, so
     every failure has to release what it got this far. */
  int error;
  try
  {
    error = OpenFile(name);
  }
  catch (const std::bad_alloc&)
  {
    error = IM_ERR_MEM;
  }
  catch (const std::exception&)
  {
    /* The magic number matched but the header did not parse. */
    error = IM_ERR_FORMAT;
  }

  if (error != IM_ERR_NONE)
    Close();
  return error;
}

int imFileFormatEXR::OpenFile(const char* name)
{
  unsigned char magic[4];
  FILE* file = fopen(name, "rb");
  if (!file)
    return IM_ERR_OPEN;

  /* Check the magic number before handing the file to OpenEXR: imFileOpen
     offers every file to every driver, and letting the library throw on each
     JPEG would be both slow and noisy under a debugger. */
  size_t got = fread(magic, 1, 4, file);
  fclose(file);
  if (got != 4 || magic[0] != 0x76 || magic[1] != 0x2F || magic[2] != 0x31 ||
      magic[3] != 0x01)
    return IM_ERR_FORMAT;

  this->file_name = name;
  this->input = new Imf::InputFile(name);

  /* Deep files hold a variable number of samples per pixel, which is not an
     image IM can represent. */
  if (this->input->header().hasType() &&
      (this->input->header().type() == "deepscanline" ||
       this->input->header().type() == "deeptile"))
    return IM_ERR_DATA;

  int error = ChooseChannels(this->input->header().channels());
  if (error != IM_ERR_NONE)
    return error;

  strcpy(this->compression, iEXRCompName(this->input->header().compression()));
  this->image_count = 1;

  return IM_ERR_NONE;
}

int imFileFormatEXR::New(const char* name)
{
  /* OutputFile writes the header on construction, which needs the channel
     list, which needs the image -- so nothing is created until
     WriteImageData. Check the path now, the way a stream driver would. */
  FILE* file = fopen(name, "wb");
  if (!file)
    return IM_ERR_OPEN;
  fclose(file);

  this->file_name = name;
  this->image_count = 1;
  return IM_ERR_NONE;
}

void imFileFormatEXR::Close()
{
  delete this->input;
  this->input = NULL;
}

void* imFileFormatEXR::Handle(int index)
{
  if (index == 0)
    return (void*)this->input;
  return NULL;
}

int imFileFormatEXR::ReadImageInfo(int index)
{
  if (index != 0 || !this->input)
    return IM_ERR_DATA;

  /* The data window, not the display window: it is the region that has
     pixels, and the two differ for crops and overscan renders. */
  const Imath::Box2i& window = this->input->header().dataWindow();
  long width = (long)window.max.x - (long)window.min.x + 1;
  long height = (long)window.max.y - (long)window.min.y + 1;
  if (width <= 0 || height <= 0 || width > 0x7FFFFFFF / 16 || height > 0x7FFFFFFF / 16)
    return IM_ERR_DATA;

  this->width = (int)width;
  this->height = (int)height;
  this->file_data_type = IM_FLOAT;

  this->file_color_mode = this->is_gray? IM_GRAY: IM_RGB;
  if (this->has_alpha)
    this->file_color_mode |= IM_ALPHA;
  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  return IM_ERR_NONE;
}

/* Reads the whole data window into an interleaved float buffer. */
int imFileFormatEXR::ReadPixels(float* buffer)
{
  const Imath::Box2i& window = this->input->header().dataWindow();
  size_t xstride = sizeof(float) * (size_t)this->channel_count;
  size_t ystride = xstride * (size_t)this->width;

  if (this->is_luma_chroma)
  {
    /* RgbaInputFile reconstructs RGB from luminance and subsampled chroma.
       It hands back half, which is all such a file holds anyway. */
    std::vector<Imf::Rgba> rgba((size_t)this->width * (size_t)this->height);

    Imf::RgbaInputFile file(this->file_name.c_str());
    file.setFrameBuffer(&rgba[0] - window.min.x - (long)window.min.y * this->width,
                        1, (size_t)this->width);
    file.readPixels(window.min.y, window.max.y);

    for (size_t i = 0; i < rgba.size(); i++)
    {
      float* pixel = buffer + i * (size_t)this->channel_count;
      pixel[0] = rgba[i].r;
      pixel[1] = rgba[i].g;
      pixel[2] = rgba[i].b;
      if (this->has_alpha)
        pixel[3] = rgba[i].a;
    }
    return IM_ERR_NONE;
  }

  Imf::FrameBuffer frame;
  char* base = (char*)buffer;

  if (this->is_gray)
    frame.insert(this->gray_channel.c_str(),
                 Imf::Slice::Make(Imf::FLOAT, base, window, xstride, ystride));
  else
  {
    const char* names[3] = { "R", "G", "B" };
    for (int c = 0; c < 3; c++)
      frame.insert((this->prefix + names[c]).c_str(),
                   Imf::Slice::Make(Imf::FLOAT, base + c * sizeof(float), window,
                                    xstride, ystride));
  }

  if (this->has_alpha)
    frame.insert((this->prefix + "A").c_str(),
                 Imf::Slice::Make(Imf::FLOAT,
                                  base + (this->channel_count - 1) * sizeof(float),
                                  window, xstride, ystride));

  this->input->setFrameBuffer(frame);
  this->input->readPixels(window.min.y, window.max.y);

  return IM_ERR_NONE;
}

int imFileFormatEXR::ReadImageData(void* data)
{
  if (!this->input)
    return IM_ERR_ACCESS;

  size_t line_floats = (size_t)this->width * (size_t)this->channel_count;
  float* buffer = (float*)malloc(line_floats * (size_t)this->height * sizeof(float));
  if (!buffer)
    return IM_ERR_MEM;

  int error;
  try
  {
    error = ReadPixels(buffer);
  }
  catch (const std::bad_alloc&)
  {
    error = IM_ERR_MEM;
  }
  catch (const std::exception&)
  {
    /* A truncated or corrupt file: OpenEXR found it out mid-image. */
    error = IM_ERR_ACCESS;
  }

  if (error != IM_ERR_NONE)
  {
    free(buffer);
    return error;
  }

  int count = imFileLineBufferCount(this);
  int lin = 0, plane = 0;
  imCounterTotal(this->counter, count, "Reading EXR...");

  for (int i = 0; i < count; i++)
  {
    memcpy(this->line_buffer, buffer + (size_t)lin * line_floats,
           line_floats * sizeof(float));

    imFileLineBufferRead(this, data, lin, plane);

    if (!imCounterInc(this->counter))
    {
      free(buffer);
      return IM_ERR_COUNTER;
    }

    imFileLineBufferInc(this, &lin, &plane);
  }

  free(buffer);
  return IM_ERR_NONE;
}

int imFileFormatEXR::WriteImageInfo()
{
  imAttribTable* attrib_table = AttribTable();
  int color_space = imColorModeSpace(this->user_color_mode);

  if (this->user_data_type != IM_FLOAT)
    return IM_ERR_DATA;
  this->file_data_type = IM_FLOAT;

  if (color_space == IM_GRAY)
    this->is_gray = 1;
  else if (color_space == IM_RGB)
    this->is_gray = 0;
  else
    return IM_ERR_DATA;

  this->has_alpha = imColorModeHasAlpha(this->user_color_mode);
  this->channel_count = (this->is_gray? 1: 3) + (this->has_alpha? 1: 0);

  this->file_color_mode = color_space;
  if (this->has_alpha)
    this->file_color_mode |= IM_ALPHA;
  this->file_color_mode |= IM_TOPDOWN;
  if (this->channel_count > 1)
    this->file_color_mode |= IM_PACKED;

  if (this->compression[0] == 0)
    strcpy(this->compression, IM_EXR_DEFAULT_COMPRESSION);
  else
  {
    int known = 0;
    for (int i = 0; i < 10; i++)
      if (imStrEqual(this->compression, iEXRCompTable[i]))
        known = 1;
    if (!known)
      return IM_ERR_COMPRESS;
  }

  int* half = (int*)attrib_table->Get("Half");
  this->write_half = (half && *half);

  return IM_ERR_NONE;
}

int imFileFormatEXR::WriteImageData(void* data)
{
  size_t line_floats = (size_t)this->width * (size_t)this->channel_count;
  float* buffer = (float*)malloc(line_floats * (size_t)this->height * sizeof(float));
  if (!buffer)
    return IM_ERR_MEM;

  int count = imFileLineBufferCount(this);
  int lin = 0, plane = 0;
  imCounterTotal(this->counter, count, "Writing EXR...");

  for (int i = 0; i < count; i++)
  {
    imFileLineBufferWrite(this, data, lin, plane);
    memcpy(buffer + (size_t)lin * line_floats, this->line_buffer,
           line_floats * sizeof(float));

    if (!imCounterInc(this->counter))
    {
      free(buffer);
      return IM_ERR_COUNTER;
    }

    imFileLineBufferInc(this, &lin, &plane);
  }

  Imf::Compression compression = Imf::ZIP_COMPRESSION;
  for (int i = 0; i < 10; i++)
    if (imStrEqual(this->compression, iEXRCompTable[i]))
      compression = iEXRCompValue[i];

  int error = IM_ERR_NONE;
  try
  {
    Imf::PixelType type = this->write_half? Imf::HALF: Imf::FLOAT;
    Imf::Header header(this->width, this->height);
    header.compression() = compression;

    const char* names[4];
    int n = 0;
    if (this->is_gray)
      names[n++] = "Y";
    else
    {
      names[n++] = "R";
      names[n++] = "G";
      names[n++] = "B";
    }
    if (this->has_alpha)
      names[n++] = "A";

    for (int c = 0; c < n; c++)
      header.channels().insert(names[c], Imf::Channel(type));

    /* OpenEXR converts between pixel types when reading but not when
       writing -- a FLOAT slice for a HALF channel throws "not compatible with
       the frame buffer's pixel type" -- so half is converted here. half's
       float constructor rounds to nearest. */
    std::vector<half> halves;
    Imf::FrameBuffer frame;
    char* base = (char*)buffer;
    size_t sample = sizeof(float);

    if (this->write_half)
    {
      size_t total = line_floats * (size_t)this->height;
      halves.resize(total);
      for (size_t i = 0; i < total; i++)
        halves[i] = half(buffer[i]);
      base = (char*)&halves[0];
      sample = sizeof(half);
    }

    size_t xstride = sample * (size_t)this->channel_count;
    size_t ystride = xstride * (size_t)this->width;
    for (int c = 0; c < n; c++)
      frame.insert(names[c], Imf::Slice(type, base + c * sample, xstride, ystride));

    Imf::OutputFile file(this->file_name.c_str(), header);
    file.setFrameBuffer(frame);
    file.writePixels(this->height);
  }
  catch (const std::bad_alloc&)
  {
    error = IM_ERR_MEM;
  }
  catch (const std::exception&)
  {
    error = IM_ERR_ACCESS;
  }

  free(buffer);
  return error;
}

int imFormatEXR::CanWrite(const char* compression, int color_mode, int data_type) const
{
  int color_space = imColorModeSpace(color_mode);

  if (color_space != IM_RGB && color_space != IM_GRAY)
    return IM_ERR_DATA;

  if (data_type != IM_FLOAT)
    return IM_ERR_DATA;

  if (!compression || compression[0] == 0)
    return IM_ERR_NONE;

  for (int i = 0; i < 10; i++)
    if (imStrEqual(compression, iEXRCompTable[i]))
      return IM_ERR_NONE;

  return IM_ERR_COMPRESS;
}
