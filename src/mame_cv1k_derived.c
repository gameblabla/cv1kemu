#include "mame_cv1k_derived.h"

static cv1k_u8 colrtable[0x20][0x40];
static cv1k_u8 colrtable_rev[0x20][0x40];
static cv1k_u8 colrtable_add[0x20][0x20];
static int tables_ready = 0;

static cv1k_u8 clamp5(cv1k_u32 v)
{
    return (cv1k_u8)((v > 0x1fUL) ? 0x1fUL : v);
}

void cv1k_mame_build_color_tables(void)
{
    int x;
    int y;
    for (y = 0; y < 0x40; y++) {
        for (x = 0; x < 0x20; x++) {
            colrtable[x][y] = clamp5(((cv1k_u32)x * (cv1k_u32)y) / 0x1fUL);
            colrtable_rev[x ^ 0x1f][y] = clamp5(((cv1k_u32)x * (cv1k_u32)y) / 0x1fUL);
        }
    }
    for (y = 0; y < 0x20; y++) {
        for (x = 0; x < 0x20; x++) {
            colrtable_add[x][y] = clamp5((cv1k_u32)x + (cv1k_u32)y);
        }
    }
    tables_ready = 1;
}

cv1k_u8 cv1k_mame_mul5(cv1k_u8 x, cv1k_u8 y)
{
    if (!tables_ready) cv1k_mame_build_color_tables();
    return colrtable[x & 0x1fU][y & 0x3fU];
}

cv1k_u8 cv1k_mame_mul5_rev(cv1k_u8 x, cv1k_u8 y)
{
    if (!tables_ready) cv1k_mame_build_color_tables();
    return colrtable_rev[x & 0x1fU][y & 0x3fU];
}

cv1k_u8 cv1k_mame_add5(cv1k_u8 x, cv1k_u8 y)
{
    if (!tables_ready) cv1k_mame_build_color_tables();
    return colrtable_add[x & 0x1fU][y & 0x1fU];
}
