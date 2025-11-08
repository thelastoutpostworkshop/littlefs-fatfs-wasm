#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>

#include "lfs.h"
#include "lfs_util.h"

#define LFSJS_PATH_MAX 512
#define LFSJS_DEFAULT_LOOKAHEAD 32

static lfs_t g_lfs;
static struct lfs_config g_cfg;
static uint8_t *g_storage = NULL;
static bool g_is_mounted = false;

static size_t lfsjs_total_bytes(const struct lfs_config *cfg);
static int lfsjs_mount_internal(bool allow_format);

static void lfsjs_release(void) {
    if (g_is_mounted) {
        lfs_unmount(&g_lfs);
        g_is_mounted = false;
    }
    if (g_storage) {
        free(g_storage);
        g_storage = NULL;
    }
}

static size_t lfsjs_total_bytes(const struct lfs_config *cfg) {
    return (size_t)cfg->block_size * cfg->block_count;
}

static size_t lfsjs_current_size(void) {
    if (!g_storage) {
        return 0;
    }
    return lfsjs_total_bytes(&g_cfg);
}

static void lfsjs_fill_erased(void) {
    if (g_storage) {
        memset(g_storage, 0xFF, lfsjs_total_bytes(&g_cfg));
    }
}

static int lfsjs_ram_read(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, void *buffer, lfs_size_t size) {
    size_t idx = (size_t)block * c->block_size + off;
    memcpy(buffer, &g_storage[idx], size);
    return 0;
}

static int lfsjs_ram_prog(const struct lfs_config *c, lfs_block_t block,
                          lfs_off_t off, const void *buffer, lfs_size_t size) {
    size_t idx = (size_t)block * c->block_size + off;
    memcpy(&g_storage[idx], buffer, size);
    return 0;
}

static int lfsjs_ram_erase(const struct lfs_config *c, lfs_block_t block) {
    size_t idx = (size_t)block * c->block_size;
    memset(&g_storage[idx], 0xFF, c->block_size);
    return 0;
}

static int lfsjs_ram_sync(const struct lfs_config *c) {
    (void)c;
    return 0;
}

static uint32_t lfsjs_choose_io_size(uint32_t block_size) {
    const uint32_t min_io = 16;
    return block_size < min_io ? block_size : min_io;
}

static uint32_t lfsjs_choose_lookahead(uint32_t requested) {
    uint32_t value = requested ? requested : LFSJS_DEFAULT_LOOKAHEAD;
    if (value < 16) {
        value = 16;
    }
    /* lookahead must be a multiple of 8 bytes */
    value = (value + 7u) & ~7u;
    return value;
}

static int lfsjs_configure(uint32_t block_size, uint32_t block_count,
                           uint32_t lookahead_size) {
    if (block_size == 0 || block_count == 0) {
        return LFS_ERR_INVAL;
    }

    lfsjs_release();
    memset(&g_cfg, 0, sizeof(g_cfg));

    g_cfg.read = lfsjs_ram_read;
    g_cfg.prog = lfsjs_ram_prog;
    g_cfg.erase = lfsjs_ram_erase;
    g_cfg.sync = lfsjs_ram_sync;

    uint32_t io_size = lfsjs_choose_io_size(block_size);
    g_cfg.read_size = io_size;
    g_cfg.prog_size = io_size;
    g_cfg.cache_size = block_size;
    g_cfg.block_size = block_size;
    g_cfg.block_count = block_count;
    g_cfg.block_cycles = 512;
    g_cfg.lookahead_size = lfsjs_choose_lookahead(lookahead_size);

    size_t total_bytes = lfsjs_total_bytes(&g_cfg);
    g_storage = (uint8_t *)malloc(total_bytes);
    if (!g_storage) {
        return LFS_ERR_NOMEM;
    }

    lfsjs_fill_erased();
    return 0;
}

static int lfsjs_mount_internal(bool allow_format) {
    int err = lfs_mount(&g_lfs, &g_cfg);
    if (err && allow_format) {
        err = lfs_format(&g_lfs, &g_cfg);
        if (err) {
            g_is_mounted = false;
            return err;
        }
        err = lfs_mount(&g_lfs, &g_cfg);
    }

    if (err == 0) {
        g_is_mounted = true;
    } else {
        g_is_mounted = false;
    }
    return err;
}

static int lfsjs_ensure_mounted(void) {
    if (!g_is_mounted) {
        return LFS_ERR_INVAL;
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_init(uint32_t block_size, uint32_t block_count,
               uint32_t lookahead_size) {
    int err = lfsjs_configure(block_size, block_count, lookahead_size);
    if (err) {
        return err;
    }

    err = lfsjs_mount_internal(true);
    if (err) {
        lfsjs_release();
    }
    return err;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_init_from_image(uint32_t block_size, uint32_t block_count,
                          uint32_t lookahead_size, const uint8_t *image,
                          uint32_t image_len) {
    int err = lfsjs_configure(block_size, block_count, lookahead_size);
    if (err) {
        return err;
    }

    size_t total = lfsjs_total_bytes(&g_cfg);
    if (!image || image_len != total) {
        lfsjs_release();
        return LFS_ERR_INVAL;
    }

    memcpy(g_storage, image, total);

    err = lfs_mount(&g_lfs, &g_cfg);
    if (err) {
        lfsjs_release();
        return err;
    }

    g_is_mounted = true;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_format(void) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }

    lfs_unmount(&g_lfs);
    g_is_mounted = false;

    err = lfs_format(&g_lfs, &g_cfg);
    if (err) {
        return err;
    }
    return lfsjs_mount_internal(false);
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_add_file(const char *path, const uint8_t *data, uint32_t length) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return LFS_ERR_INVAL;
    }

    lfs_file_t file;
    err = lfs_file_open(&g_lfs, &file, path,
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        return err;
    }

    if (length > 0 && data) {
        lfs_ssize_t written = lfs_file_write(&g_lfs, &file, data, length);
        if (written < 0) {
            lfs_file_close(&g_lfs, &file);
            return (int)written;
        }
        if ((uint32_t)written != length) {
            lfs_file_close(&g_lfs, &file);
            return LFS_ERR_IO;
        }
    }

    err = lfs_file_close(&g_lfs, &file);
    return err < 0 ? err : 0;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_delete_file(const char *path) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path) {
        return LFS_ERR_INVAL;
    }
    return lfs_remove(&g_lfs, path);
}

static int lfsjs_emit_file(const char *path, lfs_off_t size, char **cursor,
                           const char *end) {
    const char *display = path;
    if (display[0] == '/' && display[1] != '\0') {
        display = &display[1];
    }

    int needed = snprintf(NULL, 0, "%s\t%ld\n", display, (long)size);
    if (needed < 0) {
        return LFS_ERR_IO;
    }
    if (*cursor + needed + 1 > end) {
        return LFS_ERR_NOSPC;
    }

    int written = snprintf(*cursor, (size_t)(end - *cursor), "%s\t%ld\n",
                           display, (long)size);
    if (written != needed) {
        return LFS_ERR_IO;
    }
    *cursor += written;
    return 0;
}

static int lfsjs_join_path(const char *base, const char *leaf, char *out,
                           size_t out_len) {
    if (strcmp(base, "/") == 0) {
        if ((size_t)snprintf(out, out_len, "/%s", leaf) >= out_len) {
            return LFS_ERR_NAMETOOLONG;
        }
        return 0;
    }

    if ((size_t)snprintf(out, out_len, "%s/%s", base, leaf) >= out_len) {
        return LFS_ERR_NAMETOOLONG;
    }
    return 0;
}

static int lfsjs_walk(const char *dir, char **cursor, const char *end) {
    lfs_dir_t directory;
    struct lfs_info info;

    int err = lfs_dir_open(&g_lfs, &directory, dir);
    if (err < 0) {
        return err;
    }

    while (true) {
        int res = lfs_dir_read(&g_lfs, &directory, &info);
        if (res < 0) {
            lfs_dir_close(&g_lfs, &directory);
            return res;
        }
        if (res == 0) {
            break;
        }

        if (strcmp(info.name, ".") == 0 || strcmp(info.name, "..") == 0) {
            continue;
        }

        char path[LFSJS_PATH_MAX];
        err = lfsjs_join_path(dir, info.name, path, sizeof(path));
        if (err) {
            lfs_dir_close(&g_lfs, &directory);
            return err;
        }

        if (info.type == LFS_TYPE_DIR) {
            err = lfsjs_walk(path, cursor, end);
            if (err) {
                lfs_dir_close(&g_lfs, &directory);
                return err;
            }
        } else if (info.type == LFS_TYPE_REG) {
            err = lfsjs_emit_file(path, info.size, cursor, end);
            if (err) {
                lfs_dir_close(&g_lfs, &directory);
                return err;
            }
        }
    }

    lfs_dir_close(&g_lfs, &directory);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_list(uint32_t buffer_ptr, uint32_t buffer_len) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!buffer_ptr || !buffer_len) {
        return LFS_ERR_INVAL;
    }

    char *cursor = (char *)(uintptr_t)buffer_ptr;
    const char *end = cursor + buffer_len;
    *cursor = '\0';

    err = lfsjs_walk("/", &cursor, end);
    if (err) {
        return err;
    }

    if (cursor < end) {
        *cursor = '\0';
    }
    return (int)(cursor - (char *)(uintptr_t)buffer_ptr);
}

static int lfsjs_stat(const char *path, struct lfs_info *info) {
    if (!path || !info) {
        return LFS_ERR_INVAL;
    }
    return lfs_stat(&g_lfs, path, info);
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_file_size(const char *path) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }

    struct lfs_info info;
    err = lfsjs_stat(path, &info);
    if (err) {
        return err;
    }
    if (info.type != LFS_TYPE_REG) {
        return LFS_ERR_ISDIR;
    }
    return (int)info.size;
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_read_file(const char *path, uint32_t buffer_ptr, uint32_t buffer_len) {
    int err = lfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || buffer_ptr == 0 || buffer_len == 0) {
        return LFS_ERR_INVAL;
    }

    struct lfs_info info;
    err = lfsjs_stat(path, &info);
    if (err) {
        return err;
    }
    if (info.type != LFS_TYPE_REG) {
        return LFS_ERR_ISDIR;
    }
    if ((uint32_t)info.size > buffer_len) {
        return LFS_ERR_NOSPC;
    }

    lfs_file_t file;
    err = lfs_file_open(&g_lfs, &file, path, LFS_O_RDONLY);
    if (err < 0) {
        return err;
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)buffer_ptr;
    lfs_size_t remaining = info.size;
    while (remaining > 0) {
        lfs_size_t chunk = remaining;
        if (chunk > 4096) {
            chunk = 4096;
        }
        lfs_ssize_t read =
            lfs_file_read(&g_lfs, &file, dest + (info.size - remaining), chunk);
        if (read < 0) {
            lfs_file_close(&g_lfs, &file);
            return (int)read;
        }
        remaining -= (lfs_size_t)read;
    }

    lfs_file_close(&g_lfs, &file);
    return (int)info.size;
}

EMSCRIPTEN_KEEPALIVE
uint32_t lfsjs_storage_size(void) {
    return (uint32_t)lfsjs_current_size();
}

EMSCRIPTEN_KEEPALIVE
int lfsjs_export_image(uint32_t buffer_ptr, uint32_t buffer_len) {
    size_t total = lfsjs_current_size();
    if (!g_storage || total == 0) {
        return LFS_ERR_INVAL;
    }
    if (buffer_ptr == 0 || buffer_len < total) {
        return LFS_ERR_NOSPC;
    }

    memcpy((void *)(uintptr_t)buffer_ptr, g_storage, total);
    return (int)total;
}
