#include <emscripten/emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

#define FATFSJS_SECTOR_SIZE 4096
#define FATFSJS_PATH_MAX 512
#define FATFSJS_MAX_READ_CHUNK 4096

#define FATFSJS_ERR_INVAL -1
#define FATFSJS_ERR_NOT_MOUNTED -2
#define FATFSJS_ERR_NOSPC -3
#define FATFSJS_ERR_IO -4

static FATFS g_fs;
static bool g_is_mounted = false;
static uint8_t *g_storage = NULL;
static uint32_t g_sector_count = 0;
static uint32_t g_volume_sector_count = 0;
static uint32_t g_sector_offset = 0;
static uint32_t g_total_bytes = 0;

static int fatfsjs_result(FRESULT res) {
    return res == FR_OK ? 0 : -((int)res);
}

static int fatfsjs_ensure_mounted(void) {
    return g_is_mounted ? 0 : FATFSJS_ERR_NOT_MOUNTED;
}

static uint16_t fatfsjs_read_u16(const uint8_t *ptr) {
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static bool fatfsjs_starts_with_ci(const char *value, const char *prefix) {
    while (*prefix) {
        if (*value == '\0') {
            return false;
        }
        unsigned char a = (unsigned char)*value;
        unsigned char b = (unsigned char)*prefix;
        if ((a | 0x20) != (b | 0x20)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static bool fatfsjs_is_boot_sector(const uint8_t *sector) {
    if (!sector) {
        return false;
    }
    uint8_t jump = sector[0];
    if (jump != 0xEB && jump != 0xE9) {
        return false;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return false;
    }
    return fatfsjs_read_u16(sector + 11) == FATFSJS_SECTOR_SIZE;
}

static void fatfsjs_detect_offset(void) {
    g_sector_offset = 0;
    g_volume_sector_count = g_sector_count;
    if (!g_storage || g_sector_count < 2) {
        return;
    }
    const uint8_t *s0 = g_storage;
    const uint8_t *s1 = g_storage + FATFSJS_SECTOR_SIZE;
    bool boot0 = fatfsjs_is_boot_sector(s0);
    bool boot1 = fatfsjs_is_boot_sector(s1);
    if (boot1 && !boot0) {
        g_sector_offset = 1;
    } else if (boot0 && boot1) {
        g_sector_offset = 1;
    }
    if (g_sector_offset >= g_sector_count) {
        g_sector_offset = 0;
    }
    g_volume_sector_count = g_sector_count - g_sector_offset;
}

static const char *fatfsjs_skip_mount(const char *path) {
    if (!path) {
        return "";
    }
    const char *cursor = path;
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    if (fatfsjs_starts_with_ci(cursor, "fatfs")) {
        const char *after = cursor + 5;
        if (*after == '\0' || *after == '/' || *after == '\\') {
            cursor = after;
        }
    }
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    return cursor;
}

static int fatfsjs_build_ff_path(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    const char *cursor = fatfsjs_skip_mount(path);
    if (!cursor || *cursor == '\0') {
        if (snprintf(out, out_len, "0:/") >= (int)out_len) {
            return FATFSJS_ERR_INVAL;
        }
        return 0;
    }

    size_t len = 0;
    len += (size_t)snprintf(out, out_len, "0:/");
    if (len >= out_len) {
        return FATFSJS_ERR_INVAL;
    }

    bool last_sep = false;
    while (*cursor) {
        char ch = *cursor++;
        if (ch == '/' || ch == '\\') {
            if (!last_sep && len + 1 < out_len) {
                out[len++] = '/';
                last_sep = true;
            }
            continue;
        }
        if (len + 1 >= out_len) {
            return FATFSJS_ERR_INVAL;
        }
        out[len++] = ch;
        last_sep = false;
    }
    if (len > 3 && out[len - 1] == '/') {
        len--;
    }
    out[len] = '\0';
    return 0;
}

static int fatfsjs_emit_entry(const char *path, FSIZE_t size, char type,
                              char **cursor, const char *end) {
    int needed = snprintf(NULL, 0, "%s\t%lu\t%c\n", path ? path : "",
                          (unsigned long)size, type);
    if (needed < 0) {
        return FATFSJS_ERR_IO;
    }
    if (*cursor + needed + 1 > end) {
        return FATFSJS_ERR_NOSPC;
    }
    int written =
        snprintf(*cursor, (size_t)(end - *cursor), "%s\t%lu\t%c\n",
                 path ? path : "", (unsigned long)size, type);
    if (written != needed) {
        return FATFSJS_ERR_IO;
    }
    *cursor += written;
    return 0;
}

static int fatfsjs_join_path(const char *base, const char *leaf, char *out,
                             size_t out_len) {
    if (!base || base[0] == '\0') {
        if ((size_t)snprintf(out, out_len, "%s", leaf) >= out_len) {
            return FATFSJS_ERR_INVAL;
        }
        return 0;
    }
    if ((size_t)snprintf(out, out_len, "%s/%s", base, leaf) >= out_len) {
        return FATFSJS_ERR_INVAL;
    }
    return 0;
}

static int fatfsjs_list_dir(const char *ff_path, const char *rel_prefix,
                            char **cursor, const char *end) {
    FF_DIR dir;
    FILINFO info;
    FRESULT res = f_opendir(&dir, ff_path);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }

    while (true) {
        memset(&info, 0, sizeof(info));
        res = f_readdir(&dir, &info);
        if (res != FR_OK) {
            f_closedir(&dir);
            return fatfsjs_result(res);
        }
        if (info.fname[0] == '\0') {
            break;
        }

        const char *name = info.fname[0] ? info.fname : info.altname;
        if (!name || name[0] == '\0') {
            continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        char rel_path[FATFSJS_PATH_MAX];
        int err = fatfsjs_join_path(rel_prefix, name, rel_path,
                                    sizeof(rel_path));
        if (err) {
            f_closedir(&dir);
            return err;
        }

        char type = (info.fattrib & AM_DIR) ? 'd' : 'f';
        err = fatfsjs_emit_entry(rel_path, info.fsize, type, cursor, end);
        if (err) {
            f_closedir(&dir);
            return err;
        }

        if (info.fattrib & AM_DIR) {
            char child_ff[FATFSJS_PATH_MAX];
            err = fatfsjs_join_path(ff_path, name, child_ff, sizeof(child_ff));
            if (err) {
                f_closedir(&dir);
                return err;
            }
            err = fatfsjs_list_dir(child_ff, rel_path, cursor, end);
            if (err) {
                f_closedir(&dir);
                return err;
            }
        }
    }

    f_closedir(&dir);
    return 0;
}

static int fatfsjs_ensure_parent_dirs(const char *ff_path) {
    if (!ff_path) {
        return FATFSJS_ERR_INVAL;
    }
    char temp[FATFSJS_PATH_MAX];
    if (strlen(ff_path) >= sizeof(temp)) {
        return FATFSJS_ERR_INVAL;
    }
    strcpy(temp, ff_path);

    char *slash = strchr(temp, '/');
    if (!slash) {
        return 0;
    }
    slash++; /* after "0:" */
    if (*slash == '\0') {
        return 0;
    }

    char *last = strrchr(slash, '/');
    if (!last) {
        return 0;
    }

    for (char *ptr = slash; ptr < last; ptr++) {
        if (*ptr == '/') {
            *ptr = '\0';
            FRESULT res = f_mkdir(temp);
            if (res != FR_OK && res != FR_EXIST) {
                *ptr = '/';
                return fatfsjs_result(res);
            }
            *ptr = '/';
        }
    }
    return 0;
}

static int fatfsjs_format_internal(void) {
    MKFS_PARM options;
    options.fmt = FM_FAT;
    options.n_fat = 0;
    options.align = 0;
    options.n_root = 0;
    options.au_size = 0;

    uint8_t *work = (uint8_t *)malloc(FATFSJS_SECTOR_SIZE);
    if (!work) {
        return FATFSJS_ERR_NOSPC;
    }
    FRESULT res = f_mkfs("0:", &options, work, FATFSJS_SECTOR_SIZE);
    free(work);
    return fatfsjs_result(res);
}

static void fatfsjs_release(void) {
    if (g_is_mounted) {
        f_mount(NULL, "0:", 0);
        g_is_mounted = false;
    }
    free(g_storage);
    g_storage = NULL;
    g_sector_count = 0;
    g_volume_sector_count = 0;
    g_sector_offset = 0;
    g_total_bytes = 0;
    memset(&g_fs, 0, sizeof(g_fs));
}

static int fatfsjs_configure(uint32_t block_size, uint32_t block_count,
                             bool clear_storage) {
    if (block_size != FATFSJS_SECTOR_SIZE || block_count == 0) {
        return FATFSJS_ERR_INVAL;
    }

    uint64_t total = (uint64_t)block_size * block_count;
    if (total == 0 || total > UINT32_MAX) {
        return FATFSJS_ERR_INVAL;
    }

    fatfsjs_release();
    g_storage = (uint8_t *)malloc((size_t)total);
    if (!g_storage) {
        return FATFSJS_ERR_NOSPC;
    }
    if (clear_storage) {
        memset(g_storage, 0xFF, (size_t)total);
    }
    g_sector_count = block_count;
    g_volume_sector_count = block_count;
    g_sector_offset = 0;
    g_total_bytes = (uint32_t)total;
    return 0;
}

static int fatfsjs_mount_internal(bool allow_format) {
    FRESULT res = f_mount(&g_fs, "0:", 1);
    if (res != FR_OK && allow_format) {
        int format_res = fatfsjs_format_internal();
        if (format_res < 0) {
            g_is_mounted = false;
            return format_res;
        }
        res = f_mount(&g_fs, "0:", 1);
    }
    g_is_mounted = (res == FR_OK);
    return fatfsjs_result(res);
}

DWORD get_fattime(void) {
    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0 || !g_storage) {
        return STA_NOINIT;
    }
    return 0;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0 || !g_storage) {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_storage || !buff || count == 0) {
        return RES_PARERR;
    }
    if (sector + count > g_volume_sector_count) {
        return RES_PARERR;
    }
    uint64_t offset =
        (uint64_t)(sector + g_sector_offset) * FATFSJS_SECTOR_SIZE;
    uint64_t length = (uint64_t)count * FATFSJS_SECTOR_SIZE;
    if (offset + length > g_total_bytes) {
        return RES_PARERR;
    }
    memcpy(buff, g_storage + offset, (size_t)length);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !g_storage || !buff || count == 0) {
        return RES_PARERR;
    }
    if (sector + count > g_volume_sector_count) {
        return RES_PARERR;
    }
    uint64_t offset =
        (uint64_t)(sector + g_sector_offset) * FATFSJS_SECTOR_SIZE;
    uint64_t length = (uint64_t)count * FATFSJS_SECTOR_SIZE;
    if (offset + length > g_total_bytes) {
        return RES_PARERR;
    }
    memcpy(g_storage + offset, buff, (size_t)length);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0 || !g_storage) {
        return RES_PARERR;
    }
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            if (!buff) {
                return RES_PARERR;
            }
            *(LBA_t *)buff = g_volume_sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE:
            if (!buff) {
                return RES_PARERR;
            }
            *(WORD *)buff = FATFSJS_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            if (!buff) {
                return RES_PARERR;
            }
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init(uint32_t block_size, uint32_t block_count) {
    int err = fatfsjs_configure(block_size, block_count, true);
    if (err) {
        return err;
    }
    err = fatfsjs_mount_internal(true);
    if (err) {
        fatfsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init_from_image(const uint8_t *image, uint32_t image_len) {
    if (!image || image_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    if (image_len % FATFSJS_SECTOR_SIZE != 0) {
        return FATFSJS_ERR_INVAL;
    }
    uint32_t block_count = image_len / FATFSJS_SECTOR_SIZE;
    int err = fatfsjs_configure(FATFSJS_SECTOR_SIZE, block_count, false);
    if (err) {
        return err;
    }
    memcpy(g_storage, image, image_len);
    fatfsjs_detect_offset();
    err = fatfsjs_mount_internal(false);
    if (err) {
        fatfsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_format(void) {
    if (!g_storage || g_sector_count == 0) {
        return FATFSJS_ERR_NOT_MOUNTED;
    }
    if (g_is_mounted) {
        f_mount(NULL, "0:", 0);
        g_is_mounted = false;
    }
    int err = fatfsjs_format_internal();
    if (err) {
        return err;
    }
    return fatfsjs_mount_internal(false);
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_list(const char *path, uint32_t buffer_ptr, uint32_t buffer_len) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!buffer_ptr || buffer_len == 0) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    char *cursor = (char *)(uintptr_t)buffer_ptr;
    const char *end = cursor + buffer_len;
    *cursor = '\0';

    FILINFO info;
    FRESULT res = f_stat(ff_path, &info);
    if (res == FR_OK && !(info.fattrib & AM_DIR)) {
        err = fatfsjs_emit_entry("", info.fsize, 'f', &cursor, end);
        if (err) {
            return err;
        }
        if (cursor < end) {
            *cursor = '\0';
        }
        return (int)(cursor - (char *)(uintptr_t)buffer_ptr);
    }

    err = fatfsjs_list_dir(ff_path, "", &cursor, end);
    if (err) {
        return err;
    }

    if (cursor < end) {
        *cursor = '\0';
    }
    return (int)(cursor - (char *)(uintptr_t)buffer_ptr);
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_file_size(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    FILINFO info;
    FRESULT res = f_stat(ff_path, &info);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }
    if (info.fattrib & AM_DIR) {
        return FATFSJS_ERR_INVAL;
    }
    return (int)info.fsize;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_read_file(const char *path, uint32_t buffer_ptr,
                      uint32_t buffer_len) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || !buffer_ptr) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    FILINFO info;
    FRESULT res = f_stat(ff_path, &info);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }
    if (info.fattrib & AM_DIR) {
        return FATFSJS_ERR_INVAL;
    }
    if (info.fsize > buffer_len) {
        return FATFSJS_ERR_NOSPC;
    }

    FIL file;
    res = f_open(&file, ff_path, FA_READ);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)buffer_ptr;
    UINT remaining = (UINT)info.fsize;
    UINT total_read = 0;
    while (remaining > 0) {
        UINT chunk = remaining > FATFSJS_MAX_READ_CHUNK
                         ? FATFSJS_MAX_READ_CHUNK
                         : remaining;
        UINT read = 0;
        res = f_read(&file, dest + total_read, chunk, &read);
        if (res != FR_OK) {
            f_close(&file);
            return fatfsjs_result(res);
        }
        if (read == 0) {
            break;
        }
        total_read += read;
        remaining -= read;
    }
    f_close(&file);

    if (total_read != info.fsize) {
        return FATFSJS_ERR_IO;
    }
    return (int)info.fsize;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_write_file(const char *path, const uint8_t *data,
                       uint32_t length) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || (length > 0 && !data)) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    err = fatfsjs_ensure_parent_dirs(ff_path);
    if (err) {
        return err;
    }

    FIL file;
    FRESULT res = f_open(&file, ff_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }

    UINT remaining = length;
    UINT written_total = 0;
    while (remaining > 0) {
        UINT chunk = remaining > FATFSJS_MAX_READ_CHUNK
                         ? FATFSJS_MAX_READ_CHUNK
                         : remaining;
        UINT written = 0;
        res = f_write(&file, data + written_total, chunk, &written);
        if (res != FR_OK) {
            f_close(&file);
            return fatfsjs_result(res);
        }
        written_total += written;
        remaining -= written;
    }

    res = f_close(&file);
    if (res != FR_OK) {
        return fatfsjs_result(res);
    }
    if (written_total != length) {
        return FATFSJS_ERR_IO;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_delete_file(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    return fatfsjs_result(f_unlink(ff_path));
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_mkdir(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_path[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(path, ff_path, sizeof(ff_path));
    if (err) {
        return err;
    }

    return fatfsjs_result(f_mkdir(ff_path));
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_rename(const char *old_path, const char *new_path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!old_path || !new_path) {
        return FATFSJS_ERR_INVAL;
    }

    char ff_old[FATFSJS_PATH_MAX];
    char ff_new[FATFSJS_PATH_MAX];
    err = fatfsjs_build_ff_path(old_path, ff_old, sizeof(ff_old));
    if (err) {
        return err;
    }
    err = fatfsjs_build_ff_path(new_path, ff_new, sizeof(ff_new));
    if (err) {
        return err;
    }

    return fatfsjs_result(f_rename(ff_old, ff_new));
}

EMSCRIPTEN_KEEPALIVE
uint32_t fatfsjs_storage_size(void) {
    return g_total_bytes;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_export_image(uint32_t buffer_ptr, uint32_t buffer_len) {
    if (!g_storage || g_total_bytes == 0) {
        return FATFSJS_ERR_INVAL;
    }
    if (!buffer_ptr || buffer_len < g_total_bytes) {
        return FATFSJS_ERR_NOSPC;
    }
    memcpy((void *)(uintptr_t)buffer_ptr, g_storage, g_total_bytes);
    return (int)g_total_bytes;
}
