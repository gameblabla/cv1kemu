#include <stdio.h>

static void w16(FILE *f, unsigned int v)
{
    fputc((int)((v >> 8) & 255U), f);
    fputc((int)(v & 255U), f);
}

int main(int argc, char **argv)
{
    FILE *f;
    unsigned long i;
    const char *path;
    path = (argc > 1) ? argv[1] : "dummy_u4.bin";
    f = fopen(path, "wb");
    if (f == NULL) return 1;
    /* Minimal SH-style stream: mov #1,r0; add #1,r0; bra -2; nop. */
    w16(f, 0xe001U);
    w16(f, 0x7001U);
    w16(f, 0xaffdU);
    w16(f, 0x0009U);
    for (i = 8UL; i < 4096UL; i++) fputc(0xff, f);
    fclose(f);
    return 0;
}
