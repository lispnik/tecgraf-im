#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <im.h>
#include <im_image.h>
#include <im_process.h>
#include <im_util.h>

void print_usage(const char* program_name) {
    printf("Usage: %s <input_image> <output_image>\n", program_name);
    printf("Enhances a document image by removing noise and improving contrast.\n");
    printf("Example: %s messy_document.jpg clean_document.png\n", program_name);
}

void enhance_document_scan(const char* input_path, const char* output_path) {
    printf("Starting document enhancement pipeline...\n");

    // 1. Load the original image
    int error;
    imFile* ifile = imFileOpen(input_path, &error);
    if (!ifile) {
        printf("Error: Could not open input file '%s' (error code: %d)\n", input_path, error);
        return;
    }

    int width, height, color_mode, data_type;
    error = imFileReadImageInfo(ifile, 0, &width, &height, &color_mode, &data_type);
    if (error != IM_ERR_NONE) {
        printf("Error: Could not read image info (error code: %d)\n", error);
        imFileClose(ifile);
        return;
    }

    imImage* original = imImageCreate(width, height, color_mode, data_type);
    if (!original) {
        printf("Error: Could not create image\n");
        imFileClose(ifile);
        return;
    }

    error = imFileReadImageData(ifile, original->data[0], 1, 0);
    imFileClose(ifile);

    if (error != IM_ERR_NONE) {
        printf("Error: Could not read image data (error code: %d)\n", error);
        imImageDestroy(original);
        return;
    }

    printf("✓ Loaded image: %dx%d, color_mode=%d, data_type=%d\n",
           width, height, color_mode, data_type);

    // 2. Convert to grayscale if needed
    imImage* gray;
    if (imColorModeSpace(color_mode) == IM_GRAY) {
        gray = imImageClone(original);
        printf("✓ Image already in grayscale\n");
    } else {
        gray = imImageCreateBased(original, -1, -1, IM_GRAY, -1);
        if (!gray) {
            printf("Error: Could not create grayscale image\n");
            imImageDestroy(original);
            return;
        }
        imProcessConvertColorSpace(original, gray);
        printf("✓ Converted to grayscale\n");
    }

    // 3. Simple enhancement: normalize components
    imImage* enhanced = imImageClone(gray);
    if (!enhanced) {
        printf("Error: Could not create enhanced image\n");
        imImageDestroy(original);
        imImageDestroy(gray);
        return;
    }

    imProcessNormalizeComponents(gray, enhanced);
    printf("✓ Applied normalization for contrast enhancement\n");

    // 4. Save the enhanced image
    imFile* ofile = imFileNew(output_path, "PNG", &error);
    if (!ofile) {
        printf("Error: Could not create output file '%s' (error code: %d)\n", output_path, error);
        goto cleanup;
    }

    error = imFileWriteImageInfo(ofile, enhanced->width, enhanced->height,
                                 enhanced->color_space, enhanced->data_type);
    if (error != IM_ERR_NONE) {
        printf("Error: Could not write image info (error code: %d)\n", error);
        imFileClose(ofile);
        goto cleanup;
    }

    error = imFileWriteImageData(ofile, enhanced->data[0]);
    imFileClose(ofile);

    if (error != IM_ERR_NONE) {
        printf("Error: Could not write image data (error code: %d)\n", error);
    } else {
        printf("✓ Saved enhanced image to '%s'\n", output_path);
    }

    // Print statistics
    printf("\nImage Enhancement Summary:\n");
    printf("- Input: %s (%dx%d)\n", input_path, width, height);
    printf("- Output: %s (PNG format)\n", output_path);
    printf("- Processing: Grayscale conversion → Normalization\n");

cleanup:
    // Cleanup
    imImageDestroy(original);
    imImageDestroy(gray);
    imImageDestroy(enhanced);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    printf("IM Document Enhancement Tool\n");
    printf("============================\n\n");

    // Process the document
    enhance_document_scan(argv[1], argv[2]);

    return 0;
}