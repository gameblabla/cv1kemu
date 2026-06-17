#include "romset.h"
#include "platform.h"
#include "video.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CV1K_WITH_ZLIB
#include <zlib.h>
#endif

#define ZIP_EOCD_SIG 0x06054b50UL
#define ZIP_CEN_SIG  0x02014b50UL
#define ZIP_LOC_SIG  0x04034b50UL
#define METHOD_STORE 0U
#define METHOD_DEFLATE 8U

struct rom_spec {
    const char *name;
    cv1k_u32 expected_crc;
    cv1k_u32 source_size;
    cv1k_u32 load_size;
    int swap16;
    int role;
};

#define ROLE_U2 0
#define ROLE_U4 1
#define ROLE_U23 2
#define ROLE_U24 3

struct game_set {
    const char *name;
    const char *description;
    /* Entry order is fixed: [0]=NAND(u2), [1]=boot(u4), [2]=sound low(u23),
     * [3]=sound high(u24).  CRCs/sizes/file names taken from the MAME 0.282
     * cave/cv1k.cpp driver manifest (non-merged sets). */
    struct rom_spec specs[CV1K_ROM_ENTRY_COUNT];
    /* Per-game vblank-wait idle loop PC, matching MAME cv1k install_speedups()
     * (idlepc).  The frame runners fast-forward past the spin loop when the main
     * thread parks here, so this must match the game's init_* speedup or the
     * whole frame's spin is interpreted (huge, needless host CPU use). */
    cv1k_u32 idle_pc;
    int display_rotation;
    int model;
};

/* The boot ROM (u4) CRC uniquely identifies each set, so detection keys on it.
 * ddpsdoj is first and is used as the fallback when nothing else matches, which
 * preserves the previous single-set behaviour for unknown dumps. */
static const struct game_set g_games[] = {
    { "mushisam", "Mushihime-Sama (Japan, 2004/10/12.MASTER VER.)", {
        { "mushisam_u2", 0x4f0a842aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "mushisam_u4", 0x15321b30UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x138e2050UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe3d05c9fUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a2aaUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "mushisama", "Mushihime-Sama (Japan, 2004/10/12 MASTER VER.)", {
        { "mushisam_u2", 0x4f0a842aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "mushisama_u4", 0x0b5b30b2UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x138e2050UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe3d05c9fUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a0aaUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "mushisamb", "Mushihime-Sama (Japan, 2004/10/12 MASTER VER)", {
        { "mushisam_u2", 0x4f0a842aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "mushisamb_u4", 0x9f1c7f51UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x138e2050UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe3d05c9fUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a2aaUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "espgal2", "Espgaluda II (Japan, 2005/11/14.MASTER VER.)", {
        { "u2", 0x222f58c7UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "espgal2_u4", 0x2cb37c03UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xb9a10c22UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc76b1ec4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05177aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "espgal2a", "Espgaluda II (Japan, 2005/11/14 MASTER VER, newer CV1000-B PCB)", {
        { "u2", 0x222f58c7UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "espgal2a_u4", 0x843608b8UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xb9a10c22UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc76b1ec4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05177aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "espgal2b", "Espgaluda II (Japan, 2005/11/14 MASTER VER, original CV1000-B PCB)", {
        { "u2", 0x222f58c7UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "espgal2b_u4", 0x09c908bbUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xb9a10c22UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc76b1ec4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05177aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "mushitam", "Puzzle! Mushihime-Tama (Japan, 2005/09/09.MASTER VER)", {
        { "mushitam_u2", 0x8ba498abUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "mushitam_u4", 0xc49eb6b1UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x701a912aUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x6feeb9a1UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a0daUL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_B },
    { "mushitama", "Puzzle! Mushihime-Tama (Japan, 2005/09/09 MASTER VER)", {
        { "mushitam_u2", 0x8ba498abUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "mushitama_u4", 0x4a23e6c8UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x701a912aUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x6feeb9a1UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a0daUL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_B },
    { "futari15", "Mushihime-Sama Futari Ver 1.5 (Japan, 2006/12/8.MASTER VER. 1.54.)", {
        { "futari15_u2", 0xb9eae1fcUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "futari15_u4", 0xe8c5f128UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x39f1e1f4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc631a766UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "futari15a", "Mushihime-Sama Futari Ver 1.5 (Japan, 2006/12/8 MASTER VER 1.54)", {
        { "futari15_u2", 0xb9eae1fcUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "futari15a_u4", 0xa609cf89UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x39f1e1f4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc631a766UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "futari10", "Mushihime-Sama Futari Ver 1.0 (Japan, 2006/10/23 MASTER VER.)", {
        { "futari10_u2", 0x78ffcd0cUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "futari10_u4", 0xb127dca7UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x39f1e1f4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc631a766UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "futaribl", "Mushihime-Sama Futari Black Label - Another Ver (World, 2009/11/27 INTERNATIONAL BL)", {
        { "futariblk_u2", 0x08c6fd62UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "futaribli_u4", 0x1971dd16UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x39f1e1f4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc631a766UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "futariblj", "Mushihime-Sama Futari Black Label (Japan, 2007/12/11 BLACK LABEL VER)", {
        { "futariblk_u2", 0x08c6fd62UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "futariblk_u4", 0xb9467b6dUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x39f1e1f4UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xc631a766UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "ibara", "Ibara (Japan, 2005/03/22 MASTER VER.., '06. 3. 7 ver.)", {
        { "u2", 0x55840976UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ibara_u4", 0xd5fb6657UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xee5e585dUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xf0aa3cb6UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a0aaUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "ibarao", "Ibara (Japan, 2005/03/22 MASTER VER..)", {
        { "u2", 0x55840976UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ibarao_u4", 0x8e6c155dUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xee5e585dUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xf0aa3cb6UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c04a0aaUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "ibarablk", "Ibara Kuro Black Label (Japan, 2006/02/06. MASTER VER.)", {
        { "ibarablk_u2", 0x5e46be44UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ibarablk_u4", 0xee1f1f77UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xa436bb22UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xd11ab6b6UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "ibarablka", "Ibara Kuro Black Label (Japan, 2006/02/06 MASTER VER.)", {
        { "ibarablka_u2", 0x33400d96UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ibarablka_u4", 0xa9d43839UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xa436bb22UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xd11ab6b6UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "deathsml", "Deathsmiles (Japan, 2007/10/09 MASTER VER)", {
        { "u2", 0x59ef5d78UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0x1a7b98bfUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xaab718c8UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x83881d84UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c0519a2UL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_B },
    { "mmpork", "Muchi Muchi Pork! (Japan, 2007/ 4/17 MASTER VER.)", {
        { "u2", 0x1ee961b8UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0xd06cfa42UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4a4b36dfUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xce83d07bUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "mmmbanc", "Medal Mahjong Moukari Bancho (Japan, 2007/06/05 MASTER VER.)", {
        { "u2", 0x2e38965aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0x5589d8c6UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4caaa1bfUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x8e3a51baUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_B },
    { "pinkswts", "Pink Sweets: Ibara Sorekara (Japan, 2006/04/06 MASTER VER....)", {
        { "pinkswts_u2", 0xa2fa5363UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "pinkswts_u4", 0x5d812c9eUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4b82d250UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe93f0627UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "pinkswtsa", "Pink Sweets: Ibara Sorekara (Japan, 2006/04/06 MASTER VER...)", {
        { "pnkswtsa_u2", 0x829a862eUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "pnkswtsa_u4", 0xee3339b2UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4b82d250UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe93f0627UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "pinkswtsb", "Pink Sweets: Ibara Sorekara (Japan, 2006/04/06 MASTER VER.)", {
        { "pnkswtsx_u2", 0x91e4deb2UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "pnkswtsb_u4", 0x68bcc009UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4b82d250UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe93f0627UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "pinkswtsx", "Pink Sweets: Ibara Sorekara (Japan, 2006/xx/xx MASTER VER.)", {
        { "pnkswtsx_u2", 0x91e4deb2UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "pnkswtsx_u4", 0x8fe05bf0UL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4b82d250UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe93f0627UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "pinkswtssc", "Pink Sweets: Suicide Club (2017/10/31 SUICIDECLUB VER., bootleg)", {
        { "suicideclub.u2", 0x32324608UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "suicideclub.u4", 0x5e03662fUL, 0x00200000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x4b82d250UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xe93f0627UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c05176aUL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_B },
    { "ddpdfk", "DoDonPachi Dai-Fukkatsu Ver 1.5 (Japan, 2008/06/23 MASTER VER 1.5)", {
        { "ddpdfk_u2", 0x84a51a4fUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ddpdfk_u4", 0x9976d699UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x27032cdeUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xa6178c2cUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_D },
    { "ddpdfk10", "DoDonPachi Dai-Fukkatsu Ver 1.0 (Japan, 2008/05/16 MASTER VER)", {
        { "ddpdfk10_u2", 0xd349cb2aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "ddpdfk10_u4", 0xa3d650b2UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x27032cdeUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xa6178c2cUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_D },
    { "dsmbl", "Deathsmiles MegaBlack Label (Japan, 2008/10/06 MEGABLACK LABEL VER)", {
        { "u2", 0xd6b85b7aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0x77fc5ad1UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xa9536a6aUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x3b673326UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_D },
    { "dfkbl", "DoDonPachi Dai-Fukkatsu Black Label (Japan, 2010/1/18 BLACK LABEL)", {
        { "u2", 0x29f9d73aUL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0x8092ca9dUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x36d4093bUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x31f9eb0aUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_D },
    { "akatana", "Akai Katana (Japan, 2010/ 8/13 MASTER VER.)", {
        { "u2", 0x89a2e1a5UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0x613fd380UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0x34a67e24UL, 0x00400000UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0x10760fedUL, 0x00400000UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_0, CV1K_MODEL_D },
    { "ddpsdoj", "DoDonPachi SaiDaiOuJou (Japan, 2012/ 4/20)", {
        { "u2", 0x668e4cd6UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2 },
        { "u4", 0xe2a4411cUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U4 },
        { "u23", 0xac94801cUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U23 },
        { "u24", 0xf593045bUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U24 } },
        0x0c1d1346UL, CV1K_DISPLAY_ROT_CCW, CV1K_MODEL_D },
};

#define CV1K_GAME_COUNT (sizeof(g_games) / sizeof(g_games[0]))

/* Active set: defaults to ddpsdoj, replaced by detection in the loader. */
static const struct rom_spec *ddpsdoj_specs = g_games[0].specs;

static cv1k_u16 le16(const cv1k_u8 *p)
{
    return (cv1k_u16)((cv1k_u16)p[0] | ((cv1k_u16)p[1] << 8));
}

static cv1k_u32 le32(const cv1k_u8 *p)
{
    return (cv1k_u32)p[0] | ((cv1k_u32)p[1] << 8) | ((cv1k_u32)p[2] << 16) | ((cv1k_u32)p[3] << 24);
}

static void setmsg(struct cv1k_romset_report *r, const char *s)
{
    if (r != NULL) {
        strncpy(r->message, s, sizeof(r->message) - 1U);
        r->message[sizeof(r->message) - 1U] = '\0';
    }
}

/* Point the report at a specific game manifest without discarding fields the
 * caller may already have populated (source_path is preserved by callers). */
static void report_set_game(struct cv1k_romset_report *r, const struct game_set *g)
{
    int i;
    if (r == NULL || g == NULL) return;
    strncpy(r->set_name, g->name, sizeof(r->set_name) - 1U);
    r->set_name[sizeof(r->set_name) - 1U] = '\0';
    strncpy(r->description, g->description, sizeof(r->description) - 1U);
    r->description[sizeof(r->description) - 1U] = '\0';
    r->idle_pc = g->idle_pc;
    r->display_rotation = g->display_rotation;
    r->model = g->model;
    for (i = 0; i < CV1K_ROM_ENTRY_COUNT; i++) {
        strncpy(r->entry[i].name, g->specs[i].name, sizeof(r->entry[i].name) - 1U);
        r->entry[i].name[sizeof(r->entry[i].name) - 1U] = '\0';
        r->entry[i].expected_crc = g->specs[i].expected_crc;
        r->entry[i].source_size = g->specs[i].source_size;
        r->entry[i].loaded_size = g->specs[i].load_size;
        r->entry[i].ignored_tail = (int)(g->specs[i].source_size - g->specs[i].load_size);
    }
}

void cv1k_romset_report_clear(struct cv1k_romset_report *r)
{
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
    report_set_game(r, &g_games[0]);
}

static cv1k_u32 crc32_buf(const cv1k_u8 *p, cv1k_u32 n)
{
#ifdef CV1K_WITH_ZLIB
    return (cv1k_u32)crc32(0UL, (const Bytef *)p, (uInt)n);
#else
    cv1k_u32 crc;
    cv1k_u32 i;
    int bit;
    crc = 0xffffffffUL;
    for (i = 0UL; i < n; i++) {
        crc ^= (cv1k_u32)p[i];
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1UL) != 0UL) crc = (crc >> 1) ^ 0xedb88320UL;
            else crc >>= 1;
        }
    }
    return crc ^ 0xffffffffUL;
#endif
}

static void copy_mame16(cv1k_u8 *dst, cv1k_u32 dst_size, const cv1k_u8 *src, cv1k_u32 load_size, int swap16)
{
    cv1k_u32 i;
    if (load_size > dst_size) load_size = dst_size;
    memset(dst, 0xff, (size_t)dst_size);
    if (swap16) {
        for (i = 0UL; i + 1UL < load_size; i += 2UL) {
            dst[i] = src[i + 1UL];
            dst[i + 1UL] = src[i];
        }
        if ((load_size & 1UL) != 0UL) dst[load_size - 1UL] = src[load_size - 1UL];
    } else {
        memcpy(dst, src, (size_t)load_size);
    }
}

static int apply_entry(struct cv1k_machine *m, const struct rom_spec *s, const cv1k_u8 *data, cv1k_u32 size, struct cv1k_rom_entry_report *er)
{
    cv1k_u32 actual_crc;
    cv1k_u32 use_size;
    if (m == NULL || s == NULL || data == NULL || er == NULL) return 0;
    er->present = 1;
    er->source_size = size;
    actual_crc = crc32_buf(data, size);
    er->actual_crc = actual_crc;
    er->crc_ok = (actual_crc == s->expected_crc);
    use_size = s->load_size;
    if (use_size > size) use_size = size;
    er->loaded_size = use_size;
    er->ignored_tail = (int)((size > use_size) ? (size - use_size) : 0UL);

    if (s->role == ROLE_U2) {
        if (m->nand.data == NULL) return 0;
        memset(m->nand.data, 0xff, (size_t)m->nand.size);
        if (use_size > m->nand.size) use_size = m->nand.size;
        memcpy(m->nand.data, data, (size_t)use_size);
        cv1k_nand_scan_map(&m->nand);
        m->nand.size = (use_size > 0UL) ? use_size : m->nand.size;
        m->nand.manufacturer = 0xecU;
        m->nand.device = 0xf1U;
        er->loaded_size = use_size;
        return 1;
    }
    if (s->role == ROLE_U4) {
        cv1k_u32 dst_off;
        cv1k_u32 mapped_size;
        cv1k_u32 chunk;
        copy_mame16(m->boot_rom, CV1K_BOOT_ROM_MAX, data, use_size, s->swap16);
        mapped_size = s->load_size;
        if (mapped_size > CV1K_BOOT_ROM_MAX) mapped_size = CV1K_BOOT_ROM_MAX;
        if (use_size > 0UL && mapped_size > use_size) {
            for (dst_off = use_size; dst_off < mapped_size; dst_off += use_size) {
                chunk = mapped_size - dst_off;
                if (chunk > use_size) chunk = use_size;
                copy_mame16(m->boot_rom + dst_off, CV1K_BOOT_ROM_MAX - dst_off, data, chunk, s->swap16);
            }
        }
        m->boot_rom_size = mapped_size;
        er->loaded_size = mapped_size;
        return 1;
    }
    if (s->role == ROLE_U23) {
        if (m->sound_rom == NULL) return 0;
        memset(m->sound_rom, 0xff, (size_t)CV1K_SOUND_ROM_MAX);
        copy_mame16(m->sound_rom, CV1K_SOUND_ROM_MAX / 2UL, data, use_size, s->swap16);
        if (m->sound_rom_size < CV1K_SOUND_ROM_MAX / 2UL) m->sound_rom_size = CV1K_SOUND_ROM_MAX / 2UL;
        er->loaded_size = use_size;
        return 1;
    }
    if (s->role == ROLE_U24) {
        if (m->sound_rom == NULL) return 0;
        copy_mame16(m->sound_rom + (CV1K_SOUND_ROM_MAX / 2UL), CV1K_SOUND_ROM_MAX / 2UL, data, use_size, s->swap16);
        m->sound_rom_size = CV1K_SOUND_ROM_MAX;
        er->loaded_size = use_size;
        return 1;
    }
    return 0;
}

static int path_is_zip(const char *path)
{
    const char *dot;
    if (path == NULL) return 0;
    dot = strrchr(path, '.');
    if (dot == NULL) return 0;
    return (strcmp(dot, ".zip") == 0 || strcmp(dot, ".ZIP") == 0);
}

static int load_file_alloc(const char *path, cv1k_u8 **out, cv1k_u32 *out_size)
{
    cv1k_u32 size;
    cv1k_u8 *buf;
    cv1k_u32 got;
    size = cv1k_file_size(path);
    if (size == 0UL) return 0;
    buf = (cv1k_u8 *)cv1k_xmalloc(size);
    if (buf == NULL) return 0;
    got = 0UL;
    if (!cv1k_read_file(path, buf, size, &got) || got != size) {
        cv1k_free(buf);
        return 0;
    }
    *out = buf;
    *out_size = size;
    return 1;
}

static int load_from_dir(struct cv1k_machine *m, const char *dir, struct cv1k_romset_report *r)
{
    int i;
    int all_ok;
    char path[512];
    cv1k_u8 *buf;
    cv1k_u32 size;
    all_ok = 1;
    for (i = 0; i < CV1K_ROM_ENTRY_COUNT; i++) {
        size_t n;
        n = strlen(dir);
        if (n > 0U && (dir[n - 1U] == '/' || dir[n - 1U] == '\\')) sprintf(path, "%s%s", dir, ddpsdoj_specs[i].name);
        else sprintf(path, "%s/%s", dir, ddpsdoj_specs[i].name);
        buf = NULL;
        size = 0UL;
        if (!load_file_alloc(path, &buf, &size)) {
            r->entry[i].present = 0;
            all_ok = 0;
            continue;
        }
        if (!apply_entry(m, &ddpsdoj_specs[i], buf, size, &r->entry[i])) all_ok = 0;
        if (!r->entry[i].crc_ok) all_ok = 0;
        cv1k_free(buf);
    }
    return all_ok;
}

#ifdef CV1K_WITH_ZLIB
static int inflate_raw(const cv1k_u8 *src, cv1k_u32 src_size, cv1k_u8 *dst, cv1k_u32 dst_size)
{
    z_stream z;
    int rc;
    memset(&z, 0, sizeof(z));
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_size;
    z.next_out = (Bytef *)dst;
    z.avail_out = (uInt)dst_size;
    rc = inflateInit2(&z, -MAX_WBITS);
    if (rc != Z_OK) return 0;
    rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return rc == Z_STREAM_END && z.total_out == dst_size;
}

static int zip_extract_entry(const cv1k_u8 *zip, cv1k_u32 zip_size, const char *wanted, cv1k_u8 **out, cv1k_u32 *out_size)
{
    cv1k_u32 eocd;
    cv1k_u32 cd_off;
    cv1k_u32 cd_size;
    cv1k_u32 pos;
    cv1k_u16 count;
    cv1k_u16 i;
    eocd = zip_size;
    if (zip_size < 22UL) return 0;
    while (eocd > 0UL) {
        eocd--;
        if (eocd + 22UL <= zip_size && le32(zip + eocd) == ZIP_EOCD_SIG) break;
        if (zip_size - eocd > 0x10000UL + 22UL) return 0;
    }
    if (eocd == 0UL && le32(zip) != ZIP_EOCD_SIG) return 0;
    count = le16(zip + eocd + 10UL);
    cd_size = le32(zip + eocd + 12UL);
    cd_off = le32(zip + eocd + 16UL);
    if (cd_off + cd_size > zip_size) return 0;
    pos = cd_off;
    for (i = 0; i < count && pos + 46UL <= zip_size; i++) {
        cv1k_u16 method;
        cv1k_u32 crc;
        cv1k_u32 comp_size;
        cv1k_u32 uncomp_size;
        cv1k_u16 name_len;
        cv1k_u16 extra_len;
        cv1k_u16 comment_len;
        cv1k_u32 local_off;
        char name[128];
        if (le32(zip + pos) != ZIP_CEN_SIG) return 0;
        method = le16(zip + pos + 10UL);
        crc = le32(zip + pos + 16UL);
        comp_size = le32(zip + pos + 20UL);
        uncomp_size = le32(zip + pos + 24UL);
        name_len = le16(zip + pos + 28UL);
        extra_len = le16(zip + pos + 30UL);
        comment_len = le16(zip + pos + 32UL);
        local_off = le32(zip + pos + 42UL);
        if (pos + 46UL + name_len + extra_len + comment_len > zip_size) return 0;
        memset(name, 0, sizeof(name));
        if (name_len < sizeof(name)) memcpy(name, zip + pos + 46UL, (size_t)name_len);
        if (strcmp(name, wanted) == 0) {
            cv1k_u16 lname;
            cv1k_u16 lextra;
            cv1k_u32 data_off;
            cv1k_u8 *buf;
            cv1k_u32 check;
            if (local_off + 30UL > zip_size || le32(zip + local_off) != ZIP_LOC_SIG) return 0;
            lname = le16(zip + local_off + 26UL);
            lextra = le16(zip + local_off + 28UL);
            data_off = local_off + 30UL + (cv1k_u32)lname + (cv1k_u32)lextra;
            if (data_off + comp_size > zip_size) return 0;
            buf = (cv1k_u8 *)cv1k_xmalloc(uncomp_size);
            if (buf == NULL) return 0;
            if (method == METHOD_STORE) memcpy(buf, zip + data_off, (size_t)uncomp_size);
            else if (method == METHOD_DEFLATE) {
                if (!inflate_raw(zip + data_off, comp_size, buf, uncomp_size)) {
                    cv1k_free(buf);
                    return 0;
                }
            } else {
                cv1k_free(buf);
                return 0;
            }
            check = crc32_buf(buf, uncomp_size);
            if (check != crc) {
                cv1k_free(buf);
                return 0;
            }
            *out = buf;
            *out_size = uncomp_size;
            return 1;
        }
        pos += 46UL + (cv1k_u32)name_len + (cv1k_u32)extra_len + (cv1k_u32)comment_len;
    }
    return 0;
}

static int load_from_zip(struct cv1k_machine *m, const char *path, struct cv1k_romset_report *r)
{
    cv1k_u8 *zip;
    cv1k_u32 zip_size;
    int i;
    int all_ok;
    zip = NULL;
    zip_size = 0UL;
    if (!load_file_alloc(path, &zip, &zip_size)) return 0;
    all_ok = 1;
    for (i = 0; i < CV1K_ROM_ENTRY_COUNT; i++) {
        cv1k_u8 *buf;
        cv1k_u32 size;
        buf = NULL;
        size = 0UL;
        if (!zip_extract_entry(zip, zip_size, ddpsdoj_specs[i].name, &buf, &size)) {
            r->entry[i].present = 0;
            all_ok = 0;
            continue;
        }
        if (!apply_entry(m, &ddpsdoj_specs[i], buf, size, &r->entry[i])) all_ok = 0;
        if (!r->entry[i].crc_ok) all_ok = 0;
        cv1k_free(buf);
    }
    cv1k_free(zip);
    return all_ok;
}
#endif

/* Identify the CV1000 set by its boot ROM (u4) CRC so the report uses the
 * matching manifest instead of always assuming ddpsdoj.  Returns an index into
 * g_games; falls back to 0 (ddpsdoj) when nothing matches. */
static int detect_game(const char *path, int is_zip)
{
    unsigned int g;
#ifdef CV1K_WITH_ZLIB
    if (is_zip) {
        cv1k_u8 *zip = NULL;
        cv1k_u32 zsz = 0UL;
        int found = 0;
        if (!load_file_alloc(path, &zip, &zsz)) return 0;
        for (g = 0U; g < CV1K_GAME_COUNT; g++) {
            cv1k_u8 *buf = NULL;
            cv1k_u32 size = 0UL;
            if (zip_extract_entry(zip, zsz, g_games[g].specs[1].name, &buf, &size)) {
                cv1k_u32 crc = crc32_buf(buf, size);
                cv1k_free(buf);
                if (crc == g_games[g].specs[1].expected_crc) { found = (int)g; break; }
            }
        }
        cv1k_free(zip);
        return found;
    }
#endif
    for (g = 0U; g < CV1K_GAME_COUNT; g++) {
        char p[512];
        cv1k_u8 *buf = NULL;
        cv1k_u32 size = 0UL;
        size_t n = strlen(path);
        if (n > 0U && (path[n - 1U] == '/' || path[n - 1U] == '\\')) sprintf(p, "%s%s", path, g_games[g].specs[1].name);
        else sprintf(p, "%s/%s", path, g_games[g].specs[1].name);
        if (load_file_alloc(p, &buf, &size)) {
            cv1k_u32 crc = crc32_buf(buf, size);
            cv1k_free(buf);
            if (crc == g_games[g].specs[1].expected_crc) return (int)g;
        }
    }
    return 0;
}

int cv1k_romset_load_ddpsdoj(struct cv1k_machine *m, const char *path, struct cv1k_romset_report *report)
{
    int ok;
    int gi;
    cv1k_romset_report_clear(report);
    if (report != NULL && path != NULL) {
        strncpy(report->source_path, path, sizeof(report->source_path) - 1U);
        report->source_path[sizeof(report->source_path) - 1U] = '\0';
    }
    if (m == NULL || path == NULL || report == NULL) return 0;
    gi = detect_game(path, path_is_zip(path));
    if (m->model != g_games[gi].model) {
        cv1k_machine_shutdown(m);
        if (!cv1k_machine_init(m, g_games[gi].model)) {
            report_set_game(report, &g_games[gi]);
            setmsg(report, "machine reinitialization failed for detected CV1000 model");
            report->ok = 0;
            return 0;
        }
    }
    ddpsdoj_specs = g_games[gi].specs;
    report_set_game(report, &g_games[gi]);
    /* Point the idle-loop fast-forward at this game's vblank-wait spin PC.  Kept
     * on the machine so a mid-run CPU reset (which zeroes the cpu state) does not
     * revert it; cv1k_machine_frame_advance re-applies it to the cpu each frame. */
    m->idle_pc0 = g_games[gi].idle_pc;
    m->idle_pc1 = g_games[gi].idle_pc + 2UL;
    m->cpu.idle_pc0 = m->idle_pc0;
    m->cpu.idle_pc1 = m->idle_pc1;
    ok = 0;
    if (path_is_zip(path)) {
        report->used_zip = 1;
#ifdef CV1K_WITH_ZLIB
        ok = load_from_zip(m, path, report);
#else
        setmsg(report, "zip support was not compiled; use an extracted rom directory or rebuild with zlib");
        ok = 0;
#endif
    } else {
        report->used_zip = 0;
        ok = load_from_dir(m, path, report);
    }
    report->boot_loaded = m->boot_rom_size;
    report->nand_loaded = m->nand.size;
    report->sound_loaded = m->sound_rom_size;
    report->ok = ok;
    if (ok) {
        char msg[128];
        sprintf(msg, "%s romset loaded and CRC matched MAME cv1k manifest", report->set_name);
        setmsg(report, msg);
    } else if (report->message[0] == '\0') {
        setmsg(report, "romset load failed or CRC mismatch; see entries");
    }
    return ok;
}

void cv1k_romset_report_text(const struct cv1k_romset_report *r, char *out, cv1k_u32 out_size)
{
    int i;
    char line[128];
    if (out == NULL || out_size == 0UL) return;
    out[0] = '\0';
    if (r == NULL) return;
    sprintf(out, "set=%s model=%s rotation=%s idlepc=%08lx source=%s zip=%d ok=%d\n", r->set_name, (r->model == CV1K_MODEL_D) ? "CV1000-D" : "CV1000-B", cv1k_video_display_rotation_name(r->display_rotation), (unsigned long)r->idle_pc, r->source_path, r->used_zip, r->ok);
    for (i = 0; i < CV1K_ROM_ENTRY_COUNT; i++) {
        sprintf(line, "%s present=%d crc=%08lx expected=%08lx size=%lu load=%lu ignore=%d\n",
            r->entry[i].name,
            r->entry[i].present,
            (unsigned long)r->entry[i].actual_crc,
            (unsigned long)r->entry[i].expected_crc,
            (unsigned long)r->entry[i].source_size,
            (unsigned long)r->entry[i].loaded_size,
            r->entry[i].ignored_tail);
        if (strlen(out) + strlen(line) + 1U < out_size) strcat(out, line);
    }
    if (strlen(out) + strlen(r->message) + 2U < out_size) {
        strcat(out, r->message);
        strcat(out, "\n");
    }
}
