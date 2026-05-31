#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)((v >> 8) & 0xffU);
    p[1] = (unsigned char)(v & 0xffU);
}

int main(int argc, char **argv)
{
    unsigned char *buf;
    unsigned long size;
    unsigned long pos;
    unsigned int x;
    unsigned int y;
    unsigned short r;
    unsigned short g;
    unsigned short b;
    unsigned short pix;
    FILE *f;
    const unsigned int w = 96U;
    const unsigned int h = 64U;
    if (argc < 2) {
        fprintf(stderr, "usage: mk_blit_demo out.bin\n");
        return 2;
    }
    size = 1024UL * 1024UL;
    buf = (unsigned char *)malloc((size_t)size);
    if (buf == NULL) return 2;
    memset(buf, 0, (size_t)size);
    pos = 0UL;

    /* Upload a gradient tile to CV1000 VRAM at 32,32. */
    put16(&buf[pos + 0UL], 0x2000U);
    put16(&buf[pos + 2UL], 0x0000U);
    put16(&buf[pos + 4UL], 0x9999U);
    put16(&buf[pos + 6UL], 0x9999U);
    put16(&buf[pos + 8UL], 32U);
    put16(&buf[pos + 10UL], 32U);
    put16(&buf[pos + 12UL], (unsigned short)(w - 1U));
    put16(&buf[pos + 14UL], (unsigned short)(h - 1U));
    pos += 16UL;
    for (y = 0U; y < h; y++) {
        for (x = 0U; x < w; x++) {
            r = (unsigned short)((x * 31U) / (w - 1U));
            g = (unsigned short)((y * 31U) / (h - 1U));
            b = (unsigned short)(((x ^ y) * 31U) / 127U);
            pix = (unsigned short)(0x8000U | (r << 10) | (g << 5) | b);
            put16(&buf[pos], pix);
            pos += 2UL;
        }
    }

    /* Draw it again offset, alpha/additive enabled, with mild tint. */
    put16(&buf[pos + 0UL], 0x1300U);  /* draw + blend + transparent */
    put16(&buf[pos + 2UL], 0x7000U);  /* source alpha */
    put16(&buf[pos + 4UL], 32U);
    put16(&buf[pos + 6UL], 32U);
    put16(&buf[pos + 8UL], 152U);
    put16(&buf[pos + 10UL], 80U);
    put16(&buf[pos + 12UL], (unsigned short)(w - 1U));
    put16(&buf[pos + 14UL], (unsigned short)(h - 1U));
    put16(&buf[pos + 16UL], 0x0020U);
    put16(&buf[pos + 18UL], 0x2020U);
    pos += 20UL;

    put16(&buf[pos], 0x0000U);

    f = fopen(argv[1], "wb");
    if (f == NULL) return 2;
    fwrite(buf, 1U, (size_t)size, f);
    fclose(f);
    free(buf);
    return 0;
}
