#include <emscripten/emscripten.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spiffs.h"

#define SPIFFSJS_PATH_MAX 512
#define SPIFFSJS_MAX_READ_CHUNK 4096
#define SPIFFSJS_DEFAULT_FD_COUNT 16
#define SPIFFSJS_DEFAULT_CACHE_PAGES 64

#define SPIFFSJS_MIN(a, b) ((a) < (b) ? (a) : (b))

static spiffs g_fs;
static spiffs_config g_cfg;
static bool g_is_mounted = false;
static bool g_disk_ready = false;
static uint8_t *g_storage = NULL;
static size_t g_total_bytes = 0;
static uint32_t g_total_bytes32 = 0;
static uint32_t g_page_size = 0;
static uint32_t g_block_size = 0;
static uint32_t g_block_count = 0;
static uint8_t *g_work = NULL;
static uint32_t g_work_size = 0;
static uint8_t *g_fd_space = NULL;
static uint32_t g_fd_space_size = 0;
static void *g_cache = NULL;
static uint32_t g_cache_size = 0;

static size_t spiffsjs_total_bytes(void) {
    return g_total_bytes;
}

static s32_t spiffsjs_result(s32_t err) {
    return err == SPIFFS_OK ? 0 : err;
}

static s32_t spiffsjs_hal_read(u32_t addr, u32_t size, u8_t *dst) {
    if (!g_storage || addr + size > g_total_bytes) {
        return SPIFFS_ERR_INTERNAL;
    }
    memcpy(dst, g_storage + addr, size);
    return SPIFFS_OK;
}

static s32_t spiffsjs_hal_write(u32_t addr, u32_t size, u8_t *src) {
    if (!g_storage || addr + size > g_total_bytes) {
        return SPIFFS_ERR_INTERNAL;
    }
    memcpy(g_storage + addr, src, size);
    return SPIFFS_OK;
}

static s32_t spiffsjs_hal_erase(u32_t addr, u32_t size) {
    if (!g_storage || addr + size > g_total_bytes) {
        return SPIFFS_ERR_INTERNAL;
    }
    memset(g_storage + addr, 0xFF, size);
    return SPIFFS_OK;
}

static void spiffsjs_release(void) {
    if (g_is_mounted) {
        SPIFFS_unmount(&g_fs);
        g_is_mounted = false;
    }
    free(g_work);
    g_work = NULL;
    free(g_fd_space);
    g_fd_space = NULL;
    free(g_cache);
    g_cache = NULL;
    free(g_storage);
    g_storage = NULL;
    g_total_bytes = 0;
    g_total_bytes32 = 0;
    g_page_size = 0;
    g_block_size = 0;
    g_block_count = 0;
    g_disk_ready = false;
    g_work_size = 0;
    g_fd_space_size = 0;
    g_cache_size = 0;
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_cfg, 0, sizeof(g_cfg));
}

static int spiffsjs_configure(uint32_t page_size, uint32_t block_size,
                             uint32_t block_count, uint32_t fd_count,
                             uint32_t cache_pages) {
    if (page_size == 0 || block_size == 0 || block_count == 0) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    if (block_size < page_size || (block_size % page_size) != 0) {
        return SPIFFS_ERR_INTERNAL;
    }
    if (block_size < page_size * 8) {
        return SPIFFS_ERR_INTERNAL;
    }
    uint64_t total = (uint64_t)block_size * block_count;
    if (total == 0 || total > UINT32_MAX) {
        return SPIFFS_ERR_INTERNAL;
    }

    spiffsjs_release();

    g_storage = (uint8_t *)malloc((size_t)total);
    if (!g_storage) {
        return SPIFFS_ERR_INTERNAL;
    }
    memset(g_storage, 0xFF, (size_t)total);

    g_total_bytes = (size_t)total;
    g_total_bytes32 = (uint32_t)total;
    g_page_size = page_size;
    g_block_size = block_size;
    g_block_count = block_count;

    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.hal_read_f = spiffsjs_hal_read;
    g_cfg.hal_write_f = spiffsjs_hal_write;
    g_cfg.hal_erase_f = spiffsjs_hal_erase;
    g_cfg.phys_size = g_total_bytes32;
    g_cfg.phys_addr = 0;
    g_cfg.phys_erase_block = block_size;
    g_cfg.log_block_size = block_size;
    g_cfg.log_page_size = page_size;

#if SPIFFS_FILEHDL_OFFSET
    g_cfg.fh_ix_offset = 0;
#endif

    memset(&g_fs, 0, sizeof(g_fs));
    memcpy(&g_fs.cfg, &g_cfg, sizeof(g_cfg));

    g_work_size = page_size * 2;
    g_work = (uint8_t *)malloc(g_work_size);
    if (!g_work) {
        spiffsjs_release();
        return SPIFFS_ERR_INTERNAL;
    }

    g_fd_space_size = SPIFFS_buffer_bytes_for_filedescs(&g_fs, fd_count);
    if (g_fd_space_size == 0) {
        spiffsjs_release();
        return SPIFFS_ERR_INTERNAL;
    }
    g_fd_space = (uint8_t *)malloc(g_fd_space_size + sizeof(void *));
    if (!g_fd_space) {
        spiffsjs_release();
        return SPIFFS_ERR_INTERNAL;
    }
    memset(g_fd_space, 0, g_fd_space_size + sizeof(void *));

#if SPIFFS_CACHE
    g_cache_size = SPIFFS_buffer_bytes_for_cache(&g_fs, cache_pages);
    if (g_cache_size > 0) {
        g_cache = malloc(g_cache_size + sizeof(void *));
        if (!g_cache) {
            spiffsjs_release();
            return SPIFFS_ERR_INTERNAL;
        }
        memset(g_cache, 0, g_cache_size + sizeof(void *));
    }
#else
    g_cache = NULL;
    g_cache_size = 0;
#endif

    g_disk_ready = true;
    return 0;
}

static int spiffsjs_mount(bool allow_format) {
    if (!g_disk_ready) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    s32_t res = SPIFFS_mount(&g_fs, &g_cfg, g_work, g_fd_space, g_fd_space_size,
                             g_cache, g_cache_size, NULL);
    if (res != SPIFFS_OK && allow_format) {
        SPIFFS_unmount(&g_fs);
        res = SPIFFS_format(&g_fs);
        if (res == SPIFFS_OK) {
            res = SPIFFS_mount(&g_fs, &g_cfg, g_work, g_fd_space, g_fd_space_size,
                               g_cache, g_cache_size, NULL);
        }
    }
    g_is_mounted = (res == SPIFFS_OK);
    return spiffsjs_result(res);
}

static int spiffsjs_ensure_mounted(void) {
    if (!g_is_mounted) {
        return SPIFFS_ERR_NOT_MOUNTED;
    }
    return 0;
}

static int spiffsjs_emit_entry(const struct spiffs_dirent *entry, char **cursor,
                              const char *end) {
    const char *type = entry->type == SPIFFS_TYPE_DIR ? "dir" : "file";
    int needed =
        snprintf(NULL, 0, "%s\t%s\t%lu\n", entry->name, type,
                 (unsigned long)entry->size);
    if (needed < 0) {
        return SPIFFS_ERR_INTERNAL;
    }
    if (*cursor + needed + 1 > end) {
        return SPIFFS_ERR_INTERNAL;
    }
    int written =
        snprintf(*cursor, (size_t)(end - *cursor), "%s\t%s\t%lu\n", entry->name,
                 type, (unsigned long)entry->size);
    if (written != needed) {
        return SPIFFS_ERR_INTERNAL;
    }
    *cursor += written;
    return 0;
}

static int spiffsjs_list_inner(uint32_t buffer_ptr, uint32_t buffer_len) {
    if (!g_is_mounted) {
        return SPIFFS_ERR_NOT_MOUNTED;
    }
    if (buffer_ptr == 0 || buffer_len == 0) {
        return SPIFFS_ERR_INTERNAL;
    }

    char *cursor = (char *)(uintptr_t)buffer_ptr;
    const char *end = cursor + buffer_len;
    *cursor = '\0';

    spiffs_DIR dir;
    if (!SPIFFS_opendir(&g_fs, "/", &dir)) {
        return SPIFFS_ERR_NOT_MOUNTED;
    }

    struct spiffs_dirent entry;
    while (true) {
        struct spiffs_dirent *result = SPIFFS_readdir(&dir, &entry);
        if (!result) {
            break;
        }
        int err = spiffsjs_emit_entry(result, &cursor, end);
        if (err) {
            SPIFFS_closedir(&dir);
            return err;
        }
    }

    SPIFFS_closedir(&dir);
    if (cursor < end) {
        *cursor = '\0';
    }
    return (int)(cursor - (char *)(uintptr_t)buffer_ptr);
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_init(uint32_t page_size, uint32_t block_size, uint32_t block_count,
                  uint32_t fd_count, uint32_t cache_pages) {
    int err = spiffsjs_configure(page_size, block_size, block_count, fd_count,
                                cache_pages);
    if (err) {
        return err;
    }
    err = spiffsjs_mount(true);
    if (err) {
        spiffsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_init_from_image(uint32_t page_size, uint32_t block_size,
                             uint32_t block_count, uint32_t fd_count,
                             uint32_t cache_pages, const uint8_t *image,
                             uint32_t image_len) {
    int err = spiffsjs_configure(page_size, block_size, block_count, fd_count,
                                cache_pages);
    if (err) {
        return err;
    }
    if (!image || image_len != g_total_bytes) {
        spiffsjs_release();
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    memcpy(g_storage, image, g_total_bytes);

    err = spiffsjs_mount(false);
    if (err) {
        spiffsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_format(void) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    SPIFFS_unmount(&g_fs);
    err = SPIFFS_format(&g_fs);
    if (err != SPIFFS_OK) {
        return err;
    }
    memset(g_storage, 0xFF, g_total_bytes);
    err = spiffsjs_mount(false);
    if (err) {
        spiffsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_file_size(const char *path) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    spiffs_stat info;
    s32_t res = SPIFFS_stat(&g_fs, path, &info);
    if (res != SPIFFS_OK) {
        return res;
    }
    if (info.type == SPIFFS_TYPE_DIR) {
        return SPIFFS_ERR_NOT_A_FILE;
    }
    return (int)info.size;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_read_file(const char *path, uint32_t buffer_ptr,
                       uint32_t buffer_len) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || buffer_ptr == 0 || buffer_len == 0) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }

    spiffs_stat info;
    s32_t res = SPIFFS_stat(&g_fs, path, &info);
    if (res != SPIFFS_OK) {
        return res;
    }
    if (info.type == SPIFFS_TYPE_DIR) {
        return SPIFFS_ERR_NOT_A_FILE;
    }
    u32_t size = info.size;
    if (size > buffer_len) {
        return SPIFFS_ERR_INTERNAL;
    }

    spiffs_file file = SPIFFS_open(&g_fs, path, SPIFFS_RDONLY, 0);
    if (file < 0) {
        return file;
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)buffer_ptr;
    u32_t remaining = size;
    while (remaining > 0) {
        s32_t chunk =
            (s32_t)SPIFFSJS_MIN((u32_t)SPIFFSJS_MAX_READ_CHUNK, remaining);
        s32_t read = SPIFFS_read(&g_fs, file, dest + (size - remaining), chunk);
        if (read < 0) {
            SPIFFS_close(&g_fs, file);
            return read;
        }
        if (read == 0) {
            break;
        }
        remaining -= (u32_t)read;
    }

    SPIFFS_close(&g_fs, file);
    return (int)size;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_write_file(const char *path, const uint8_t *data,
                        uint32_t length) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || (length > 0 && !data)) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }

    spiffs_file file =
        SPIFFS_open(&g_fs, path, SPIFFS_CREAT | SPIFFS_TRUNC | SPIFFS_RDWR, 0);
    if (file < 0) {
        return file;
    }

    uint32_t written = 0;
    while (written < length) {
        uint32_t chunk =
            SPIFFSJS_MIN((uint32_t)SPIFFSJS_MAX_READ_CHUNK, length - written);
        s32_t res = SPIFFS_write(&g_fs, file, (void *)(uintptr_t)(data + written),
                                 (s32_t)chunk);
        if (res < 0) {
            SPIFFS_close(&g_fs, file);
            return res;
        }
        written += (uint32_t)res;
    }

    SPIFFS_close(&g_fs, file);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_remove_file(const char *path) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    return SPIFFS_remove(&g_fs, path);
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_list(uint32_t buffer_ptr, uint32_t buffer_len) {
    return spiffsjs_list_inner(buffer_ptr, buffer_len);
}

EMSCRIPTEN_KEEPALIVE
uint32_t spiffsjs_storage_size(void) {
    return g_total_bytes32;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_export_image(uint32_t buffer_ptr, uint32_t buffer_len) {
    size_t total = spiffsjs_total_bytes();
    if (!g_storage || total == 0) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    if (!buffer_ptr || buffer_len < total) {
        return SPIFFS_ERR_INTERNAL;
    }
    memcpy((void *)(uintptr_t)buffer_ptr, g_storage, total);
    return (int)total;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_get_usage(uint32_t usage_ptr) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!usage_ptr) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    u32_t total = 0;
    u32_t used = 0;
    s32_t res = SPIFFS_info(&g_fs, &total, &used);
    if (res != SPIFFS_OK) {
        return res;
    }
    u32_t free_bytes = (total > used) ? (total - used) : 0;
    u32_t *dest = (u32_t *)(uintptr_t)usage_ptr;
    dest[0] = total;
    dest[1] = used;
    dest[2] = free_bytes;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int spiffsjs_can_fit(const char *path, uint32_t length) {
    int err = spiffsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return SPIFFS_ERR_NOT_CONFIGURED;
    }
    u32_t total = 0;
    u32_t used = 0;
    s32_t res = SPIFFS_info(&g_fs, &total, &used);
    if (res != SPIFFS_OK) {
        return res;
    }
    u32_t free_bytes = (total > used) ? (total - used) : 0;
    return length <= free_bytes ? 1 : 0;
}
