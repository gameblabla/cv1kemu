#include "romset.h"
#include "platform.h"
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

static const struct rom_spec ddpsdoj_specs[CV1K_ROM_ENTRY_COUNT] = {
    { "u2",  0x668e4cd6UL, 0x08400000UL, 0x08400000UL, 0, ROLE_U2  },
    { "u4",  0xe2a4411cUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U4  },
    { "u23", 0xac94801cUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U23 },
    { "u24", 0xf593045bUL, 0x00400100UL, 0x00400000UL, 1, ROLE_U24 }
};

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

void cv1k_romset_report_clear(struct cv1k_romset_report *r)
{
    int i;
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
    strcpy(r->set_name, "ddpsdoj");
    for (i = 0; i < CV1K_ROM_ENTRY_COUNT; i++) {
        strncpy(r->entry[i].name, ddpsdoj_specs[i].name, sizeof(r->entry[i].name) - 1U);
        r->entry[i].expected_crc = ddpsdoj_specs[i].expected_crc;
        r->entry[i].source_size = ddpsdoj_specs[i].source_size;
        r->entry[i].loaded_size = ddpsdoj_specs[i].load_size;
        r->entry[i].ignored_tail = (int)(ddpsdoj_specs[i].source_size - ddpsdoj_specs[i].load_size);
    }
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
        copy_mame16(m->boot_rom, CV1K_BOOT_ROM_MAX, data, use_size, s->swap16);
        m->boot_rom_size = use_size;
        er->loaded_size = use_size;
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

int cv1k_romset_load_ddpsdoj(struct cv1k_machine *m, const char *path, struct cv1k_romset_report *report)
{
    int ok;
    cv1k_romset_report_clear(report);
    if (report != NULL && path != NULL) {
        strncpy(report->source_path, path, sizeof(report->source_path) - 1U);
        report->source_path[sizeof(report->source_path) - 1U] = '\0';
    }
    if (m == NULL || path == NULL || report == NULL) return 0;
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
    if (ok) setmsg(report, "ddpsdoj romset loaded and CRC matched MAME manifest");
    else if (report->message[0] == '\0') setmsg(report, "romset load failed or CRC mismatch; see entries");
    return ok;
}

void cv1k_romset_report_text(const struct cv1k_romset_report *r, char *out, cv1k_u32 out_size)
{
    int i;
    char line[128];
    if (out == NULL || out_size == 0UL) return;
    out[0] = '\0';
    if (r == NULL) return;
    sprintf(out, "set=%s source=%s zip=%d ok=%d\n", r->set_name, r->source_path, r->used_zip, r->ok);
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
