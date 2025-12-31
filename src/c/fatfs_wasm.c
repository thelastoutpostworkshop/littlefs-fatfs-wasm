#include <ctype.h>
#include <emscripten/emscripten.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FATFSJS_BYTES_PER_SECTOR 4096
#define FATFSJS_ROOT_DIR_SECTOR 4
#define FATFSJS_DATA_START_SECTOR 8
#define FATFSJS_DIR_ENTRY_SIZE 32
#define FATFSJS_MAX_NAME 64
#define FATFSJS_FAT16_EOC 0xFFF8

#define FATFSJS_ERR_INVAL -1
#define FATFSJS_ERR_NOT_MOUNTED -2
#define FATFSJS_ERR_NOT_FOUND -3
#define FATFSJS_ERR_NOT_A_FILE -4
#define FATFSJS_ERR_NOSPC -5
#define FATFSJS_ERR_UNSUPPORTED -6
#define FATFSJS_ERR_IO -7

typedef struct {
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t fat_size_sectors;
    uint32_t total_sectors;
    uint32_t fat_start_sector;
    uint32_t root_dir_sector;
    uint32_t root_dir_sectors;
    uint32_t data_start_sector;
    uint32_t cluster_count;
} fatfs_layout_t;

typedef struct {
    char name[FATFSJS_MAX_NAME];
    uint32_t size;
    uint16_t first_cluster;
    bool is_dir;
} fatfs_dirent_t;

typedef struct {
    bool is_root;
    uint16_t start_cluster;
} fatfs_dir_t;

static fatfs_layout_t g_layout;
static uint8_t *g_storage = NULL;
static uint32_t g_storage_len = 0;
static bool g_is_mounted = false;

static int fatfsjs_read_fat(uint16_t cluster, uint16_t *value);

static uint16_t fatfsjs_read_u16(const uint8_t *ptr) {
    return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8);
}

static uint32_t fatfsjs_read_u32(const uint8_t *ptr) {
    return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
}

static void fatfsjs_release(void) {
    free(g_storage);
    g_storage = NULL;
    g_storage_len = 0;
    g_is_mounted = false;
    memset(&g_layout, 0, sizeof(g_layout));
}

static int fatfsjs_parse_layout(const uint8_t *image, uint32_t image_len,
                                fatfs_layout_t *out) {
    if (!image || !out) {
        return FATFSJS_ERR_INVAL;
    }
    if (image_len < 64) {
        return FATFSJS_ERR_INVAL;
    }

    uint16_t bytes_per_sector = fatfsjs_read_u16(image + 11);
    uint8_t sectors_per_cluster = image[13];
    uint16_t reserved_sectors = fatfsjs_read_u16(image + 14);
    uint8_t num_fats = image[16];
    uint16_t fat_size_sectors = fatfsjs_read_u16(image + 22);

    if (bytes_per_sector != FATFSJS_BYTES_PER_SECTOR) {
        return FATFSJS_ERR_INVAL;
    }
    if (sectors_per_cluster == 0 || reserved_sectors == 0 || num_fats == 0 ||
        fat_size_sectors == 0) {
        return FATFSJS_ERR_INVAL;
    }

    if (image_len % bytes_per_sector != 0) {
        return FATFSJS_ERR_INVAL;
    }
    uint32_t total_sectors = image_len / bytes_per_sector;
    if (total_sectors == 0) {
        return FATFSJS_ERR_INVAL;
    }

    uint32_t fat_start_sector = reserved_sectors;
    uint32_t root_dir_sector = FATFSJS_ROOT_DIR_SECTOR;
    uint32_t data_start_sector = FATFSJS_DATA_START_SECTOR;
    if (root_dir_sector >= data_start_sector) {
        return FATFSJS_ERR_INVAL;
    }
    if (fat_start_sector + fat_size_sectors > root_dir_sector) {
        return FATFSJS_ERR_INVAL;
    }
    if (data_start_sector >= total_sectors) {
        return FATFSJS_ERR_INVAL;
    }

    uint32_t root_dir_sectors = data_start_sector - root_dir_sector;
    uint32_t root_entry_count =
        (root_dir_sectors * bytes_per_sector) / FATFSJS_DIR_ENTRY_SIZE;
    if (root_dir_sectors == 0 || root_entry_count == 0) {
        return FATFSJS_ERR_INVAL;
    }

    uint32_t data_sectors = total_sectors - data_start_sector;
    uint32_t cluster_count = data_sectors / sectors_per_cluster;

    out->bytes_per_sector = bytes_per_sector;
    out->sectors_per_cluster = sectors_per_cluster;
    out->reserved_sectors = reserved_sectors;
    out->num_fats = num_fats;
    out->root_entry_count = root_entry_count;
    out->fat_size_sectors = fat_size_sectors;
    out->total_sectors = total_sectors;
    out->fat_start_sector = fat_start_sector;
    out->root_dir_sector = root_dir_sector;
    out->root_dir_sectors = root_dir_sectors;
    out->data_start_sector = data_start_sector;
    out->cluster_count = cluster_count;
    return 0;
}

static int fatfsjs_ensure_mounted(void) {
    if (!g_is_mounted) {
        return FATFSJS_ERR_NOT_MOUNTED;
    }
    return 0;
}

static bool fatfsjs_starts_with_ci(const char *value, const char *prefix) {
    while (*prefix) {
        if (*value == '\0') {
            return false;
        }
        if (toupper((unsigned char)*value) != toupper((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static const char *fatfsjs_strip_mount(const char *path) {
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

static int fatfsjs_next_segment(const char **cursor, char *out,
                                size_t out_len) {
    if (!cursor || !out || out_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    const char *ptr = *cursor;
    while (*ptr == '/' || *ptr == '\\') {
        ptr++;
    }
    if (*ptr == '\0') {
        *cursor = ptr;
        return 0;
    }

    size_t len = 0;
    while (*ptr && *ptr != '/' && *ptr != '\\') {
        if (len + 1 >= out_len) {
            return FATFSJS_ERR_INVAL;
        }
        out[len++] = (char)toupper((unsigned char)*ptr);
        ptr++;
    }
    out[len] = '\0';
    *cursor = ptr;
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        return FATFSJS_ERR_UNSUPPORTED;
    }
    return 1;
}

static void fatfsjs_trim_spaces(char *value) {
    if (!value) {
        return;
    }
    size_t len = strlen(value);
    while (len > 0 && value[len - 1] == ' ') {
        value[len - 1] = '\0';
        len--;
    }
}

static void fatfsjs_parse_short_name(const uint8_t *entry, char *out,
                                     size_t out_len) {
    char name[9];
    char ext[4];
    memcpy(name, entry, 8);
    name[8] = '\0';
    memcpy(ext, entry + 8, 3);
    ext[3] = '\0';
    fatfsjs_trim_spaces(name);
    fatfsjs_trim_spaces(ext);
    if (ext[0] != '\0') {
        snprintf(out, out_len, "%s.%s", name, ext);
    } else {
        snprintf(out, out_len, "%s", name);
    }
}

static int fatfsjs_compare_ci(const char *a, const char *b) {
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

static int fatfsjs_decode_entry(const uint8_t *entry, fatfs_dirent_t *out,
                                bool *is_end) {
    uint8_t first = entry[0];
    if (first == 0x00) {
        if (is_end) {
            *is_end = true;
        }
        return 0;
    }
    if (first == 0xE5) {
        if (is_end) {
            *is_end = false;
        }
        return 1;
    }

    uint8_t attr = entry[11];
    if (attr == 0x0F || (attr & 0x08)) {
        if (is_end) {
            *is_end = false;
        }
        return 1;
    }

    char entry_name[FATFSJS_MAX_NAME];
    fatfsjs_parse_short_name(entry, entry_name, sizeof(entry_name));
    if (entry_name[0] == '\0') {
        if (is_end) {
            *is_end = false;
        }
        return 1;
    }
    if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0) {
        if (is_end) {
            *is_end = false;
        }
        return 1;
    }

    if (out) {
        out->size = fatfsjs_read_u32(entry + 28);
        out->first_cluster = fatfsjs_read_u16(entry + 26);
        out->is_dir = (attr & 0x10) != 0;
        strncpy(out->name, entry_name, sizeof(out->name));
        out->name[sizeof(out->name) - 1] = '\0';
    }

    if (is_end) {
        *is_end = false;
    }
    return 2;
}

typedef int (*fatfsjs_entry_cb)(const fatfs_dirent_t *entry, void *ctx);

static int fatfsjs_iterate_root(fatfsjs_entry_cb cb, void *ctx) {
    uint64_t root_offset =
        (uint64_t)g_layout.root_dir_sector * g_layout.bytes_per_sector;
    uint64_t root_bytes =
        (uint64_t)g_layout.root_dir_sectors * g_layout.bytes_per_sector;
    if (root_offset + root_bytes > g_storage_len) {
        return FATFSJS_ERR_IO;
    }

    uint32_t entries = g_layout.root_entry_count;
    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *entry =
            g_storage + root_offset + (uint64_t)i * FATFSJS_DIR_ENTRY_SIZE;
        fatfs_dirent_t decoded;
        bool is_end = false;
        int status = fatfsjs_decode_entry(entry, &decoded, &is_end);
        if (is_end) {
            return 0;
        }
        if (status != 2) {
            continue;
        }
        int res = cb(&decoded, ctx);
        if (res != 0) {
            return res;
        }
    }
    return 0;
}

static int fatfsjs_iterate_cluster_dir(uint16_t start_cluster,
                                       fatfsjs_entry_cb cb, void *ctx) {
    if (start_cluster < 2) {
        return FATFSJS_ERR_IO;
    }

    uint32_t cluster_size =
        (uint32_t)g_layout.bytes_per_sector * g_layout.sectors_per_cluster;
    if (cluster_size == 0 || (cluster_size % FATFSJS_DIR_ENTRY_SIZE) != 0) {
        return FATFSJS_ERR_IO;
    }
    uint32_t entries_per_cluster = cluster_size / FATFSJS_DIR_ENTRY_SIZE;

    uint16_t cluster = start_cluster;
    uint32_t guard = g_layout.cluster_count + 1;

    while (true) {
        if (cluster < 2 || guard == 0) {
            return FATFSJS_ERR_IO;
        }

        uint64_t sector_index =
            g_layout.data_start_sector +
            (uint64_t)(cluster - 2) * g_layout.sectors_per_cluster;
        uint64_t offset = sector_index * g_layout.bytes_per_sector;
        if (offset + cluster_size > g_storage_len) {
            return FATFSJS_ERR_IO;
        }

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            const uint8_t *entry =
                g_storage + offset + (uint64_t)i * FATFSJS_DIR_ENTRY_SIZE;
            fatfs_dirent_t decoded;
            bool is_end = false;
            int status = fatfsjs_decode_entry(entry, &decoded, &is_end);
            if (is_end) {
                return 0;
            }
            if (status != 2) {
                continue;
            }
            int res = cb(&decoded, ctx);
            if (res != 0) {
                return res;
            }
        }

        uint16_t next = 0;
        int err = fatfsjs_read_fat(cluster, &next);
        if (err) {
            return err;
        }
        if (next >= FATFSJS_FAT16_EOC || next < 2) {
            break;
        }
        cluster = next;
        guard--;
    }
    return 0;
}

static int fatfsjs_iterate_directory(const fatfs_dir_t *dir,
                                     fatfsjs_entry_cb cb, void *ctx) {
    if (!dir) {
        return FATFSJS_ERR_INVAL;
    }
    if (dir->is_root) {
        return fatfsjs_iterate_root(cb, ctx);
    }
    return fatfsjs_iterate_cluster_dir(dir->start_cluster, cb, ctx);
}

struct fatfsjs_find_ctx {
    const char *name;
    fatfs_dirent_t *out;
};

static int fatfsjs_find_cb(const fatfs_dirent_t *entry, void *ctx) {
    struct fatfsjs_find_ctx *state = (struct fatfsjs_find_ctx *)ctx;
    if (fatfsjs_compare_ci(state->name, entry->name) == 0) {
        *state->out = *entry;
        return 1;
    }
    return 0;
}

static int fatfsjs_find_in_dir(const fatfs_dir_t *dir, const char *name,
                               fatfs_dirent_t *out) {
    if (!dir || !name || !out) {
        return FATFSJS_ERR_INVAL;
    }
    struct fatfsjs_find_ctx ctx = { name, out };
    int res = fatfsjs_iterate_directory(dir, fatfsjs_find_cb, &ctx);
    if (res == 1) {
        return 0;
    }
    if (res < 0) {
        return res;
    }
    return FATFSJS_ERR_NOT_FOUND;
}

static int fatfsjs_open_dir(const char *path, fatfs_dir_t *out_dir) {
    if (!out_dir) {
        return FATFSJS_ERR_INVAL;
    }
    const char *cursor = fatfsjs_strip_mount(path);
    if (*cursor == '\0') {
        out_dir->is_root = true;
        out_dir->start_cluster = 0;
        return 0;
    }

    fatfs_dir_t current = { true, 0 };
    while (true) {
        char segment[FATFSJS_MAX_NAME];
        int seg = fatfsjs_next_segment(&cursor, segment, sizeof(segment));
        if (seg < 0) {
            return seg;
        }
        if (seg == 0) {
            return FATFSJS_ERR_INVAL;
        }

        fatfs_dirent_t entry;
        int err = fatfsjs_find_in_dir(&current, segment, &entry);
        if (err) {
            return err;
        }

        const char *next = cursor;
        while (*next == '/' || *next == '\\') {
            next++;
        }
        if (*next == '\0') {
            if (!entry.is_dir || entry.first_cluster < 2) {
                return FATFSJS_ERR_UNSUPPORTED;
            }
            out_dir->is_root = false;
            out_dir->start_cluster = entry.first_cluster;
            return 0;
        }

        if (!entry.is_dir || entry.first_cluster < 2) {
            return FATFSJS_ERR_UNSUPPORTED;
        }
        current.is_root = false;
        current.start_cluster = entry.first_cluster;
        cursor = next;
    }
}

static int fatfsjs_open_entry(const char *path, fatfs_dirent_t *out) {
    if (!out) {
        return FATFSJS_ERR_INVAL;
    }
    const char *cursor = fatfsjs_strip_mount(path);
    if (*cursor == '\0') {
        return FATFSJS_ERR_INVAL;
    }

    fatfs_dir_t current = { true, 0 };
    while (true) {
        char segment[FATFSJS_MAX_NAME];
        int seg = fatfsjs_next_segment(&cursor, segment, sizeof(segment));
        if (seg < 0) {
            return seg;
        }
        if (seg == 0) {
            return FATFSJS_ERR_INVAL;
        }

        fatfs_dirent_t entry;
        int err = fatfsjs_find_in_dir(&current, segment, &entry);
        if (err) {
            return err;
        }

        const char *next = cursor;
        while (*next == '/' || *next == '\\') {
            next++;
        }
        if (*next == '\0') {
            *out = entry;
            return 0;
        }

        if (!entry.is_dir || entry.first_cluster < 2) {
            return FATFSJS_ERR_UNSUPPORTED;
        }
        current.is_root = false;
        current.start_cluster = entry.first_cluster;
        cursor = next;
    }
}

static int fatfsjs_emit_entry(const char *name, uint32_t size, char type,
                              char **cursor, const char *end) {
    int needed = snprintf(NULL, 0, "%s\t%lu\t%c\n", name,
                          (unsigned long)size, type);
    if (needed < 0) {
        return FATFSJS_ERR_IO;
    }
    if (*cursor + needed + 1 > end) {
        return FATFSJS_ERR_NOSPC;
    }
    int written = snprintf(*cursor, (size_t)(end - *cursor), "%s\t%lu\t%c\n",
                           name, (unsigned long)size, type);
    if (written != needed) {
        return FATFSJS_ERR_IO;
    }
    *cursor += written;
    return 0;
}

static int fatfsjs_read_fat(uint16_t cluster, uint16_t *value) {
    if (!value) {
        return FATFSJS_ERR_INVAL;
    }
    uint32_t fat_bytes =
        g_layout.fat_size_sectors * g_layout.bytes_per_sector;
    uint32_t fat_offset = (uint32_t)cluster * 2;
    if (fat_offset + 1 >= fat_bytes) {
        return FATFSJS_ERR_IO;
    }
    uint32_t fat_start =
        g_layout.fat_start_sector * g_layout.bytes_per_sector;
    uint32_t absolute = fat_start + fat_offset;
    if (absolute + 1 >= g_storage_len) {
        return FATFSJS_ERR_IO;
    }
    *value = fatfsjs_read_u16(g_storage + absolute);
    return 0;
}

static int fatfsjs_copy_file(const fatfs_dirent_t *entry, uint8_t *dest,
                             uint32_t buffer_len) {
    if (!entry || !dest) {
        return FATFSJS_ERR_INVAL;
    }
    if (entry->size == 0) {
        return 0;
    }
    if (entry->size > buffer_len) {
        return FATFSJS_ERR_NOSPC;
    }
    if (entry->first_cluster < 2) {
        return FATFSJS_ERR_IO;
    }

    uint32_t cluster_size =
        (uint32_t)g_layout.bytes_per_sector * g_layout.sectors_per_cluster;
    uint32_t remaining = entry->size;
    uint32_t written = 0;
    uint16_t cluster = entry->first_cluster;
    uint32_t guard = g_layout.cluster_count + 1;

    while (remaining > 0) {
        if (cluster < 2 || guard == 0) {
            return FATFSJS_ERR_IO;
        }
        uint64_t sector_index =
            g_layout.data_start_sector +
            (uint64_t)(cluster - 2) * g_layout.sectors_per_cluster;
        uint64_t offset = sector_index * g_layout.bytes_per_sector;
        if (offset + cluster_size > g_storage_len) {
            return FATFSJS_ERR_IO;
        }

        uint32_t to_copy =
            remaining < cluster_size ? remaining : cluster_size;
        memcpy(dest + written, g_storage + offset, to_copy);
        remaining -= to_copy;
        written += to_copy;
        if (remaining == 0) {
            break;
        }

        uint16_t next = 0;
        int err = fatfsjs_read_fat(cluster, &next);
        if (err) {
            return err;
        }
        if (next >= FATFSJS_FAT16_EOC || next < 2) {
            return FATFSJS_ERR_IO;
        }
        cluster = next;
        guard--;
    }

    return (int)entry->size;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init(uint32_t block_size, uint32_t block_count) {
    (void)block_size;
    (void)block_count;
    return FATFSJS_ERR_UNSUPPORTED;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_init_from_image(const uint8_t *image, uint32_t image_len) {
    if (!image || image_len == 0) {
        return FATFSJS_ERR_INVAL;
    }

    fatfs_layout_t layout;
    int err = fatfsjs_parse_layout(image, image_len, &layout);
    if (err) {
        return err;
    }

    fatfsjs_release();
    g_storage = (uint8_t *)malloc(image_len);
    if (!g_storage) {
        return FATFSJS_ERR_IO;
    }
    memcpy(g_storage, image, image_len);
    g_storage_len = image_len;
    g_layout = layout;
    g_is_mounted = true;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_format(void) {
    return FATFSJS_ERR_UNSUPPORTED;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_write_file(const char *path, const uint8_t *data, uint32_t length) {
    (void)path;
    (void)data;
    (void)length;
    return FATFSJS_ERR_UNSUPPORTED;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_delete_file(const char *path) {
    (void)path;
    return FATFSJS_ERR_UNSUPPORTED;
}

struct fatfsjs_list_ctx {
    char *cursor;
    const char *end;
};

static int fatfsjs_list_cb(const fatfs_dirent_t *entry, void *ctx) {
    struct fatfsjs_list_ctx *state = (struct fatfsjs_list_ctx *)ctx;
    char type = entry->is_dir ? 'd' : 'f';
    return fatfsjs_emit_entry(entry->name, entry->size, type, &state->cursor,
                              state->end);
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

    fatfs_dir_t dir;
    err = fatfsjs_open_dir(path, &dir);
    if (err) {
        return err;
    }

    struct fatfsjs_list_ctx state;
    state.cursor = (char *)(uintptr_t)buffer_ptr;
    state.end = state.cursor + buffer_len;
    *state.cursor = '\0';

    err = fatfsjs_iterate_directory(&dir, fatfsjs_list_cb, &state);
    if (err) {
        return err;
    }

    if (state.cursor < state.end) {
        *state.cursor = '\0';
    }
    return (int)(state.cursor - (char *)(uintptr_t)buffer_ptr);
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_file_size(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    fatfs_dirent_t entry;
    err = fatfsjs_open_entry(path, &entry);
    if (err) {
        return err;
    }
    if (entry.is_dir) {
        return FATFSJS_ERR_NOT_A_FILE;
    }
    return (int)entry.size;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_read_file(const char *path, uint32_t buffer_ptr,
                      uint32_t buffer_len) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!path || !buffer_ptr || buffer_len == 0) {
        return FATFSJS_ERR_INVAL;
    }

    fatfs_dirent_t entry;
    err = fatfsjs_open_entry(path, &entry);
    if (err) {
        return err;
    }
    if (entry.is_dir) {
        return FATFSJS_ERR_NOT_A_FILE;
    }

    uint8_t *dest = (uint8_t *)(uintptr_t)buffer_ptr;
    return fatfsjs_copy_file(&entry, dest, buffer_len);
}

EMSCRIPTEN_KEEPALIVE
uint32_t fatfsjs_storage_size(void) {
    return g_storage_len;
}

EMSCRIPTEN_KEEPALIVE
int fatfsjs_export_image(uint32_t buffer_ptr, uint32_t buffer_len) {
    if (!g_storage || g_storage_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    if (!buffer_ptr || buffer_len < g_storage_len) {
        return FATFSJS_ERR_NOSPC;
    }
    memcpy((void *)(uintptr_t)buffer_ptr, g_storage, g_storage_len);
    return (int)g_storage_len;
}
