/** \file
 * \brief Image Processing - Global Operations
 *
 * See Copyright Notice in im_lib.h
 */

#ifndef __IM_PROCESS_GLO_H
#define __IM_PROCESS_GLO_H

#include "im_image.h"

#if	defined(__cplusplus)
extern "C" {
#endif



/** \defgroup transform Other Domain Transform Operations
 * \par
 * Hough, Distance.
 *
 * See \ref im_process_glo.h
 * \ingroup process */

/** Hough Lines Transform. \n
 * It will detect white lines in a black background. So the source image must be a IM_BINARY image 
 * with the white lines of interest enhanced. The better the threshold with the white lines the better 
 * the line detection. \n
 * The target image must have IM_GRAY, IM_INT, hg_width=180, hg_height=2*rmax+1, 
 * where rmax is the image diagonal/2 (rmax = srqrt(width*width + height*height)). \n
 * The hough transform defines  "cos(theta) * X  + sin(theta) * Y = rho" and the parameters are in the interval: \n
 * theta = "0 .. 179", rho = "-hg_height/2 .. hg_height/2" .\n
 * Where rho is the perpendicular distance from the center of the image and theta the angle with the normal.
 * So do not confuse theta with the line angle, they are perpendicular. \n
 * Returns zero if the counter aborted. \n
 * Inspired from ideas in XITE, Copyright 1991, Blab, UiO \n
 * http://www.ifi.uio.no/~blab/Software/Xite/  \n
 * Not using OpenMP when enabled.
 *
 * \verbatim im.ProcessHoughLines(src_image: imImage, dst_image: imImage) -> counter: boolean [in Lua 5] \endverbatim
 * \verbatim im.ProcessHoughLinesNew(image: imImage) -> counter: boolean, new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
int imProcessHoughLines(const imImage* src_image, imImage* dst_image);

/** Draw detected hough lines. \n
 * The source and target images can be IM_MAP, IM_GRAY or IM_RGB, with data type IM_BYTE. \n
 * Can be done in-place. \n
 * If the hough transform is not NULL, then the hough points are filtered to include only lines
 * that are significally different from each other. \n
 * The hough image is the hough transform image, but it is optional and can be NULL. 
 * If not NULL then it will be used to filter lines that are very similar. \n
 * The hough points image is a hough transform image that was thresholded to a IM_BINARY image, 
 * usually using a Local Max threshold operation (see \ref imProcessLocalMaxThreshold). Again the better the threshold the better the results. \n
 * The detected lines will be drawn using a red color.
 * If the target image is IM_GRAY, it will be changed to IM_MAP. \n
 * If the target image is IM_RGB, then only the red plane will be changed.
 * Returns the number of detected lines. \n
 * Not using OpenMP when enabled.
 *
 * \verbatim im.ProcessHoughLinesDraw(src_image: imImage, hough: imImage, hough_points: imImage, dst_image: imImage) -> lines: number [in Lua 5] \endverbatim
 * \verbatim im.ProcessHoughLinesDrawNew(image: imImage, hough: imImage, hough_points: imImage) -> lines: number, new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
int imProcessHoughLinesDraw(const imImage* src_image, const imImage* hough, const imImage* hough_points, imImage* dst_image);

/** Calculates the Cross Correlation in the frequency domain. \n 
 * CrossCorr(a,b) = IFFT(Conj(FFT(a))*FFT(b)) \n
 * Images must be of the same size and only target image must be of type complex.
 *
 * \verbatim im.ProcessCrossCorrelation(src_image1: imImage, src_image2: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessCrossCorrelationNew(image1: imImage, image2: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
void imProcessCrossCorrelation(const imImage* src_image1, const imImage* src_image2, imImage* dst_image);

/** Calculates the Auto Correlation in the frequency domain. \n 
 * Uses the cross correlation.
 * Images must be of the same size and only target image must be of type complex.
 *
 * \verbatim im.ProcessAutoCorrelation(src_image: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessAutoCorrelationNew(image: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
void imProcessAutoCorrelation(const imImage* src_image, imImage* dst_image);

/** Calculates the Distance Transform of a binary image 
 * using an aproximation of the euclidian distance.\n
 * Each white pixel in the binary image is
 * assigned a value equal to its distance from the nearest
 * black pixel. \n
 * Uses a two-pass algorithm incrementally calculating the distance. \n
 * Source image must be IM_BINARY, target must be IM_FLOAT or IM_DOUBLE.
 *
 * \verbatim im.ProcessDistanceTransform(src_image: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessDistanceTransformNew(image: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
void imProcessDistanceTransform(const imImage* src_image, imImage* dst_image);

/** Marks all the regional maximum of the distance transform. \n
 * source must be IM_GRAY+IM_FLOAT/IM_DOUBLE, target must be IM_BINARY. \n
 * We consider maximum all connected pixel values that have smaller pixel values around it.
 *
 * \verbatim im.ProcessRegionalMaximum(src_image: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessRegionalMaximumNew(image: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup transform */
void imProcessRegionalMaximum(const imImage* src_image, imImage* dst_image);

/** \defgroup watershed Watershed Segmentation
 * \par
 * Splitting an image into catchment basins by flooding it from given markers.
 * \par
 * See \ref im_process_glo.h
 * \ingroup process */

/** Marker-controlled watershed, by Meyer's flooding algorithm. \n
 * Reads src_image as a relief map and floods it from the labelled markers,
 * lowest ground first, so every pixel joins the marker whose water reached it.
 * All three images must be of the same size and one plane. \n
 * src_image is IM_GRAY of any real data type -- LOW values are flooded first,
 * so basins must be the features of interest; negate the image if they are
 * not. marker_image and dst_image are IM_GRAY/IM_USHORT, marker_image labelled
 * as \ref imAnalyzeFindRegions labels, with 0 meaning unmarked. dst_image may
 * be the same image as marker_image. \n
 * connect is 4 or 8. When mark_lines is non-zero, a pixel that two different
 * basins reach at once is left as 0 in dst_image and belongs to neither, which
 * draws one-pixel watershed lines between the regions; when it is zero, every
 * pixel is assigned and the regions meet directly. \n
 * A region seeded by no marker is never labelled: this segments the markers
 * given, it does not find them. \ref imProcessWatershedSegment is the usual
 * way to obtain markers.
 * Not using OpenMP when enabled -- the flood is inherently sequential.
 * Returns zero if the counter aborted.
 *
 * \verbatim im.ProcessWatershed(src_image: imImage, marker_image: imImage, dst_image: imImage, connect: number, mark_lines: boolean) -> counter: boolean [in Lua 5] \endverbatim
 * \verbatim im.ProcessWatershedNew(image: imImage, marker_image: imImage, connect: number, mark_lines: boolean) -> counter: boolean, new_image: imImage [in Lua 5] \endverbatim
 * \ingroup watershed */
int imProcessWatershed(const imImage* src_image, const imImage* marker_image, imImage* dst_image, int connect, int mark_lines);

/** Separates touching objects in a binary image, and labels them. \n
 * src_image is IM_BINARY, dst_image is IM_GRAY/IM_USHORT and receives one
 * label per object, exactly as \ref imAnalyzeFindRegions would -- so every
 * imAnalyzeMeasure* function reads the result directly. Both must be the same
 * size. region_count returns the number of objects found. \n
 * This is what \ref imAnalyzeFindRegions cannot do: two objects that touch are
 * one connected region, and no amount of labelling will make them two. The
 * recipe is \ref imProcessDistanceTransform to find how deep inside an object
 * each pixel is, \ref imProcessRegionalMaximum to find the object centres, and
 * a watershed of the negated distance map seeded from those centres, which
 * splits the pair along the neck between them. \n
 * connect is 4 or 8, and applies both to grouping the markers and to the
 * flood. mark_lines leaves a one-pixel gap of 0 between objects when non-zero.
 * Objects touching the border are included. \n
 * Convex objects of similar size separate cleanly; a strongly concave object
 * can carry more than one distance maximum and be split in two, which is this
 * method's characteristic failure and is not detectable from the output.
 * Returns zero if the counter aborted.
 *
 * \verbatim im.ProcessWatershedSegment(src_image: imImage, dst_image: imImage, connect: number, mark_lines: boolean) -> counter: boolean, region_count: number [in Lua 5] \endverbatim
 * \verbatim im.ProcessWatershedSegmentNew(image: imImage, connect: number, mark_lines: boolean) -> counter: boolean, region_count: number, new_image: imImage [in Lua 5] \endverbatim
 * \ingroup watershed */
int imProcessWatershedSegment(const imImage* src_image, imImage* dst_image, int connect, int mark_lines, int* region_count);



/** \defgroup fourier Fourier Transform Operations
 * \par
 * All Fourier transforms use FFTW 3.x library. \n
 * The pre-compiled binaries depend on an external library. For Windows the FFTW DLLs are included in the package. For Linux you must use the libfftw3 included with your sysytem.
 * \par
 * FFTW Copyright Matteo Frigo, Steven G. Johnson and the MIT. \n
 * http://www.fftw.org                                         \n
 * See "fftw.h"
 * \par
 * Must link with "im_fftw3" library. \n
 * \par
 * \b IMPORTANT: The FFTW lib has a GPL license. The license of the "im_fftw3" library is automatically the GPL.
 * So you cannot use it for commercial applications without contacting the authors. 
 * \par
 * FFTW 3.x have float and double support.
 * \par
 * See \ref im_process_glo.h
 * \ingroup process */

/** Forward FFT. \n
 * The result has its lowest frequency at the center of the image. \n
 * This is an unnormalized fft. \n
 * Images must be of the same size. Target image must be of type complex.
 *
 * \verbatim im.ProcessFFT(src_image: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessFFTNew(image: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup fourier */
void imProcessFFT(const imImage* src_image, imImage* dst_image);

/** Inverse FFT. \n
 * The image has its lowest frequency restored to the origin before the transform. \n
 * The result is normalized by (width*height). \n
 * Images must be of the same size and both must be of type complex.
 *
 * \verbatim im.ProcessIFFT(src_image: imImage, dst_image: imImage) [in Lua 5] \endverbatim
 * \verbatim im.ProcessIFFTNew(image: imImage) -> new_image: imImage [in Lua 5] \endverbatim
 * \ingroup fourier */
void imProcessIFFT(const imImage* src_image, imImage* dst_image);

/** Raw in-place FFT (forward or inverse). \n
 * The lowest frequency can be centered after forward, or
 * can be restored to the origin before inverse. \n
 * The result can be normalized after the transform by sqrt(w*h) [1] or by (w*h) [2], 
 * or left unnormalized [0]. \n
 * Images must be of the same size and both must be of type complex.
 *
 * \verbatim im.ProcessFFTraw(image: imImage, inverse: number, center: number, normalize: number) [in Lua 5] \endverbatim
 * \ingroup fourier */
void imProcessFFTraw(imImage* image, int inverse, int center, int normalize);

/** Auxiliary function for the raw FFT. \n 
 * This is the function used internally to change the lowest frequency position in the image. \n
 * If the image size has even dimensions the flag "center2origin" is useless. But if it is odd, 
 * you must specify if its from center to origin (usually used before inverse) or
 * from origin to center (usually used after forward). \n
 * Notice that this function is used for images in the the frequency domain. \n
 * Image type must be complex.
 *
 * \verbatim im.ProcessSwapQuadrants(image: imImage, center2origin: number) [in Lua 5] \endverbatim
 * \ingroup fourier */
void imProcessSwapQuadrants(imImage* image, int center2origin);



/** \defgroup openmp OpenMP Utilities
 * \par
 * Used inside im_process_omp only. But also exported to Lua.
 * These functions do not use OpenMP, 
 * they are used when OpenMP is enabled in im_process.
 * See \ref im_util.h
 * \ingroup process */

/** Sets the minimum number of iterations to split into threads. \n
 * Default value is 250000, or an image with 500x500. \n
 * Returns the previous value.
 *
 * \verbatim im.ProcessOpenMPSetMinCount(min_count: number) -> old_min_count: number [in Lua 5] \endverbatim
 * \ingroup openmp */
int imProcessOpenMPSetMinCount(int min_count);

/** Sets the number of threads. \n
 * Does nothing if OpenMP is not enabled. \n
 * Returns the previous value.
 *
 * \verbatim im.ProcessOpenMPSetNumThreads(min_count: number) -> old_min_count: number [in Lua 5] \endverbatim
 * \ingroup openmp */
int imProcessOpenMPSetNumThreads(int count);


#if defined(__cplusplus)
}
#endif

#endif
