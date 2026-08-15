# IM - Image Processing Library

A comprehensive toolkit for Digital Imaging providing simple APIs and abstractions for scientific applications.

[![Linux CI](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-linux.yml)
[![macOS CI](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-macos.yml)
[![Windows CI](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-windows.yml)
[![Sanitizers](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-sanitizers.yml/badge.svg)](https://github.com/lispnik/tecgraf-im/actions/workflows/ci-sanitizers.yml)

## About

IM is based on 4 core concepts: **Image Representation**, **Storage**, **Processing**, and **Capture**. The library provides:

- Support for the most popular file formats: TIFF, BMP, PNG, JPEG, GIF, AVI, and more
- Scientific data type representation for images
- Over 100 image processing operations
- Simple, unified API across all platforms
- Integration with the [CD (Canvas Draw) library](https://github.com/lispnik/tecgraf-cd)

## Fork Differences from Upstream

This fork modernizes the original [Tecgraf IM library](http://www.tecgraf.puc-rio.br/im) with significant improvements:

### Build System Modernization
- **CMake build system** - Replaced legacy "tecmake" with modern CMake (3.20+)
- **System dependencies** - Removed bundled third-party libraries, uses system packages instead
- **Cross-platform builds** - Unified build process across Linux, macOS, and Windows

### Platform Support Enhancements
- **Apple Silicon macOS** - Native support for M1/M2 Macs with Homebrew integration
- **Proper install names** - macOS libraries built with absolute paths, no post-build fixup required
- **OpenMP support** - Enhanced parallel processing capabilities on all platforms

### Development Infrastructure
- **GitHub Actions CI** - Comprehensive continuous integration for all supported platforms
- **Automated testing** - Build verification and smoke tests across environments
- **Modern packaging** - Standardized library packaging and distribution

### Compatibility Improvements
- **Runtime library loading** - Libraries loadable from any runtime (CFFI, ctypes, etc.)
- **Dependency management** - Clean separation of required vs. optional dependencies
- **Cross-platform consistency** - Unified behavior across different operating systems

## Features

- **Image Formats**: TIFF, BMP, PNG, JPEG, GIF, AVI, PCD, PCX, TGA, RAS, SGI, and more,
  plus optional HEIC/AVIF via libheif (`-DIM_BUILD_HEIF=ON`, see BUILDING.md)
- **Data Types**: Support for scientific data types (int, float, complex)
- **Processing Operations**: 100+ image processing functions including:
  - Arithmetic operations
  - Geometric transformations
  - Morphological operations
  - Color space conversions
  - Fourier transforms (via FFTW)
  - Statistical analysis
- **Capture**: Video capture and image acquisition capabilities
- **Language Bindings**: C/C++ API with Lua bindings

## Building

See [BUILDING.md](BUILDING.md) for detailed build instructions.

### Quick Start

**macOS (Homebrew):**
```bash
brew install cmake ninja pkg-config libtiff jpeg-turbo libpng libexif liblzf lz4 jasper fftw libomp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install cmake ninja-build pkg-config libtiff-dev libjpeg-dev libpng-dev libexif-dev liblzf-dev liblz4-dev libfftw3-dev libomp-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DIM_BUILD_JP2=OFF
cmake --build build -j
```

**Windows:**
```bash
# Install dependencies via vcpkg
vcpkg install tiff libjpeg-turbo libpng libexif lz4 fftw3

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Usage Example

```c
#include <im.h>
#include <im_image.h>
#include <im_process.h>

int main() {
    // Initialize IM
    imFormatRegisterPNG();
    imFormatRegisterJPEG();
    
    // Load an image
    int error;
    imImage* image = imFileLoadImage("input.png", 0, &error);
    if (!image) return -1;
    
    // Create a destination image
    imImage* dest = imImageCreateBased(image, -1, -1, -1, -1);
    
    // Apply Gaussian blur
    imProcessGaussianConvolve(image, dest, 3.0);
    
    // Save result
    imFileSaveImage("output.png", "PNG", dest);
    
    // Cleanup
    imImageDestroy(image);
    imImageDestroy(dest);
    
    return 0;
}
```

## Integration with CD Library

IM seamlessly integrates with the [CD graphics library](https://github.com/lispnik/tecgraf-cd):

```c
#include <cd.h>
#include <im.h>

// Load image with IM
imImage* image = imFileLoadImage("photo.jpg", 0, NULL);

// Create CD canvas from IM image  
cdCanvas* canvas = cdCreateCanvas(cdContextImage(), image);

// Draw graphics on the image using CD
cdCanvasForeground(canvas, CD_RED);
cdCanvasText(canvas, 50, 50, "Enhanced Image");

// Save modified image
imFileSaveImage("enhanced.jpg", "JPEG", image);

cdKillCanvas(canvas);
imImageDestroy(image);
```

## Documentation

- **Original Documentation**: [Tecgraf IM Documentation](http://www.tecgraf.puc-rio.br/im)
- **API Reference**: Available in the `html/` directory
- **Build Instructions**: [BUILDING.md](BUILDING.md)
- **Examples**: See `test/` directory

## Original Authors

- **Tecgraf/PUC-Rio** - Computer Graphics Technology Group, Pontifical Catholic University of Rio de Janeiro
- **Website**: http://www.tecgraf.puc-rio.br/im
- **Contact**: im@tecgraf.puc-rio.br

## Fork Maintainer

This modernized fork is maintained as part of the `lispnik` GitHub organization, focusing on improved cross-platform support and modern development practices.

## License

See the original [COPYRIGHT](COPYRIGHT) file for license information.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Make your changes
4. Ensure all CI checks pass
5. Submit a pull request

## Dependencies

### Required
- CMake 3.20 or later
- C99-compatible compiler
- pkg-config

### Image Format Support
- **TIFF**: libtiff
- **JPEG**: libjpeg or libjpeg-turbo  
- **PNG**: libpng + zlib
- **EXIF**: libexif

### Optional Enhanced Features
- **JPEG 2000**: Jasper library (may be disabled with `-DIM_BUILD_JP2=OFF`)
- **HEIC / AVIF**: libheif 1.17+ (opt in with `-DIM_BUILD_HEIF=ON`; note the
  x265/GPL caveat in [BUILDING.md](BUILDING.md))
- **Compression**: liblzf, lz4
- **Fourier Transforms**: FFTW3
- **Parallel Processing**: OpenMP