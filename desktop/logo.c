/* The system mark: BitCat, an 8bpp indexed sprite.
 *
 * The art lives in bitcat.h, generated from BitCat.png by
 * scripts/genbitcat.py -- 32x32, one byte per pixel into a six-colour
 * palette, disc and rim and ink all baked in.  logo_paint() scales it to
 * whatever radius the caller gives (nearest neighbour, a divide per
 * pixel) and writes solid palette colours straight through gfx_px():
 * no blending, no antialiasing at draw time, a couple of hundred stores
 * for the whole mark.  A window painted every frame -- the About banner,
 * the only place the mark shows -- costs the same as a line of text. */
#include "desktop.h"
#include "bitcat.h"

void logo_paint(int cx, int cy, int r)
{
    if (r <= 0) return;

    int d = 2 * r;
    int x0 = cx - d / 2, y0 = cy - d / 2;

    for (int j = 0; j < d; j++) {
        int sy = j * BITCAT_H / d;
        const unsigned char *row = bitcat_px + (size_t)sy * BITCAT_W;
        for (int i = 0; i < d; i++) {
            unsigned char idx = row[i * BITCAT_W / d];
            if (idx) gfx_px(x0 + i, y0 + j, bitcat_pal[idx]);
        }
    }
}
