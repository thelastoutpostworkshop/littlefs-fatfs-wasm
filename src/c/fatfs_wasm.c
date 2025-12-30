#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "ff.h"
#include "diskio.h"

#define FATFSJS_PATH_MAX 512
#define FATFSJS_MAX_READ_CHUNK 4096
#define FATFSJS_DRIVE 0

static FATFS g_fs;
static bool g_is_mounted = false;
static uint8_t *g_storage = NULL;
static uint32_t g_sector_size = 0;
static uint32_t g_sector_count = 0;
static bool g_disk_ready = false;

static size_t fatfsjs_total_bytes(void) {
    return (size_t)g_sector_size * g_sector_count;
}

static void fatfsjs_release(void) {
    if (g_is_mounted) {
        f_mount(NULL, "", 0);
        g_is_mounted = false;
    }
    if (g_storage) {
        free(g_storage);
        g_storage = NULL;
    }
    g_sector_size = 0;
    g_sector_count = 0;
    g_disk_ready = false;
}

static int fatfsjs_result(FRESULT fr) {
    return fr == FR_OK ? 0 : -(int)fr;
}

static int fatfsjs_configure(uint32_t sector_size, uint32_t sector_count) {
    if (sector_size == 0 || sector_count == 0) {
        return -(int)FR_INVALID_PARAMETER;
    }

    fatfsjs_release();

    size_t total = (size_t)sector_size * sector_count;
    g_storage = (uint8_t *)malloc(total);
    if (!g_storage) {
        return -(int)FR_NOT_ENOUGH_CORE;
    }
    memset(g_storage, 0xFF, total);

    g_sector_size = sector_size;
    g_sector_count = sector_count;
    g_disk_ready = true;
    return 0;
}

static FRESULT fatfsjs_makefs(void) {
    UINT work_size = FF_MAX_SS;
    uint8_t work[FF_MAX_SS];
    MKFS_PARM opt = {
        .fmt = FM_FAT | FM_SFD,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 0};

    uint32_t cluster_in_sectors = (32 * 1024u) / g_sector_size;
    if (cluster_in_sectors == 0) {
        cluster_in_sectors = 1;
    }
    opt.au_size = cluster_in_sectors;

    return f_mkfs("", &opt, work, work_size);
}

static int fatfsjs_mount(bool allow_format) {
    FRESULT fr = f_mount(&g_fs, "", 1);
    if (fr != FR_OK && allow_format) {
        fr = fatfsjs_makefs();
        if (fr == FR_OK) {
            fr = f_mount(&g_fs, "", 1);
        }
    }

    g_is_mounted = (fr == FR_OK);
    return fatfsjs_result(fr);
}

static int fatfsjs_ensure_mounted(void) {
    if (!g_is_mounted) {
        return -(int)FR_NOT_READY;
    }
    return 0;
}

// Disk IO hooks -------------------------------------------------------------

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != FATFSJS_DRIVE || !g_storage) {
        return STA_NOINIT;
    }
    g_disk_ready = true;
    return 0;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != FATFSJS_DRIVE || !g_disk_ready) {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != FATFSJS_DRIVE || !g_disk_ready) {
        return RES_NOTRDY;
    }
    size_t offset = (size_t)sector * g_sector_size;
    size_t length = (size_t)count * g_sector_size;
    if (offset + length > fatfsjs_total_bytes()) {
        return RES_PARERR;
    }
    memcpy(buff, g_storage + offset, length);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != FATFSJS_DRIVE || !g_disk_ready) {
        return RES_NOTRDY;
    }
    size_t offset = (size_t)sector * g_sector_size;
    size_t length = (size_t)count * g_sector_size;
    if (offset + length > fatfsjs_total_bytes()) {
        return RES_PARERR;
    }
    memcpy(g_storage + offset, buff, length);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != FATFSJS_DRIVE || !g_disk_ready) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)g_sector_size;
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = g_sector_count;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

DWORD get_fattime(void) {
    return 0;
}

void *ff_memalloc(UINT msize) {
    return malloc(msize);
}

void ff_memfree(void *mblock) {
    free(mblock);
}

// Helpers -------------------------------------------------------------------

static int fatfsjs_join_path(const char *base, const char *leaf, char *out,
                             size_t out_len) {
    if (strcmp(base, "/") == 0) {
        return snprintf(out, out_len, "/%s", leaf) >= (int)out_len
                   ? -(int)FR_INVALID_NAME
                   : 0;
    }
    return snprintf(out, out_len, "%s/%s", base, leaf) >= (int)out_len
               ? -(int)FR_INVALID_NAME
               : 0;
}

static int fatfsjs_emit_entry(const char *path, FSIZE_t size, char type,
                              char **cursor, const char *end) {
    const char *display = path;
    if (display[0] == '/' && display[1] != '\0') {
        display = &display[1];
    }
    unsigned long long display_size = (unsigned long long)size;
    int needed = snprintf(NULL, 0, "%s\t%llu\t%c\n", display, display_size,
                          type);
    if (needed < 0) {
        return -(int)FR_INT_ERR;
    }
    if (*cursor + needed + 1 > end) {
        return -(int)FR_NOT_ENOUGH_CORE;
    }
    int written = snprintf(*cursor, (size_t)(end - *cursor), "%s\t%llu\t%c\n",
                           display, display_size, type);
    if (written != needed) {
        return -(int)FR_INT_ERR;
    }
    *cursor += written;
    return 0;
}

static int fatfsjs_walk(const char *path, char **cursor, const char *end) {
    DIR dir;
    FILINFO info;
    memset(&info, 0, sizeof(info));

    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }

    while (true) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK) {
            f_closedir(&dir);
            return fatfsjs_result(fr);
        }
        if (info.fname[0] == '\0') {
            break;
        }
        const char *name = info.fname;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        char child[FATFSJS_PATH_MAX];
        if (fatfsjs_join_path(path, name, child, sizeof(child)) != 0) {
            f_closedir(&dir);
            return -(int)FR_INVALID_NAME;
        }

        if (info.fattrib & AM_DIR) {
            int err = fatfsjs_emit_entry(child, 0, 'd', cursor, end);
            if (err) {
                f_closedir(&dir);
                return err;
            }
            err = fatfsjs_walk(child, cursor, end);
            if (err) {
                f_closedir(&dir);
                return err;
            }
        } else {
            int err = fatfsjs_emit_entry(child, info.fsize, 'f', cursor, end);
            if (err) {
                f_closedir(&dir);
                return err;
            }
        }
    }

    f_closedir(&dir);
    return 0;
}

static int fatfsjs_mkdirs(const char *path) {
    char buffer[FATFSJS_PATH_MAX];
    strncpy(buffer, path, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *cursor = buffer;
    if (*cursor == '/') {
        cursor++;
    }
    while (true) {
        char *slash = strchr(cursor, '/');
        if (!slash) {
            break;
        }
        *slash = '\0';
        if (*buffer != '\0') {
            FRESULT fr = f_mkdir(buffer);
            if (fr != FR_OK && fr != FR_EXIST) {
                *slash = '/';
                return fatfsjs_result(fr);
            }
        }
        *slash = '/';
        cursor = slash + 1;
    }
    return 0;
}

static int fatfsjs_copy_from_image(const uint8_t *image, uint32_t len) {
    size_t total = fatfsjs_total_bytes();
    if (!image || len != total) {
        return -(int)FR_INVALID_PARAMETER;
    }
    memcpy(g_storage, image, total);
    return 0;
}

// Public API ----------------------------------------------------------------

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init(uint32_t sector_size, uint32_t sector_count) {
    int err = fatfsjs_configure(sector_size, sector_count);
    if (err) {
        return err;
    }
    err = fatfsjs_mount(true);
    if (err) {
        fatfsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init_from_image(uint32_t sector_size, uint32_t sector_count,
                            const uint8_t *image, uint32_t image_len) {
    int err = fatfsjs_configure(sector_size, sector_count);
    if (err) {
        return err;
    }
    err = fatfsjs_copy_from_image(image, image_len);
    if (err) {
        fatfsjs_release();
        return err;
    }
    err = fatfsjs_mount(false);
    if (err) {
        fatfsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_format(void) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    FRESULT fr = fatfsjs_makefs();
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }
    fr = f_mount(&g_fs, "", 1);
    g_is_mounted = (fr == FR_OK);
    return fatfsjs_result(fr);
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_write_file(const char *path, const uint8_t *data, uint32_t length) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || (!data && length > 0)) {
        return -(int)FR_INVALID_PARAMETER;
    }

    err = fatfsjs_mkdirs(path);
    if (err) {
        return err;
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }

    UINT written_total = 0;
    while (written_total < length) {
        UINT chunk = length - written_total;
        UINT bw = 0;
        fr = f_write(&file, data + written_total, chunk, &bw);
        if (fr != FR_OK) {
            f_close(&file);
            return fatfsjs_result(fr);
        }
        written_total += bw;
        if (bw == 0) {
            break;
        }
    }

    f_close(&file);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_delete_file(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return -(int)FR_INVALID_PARAMETER;
    }
    return fatfsjs_result(f_unlink(path));
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_file_size(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return -(int)FR_INVALID_PARAMETER;
    }

    FILINFO info;
    memset(&info, 0, sizeof(info));
    FRESULT fr = f_stat(path, &info);
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }
    if (info.fattrib & AM_DIR) {
        return -(int)FR_INVALID_NAME;
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
    if (!path || buffer_ptr == 0 || buffer_len == 0) {
        return -(int)FR_INVALID_PARAMETER;
    }

    FIL file;
    FRESULT fr = f_open(&file, path, FA_READ);
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }

    FSIZE_t size = f_size(&file);
    if ((uint32_t)size > buffer_len) {
        f_close(&file);
        return -(int)FR_INVALID_PARAMETER;
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)buffer_ptr;
    FSIZE_t remaining = size;
    while (remaining > 0) {
        UINT chunk = (UINT)(remaining > FATFSJS_MAX_READ_CHUNK
                                ? FATFSJS_MAX_READ_CHUNK
                                : remaining);
        UINT read_bytes = 0;
        fr = f_read(&file, dest + (size - remaining), chunk, &read_bytes);
        if (fr != FR_OK) {
            f_close(&file);
            return fatfsjs_result(fr);
        }
        remaining -= read_bytes;
        if (read_bytes == 0) {
            break;
        }
    }

    f_close(&file);
    return (int)size;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_list(const char *path, uint32_t buffer_ptr, uint32_t buffer_len) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (buffer_ptr == 0 || buffer_len == 0) {
        return -(int)FR_INVALID_PARAMETER;
    }

    const char *target = (path && path[0]) ? path : "/";
    char *cursor = (char *)(uintptr_t)buffer_ptr;
    const char *end = cursor + buffer_len;
    *cursor = '\0';

    FILINFO info;
    memset(&info, 0, sizeof(info));
    FRESULT fr = f_stat(target, &info);
    if (fr != FR_OK) {
        return fatfsjs_result(fr);
    }

    if (info.fattrib & AM_DIR) {
        err = fatfsjs_emit_entry(target, 0, 'd', &cursor, end);
        if (err) {
            return err;
        }
        err = fatfsjs_walk(target, &cursor, end);
        if (err) {
            return err;
        }
    } else {
        err = fatfsjs_emit_entry(target, info.fsize, 'f', &cursor, end);
        if (err) {
            return err;
        }
    }

    if (cursor < end) {
        *cursor = '\0';
    }
    return (int)(cursor - (char *)(uintptr_t)buffer_ptr);
}

EMSCRIPTEN_KEEPALIVE
uint32_t fatfsjs_storage_size(void) {
    return (uint32_t)fatfsjs_total_bytes();
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_export_image(uint32_t buffer_ptr, uint32_t buffer_len) {
    size_t total = fatfsjs_total_bytes();
    if (!g_storage || total == 0) {
        return -(int)FR_NOT_READY;
    }
    if (buffer_ptr == 0 || buffer_len < total) {
        return -(int)FR_INVALID_PARAMETER;
    }
    memcpy((void *)(uintptr_t)buffer_ptr, g_storage, total);
    return (int)total;
}
