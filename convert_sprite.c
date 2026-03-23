/*
 * convert_sprite.c - Reads a BMP sprite sheet and outputs sprite_data.h
 *
 * Usage: gcc -o convert_sprite convert_sprite.c && ./convert_sprite spritemap_smol.bmp 9
 *
 * Reads a horizontal sprite sheet BMP (24bpp, uncompressed).
 * Outputs sprite_data.h with pixel data as RGB bytes.
 * Magenta (#FF00FF) pixels are stored as the transparent marker.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <spritesheet.bmp> <num_frames>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int num_frames = atoi(argv[2]);

    FILE *f = fopen(filename, "rb");
    if (!f) { perror("fopen"); return 1; }

    // Read BMP header
    uint8_t header[54];
    fread(header, 1, 54, f);

    int width = *(int32_t *)(header + 18);
    int height = *(int32_t *)(header + 22);
    int bpp = *(int16_t *)(header + 28);
    int data_offset = *(int32_t *)(header + 10);

    printf("// BMP: %dx%d, %dbpp, %d frames\n", width, height, bpp, num_frames);

    if (bpp != 24) {
        fprintf(stderr, "Only 24bpp BMP supported, got %d\n", bpp);
        fclose(f);
        return 1;
    }

    int frame_w = width / num_frames;
    int frame_h = height;

    // BMP rows are padded to 4-byte boundaries
    int row_bytes = width * 3;
    int row_padded = (row_bytes + 3) & ~3;

    // Read all pixel data
    fseek(f, data_offset, SEEK_SET);
    uint8_t *pixels = malloc(row_padded * height);
    fread(pixels, 1, row_padded * height, f);
    fclose(f);

    // Output header file
    FILE *out = fopen("sprite_data.h", "w");
    if (!out) { perror("fopen output"); return 1; }

    fprintf(out, "/* Auto-generated sprite data — do not edit */\n");
    fprintf(out, "#ifndef SPRITE_DATA_H\n");
    fprintf(out, "#define SPRITE_DATA_H\n\n");
    fprintf(out, "#define SPRITE_W %d\n", frame_w);
    fprintf(out, "#define SPRITE_H %d\n", frame_h);
    fprintf(out, "#define SPRITE_FRAMES %d\n", num_frames);
    fprintf(out, "#define SPRITE_TRANSPARENT_R 0\n");
    fprintf(out, "#define SPRITE_TRANSPARENT_G 255\n");
    fprintf(out, "#define SPRITE_TRANSPARENT_B 0\n\n");
    fprintf(out, "/* RGB pixel data: sprite_pixels[frame][row][col][3] */\n");
    fprintf(out, "static const uint8_t sprite_pixels[%d][%d][%d][3] = {\n", num_frames, frame_h, frame_w);

    for (int frame = 0; frame < num_frames; frame++) {
        fprintf(out, "  { /* frame %d */\n", frame);
        // BMP is stored bottom-up, so flip vertically
        for (int y = 0; y < frame_h; y++) {
            int bmp_y = frame_h - 1 - y;  // flip
            fprintf(out, "    {");
            for (int x = 0; x < frame_w; x++) {
                int px = frame * frame_w + x;
                int offset = bmp_y * row_padded + px * 3;
                uint8_t b = pixels[offset + 0];
                uint8_t g = pixels[offset + 1];
                uint8_t r = pixels[offset + 2];
                fprintf(out, "{%d,%d,%d}", r, g, b);
                if (x < frame_w - 1) fprintf(out, ",");
            }
            fprintf(out, "}");
            if (y < frame_h - 1) fprintf(out, ",");
            fprintf(out, "\n");
        }
        fprintf(out, "  }");
        if (frame < num_frames - 1) fprintf(out, ",");
        fprintf(out, "\n");
    }

    fprintf(out, "};\n\n");
    fprintf(out, "#endif /* SPRITE_DATA_H */\n");
    fclose(out);
    free(pixels);

    printf("Wrote sprite_data.h: %d frames, %dx%d each\n", num_frames, frame_w, frame_h);
    return 0;
}
