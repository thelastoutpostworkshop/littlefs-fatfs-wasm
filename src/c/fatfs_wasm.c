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

static fatfs_layout_t g_layout;
static uint8_t *g_storage = NULL;
static uint32_t g_storage_len = 0;
static bool g_is_mounted = false;

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
    uint16_t root_entry_count = fatfsjs_read_u16(image + 17);
    uint16_t total_sectors_16 = fatfsjs_read_u16(image + 19);
    uint16_t fat_size_sectors = fatfsjs_read_u16(image + 22);
    uint32_t total_sectors_32 = fatfsjs_read_u32(image + 32);
    uint32_t total_sectors =
        total_sectors_16 ? total_sectors_16 : total_sectors_32;

    if (bytes_per_sector != FATFSJS_BYTES_PER_SECTOR) {
        return FATFSJS_ERR_INVAL;
    }
    if (sectors_per_cluster == 0 || reserved_sectors == 0 || num_fats == 0 ||
        fat_size_sectors == 0 || total_sectors == 0) {
        return FATFSJS_ERR_INVAL;
    }

    uint32_t root_dir_sectors =
        ((uint32_t)root_entry_count * FATFSJS_DIR_ENTRY_SIZE +
         (bytes_per_sector - 1)) /
        bytes_per_sector;
    uint32_t fat_start_sector = reserved_sectors;
    uint32_t root_dir_sector = fat_start_sector + num_fats * fat_size_sectors;
    uint32_t data_start_sector = root_dir_sector + root_dir_sectors;

    if (root_dir_sector != FATFSJS_ROOT_DIR_SECTOR ||
        data_start_sector != FATFSJS_DATA_START_SECTOR) {
        return FATFSJS_ERR_INVAL;
    }

    uint64_t total_bytes = (uint64_t)total_sectors * bytes_per_sector;
    if (total_bytes == 0 || total_bytes > UINT32_MAX) {
        return FATFSJS_ERR_INVAL;
    }
    if ((uint32_t)total_bytes != image_len) {
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

static bool fatfsjs_is_root_path(const char *path) {
    if (!path || path[0] == '\0') {
        return true;
    }
    const char *cursor = path;
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    if (*cursor == '\0') {
        return true;
    }
    if (fatfsjs_starts_with_ci(cursor, "fatfs")) {
        cursor += 5;
    }
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    return *cursor == '\0';
}

static int fatfsjs_normalize_file_path(const char *path, char *out,
                                       size_t out_len) {
    if (!path || !out || out_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    const char *cursor = path;
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    if (fatfsjs_starts_with_ci(cursor, "fatfs")) {
        cursor += 5;
    }
    while (*cursor == '/' || *cursor == '\\') {
        cursor++;
    }
    if (*cursor == '\0') {
        return FATFSJS_ERR_INVAL;
    }

    size_t len = 0;
    while (*cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            return FATFSJS_ERR_UNSUPPORTED;
        }
        if (len + 1 >= out_len) {
            return FATFSJS_ERR_INVAL;
        }
        out[len++] = (char)toupper((unsigned char)*cursor);
        cursor++;
    }
    out[len] = '\0';
    return 0;
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

static int fatfsjs_find_entry(const char *name, fatfs_dirent_t *out) {
    if (!name || !out) {
        return FATFSJS_ERR_INVAL;
    }
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
        uint8_t first = entry[0];
        if (first == 0x00) {
            break;
        }
        if (first == 0xE5) {
            continue;
        }
        uint8_t attr = entry[11];
        if (attr == 0x0F) {
            continue;
        }
        if (attr & 0x08) {
            continue;
        }

        char entry_name[FATFSJS_MAX_NAME];
        fatfsjs_parse_short_name(entry, entry_name, sizeof(entry_name));
        if (entry_name[0] == '\0') {
            continue;
        }
        if (fatfsjs_compare_ci(name, entry_name) != 0) {
            continue;
        }

        out->size = fatfsjs_read_u32(entry + 28);
        out->first_cluster = fatfsjs_read_u16(entry + 26);
        out->is_dir = (attr & 0x10) != 0;
        strncpy(out->name, entry_name, sizeof(out->name));
        out->name[sizeof(out->name) - 1] = '\0';
        return 0;
    }
    return FATFSJS_ERR_NOT_FOUND;
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

EMSCRIPTEN_KEEPALIVE
int fatfsjs_list(const char *path, uint32_t buffer_ptr, uint32_t buffer_len) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }
    if (!buffer_ptr || buffer_len == 0) {
        return FATFSJS_ERR_INVAL;
    }
    if (!fatfsjs_is_root_path(path)) {
        return FATFSJS_ERR_UNSUPPORTED;
    }

    char *cursor = (char *)(uintptr_t)buffer_ptr;
    const char *end = cursor + buffer_len;
    *cursor = '\0';

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
        uint8_t first = entry[0];
        if (first == 0x00) {
            break;
        }
        if (first == 0xE5) {
            continue;
        }
        uint8_t attr = entry[11];
        if (attr == 0x0F) {
            continue;
        }
        if (attr & 0x08) {
            continue;
        }

        char entry_name[FATFSJS_MAX_NAME];
        fatfsjs_parse_short_name(entry, entry_name, sizeof(entry_name));
        if (entry_name[0] == '\0') {
            continue;
        }

        uint32_t size = fatfsjs_read_u32(entry + 28);
        char type = (attr & 0x10) != 0 ? 'd' : 'f';
        err = fatfsjs_emit_entry(entry_name, size, type, &cursor, end);
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
int fatfsjs_file_size(const char *path) {
    int err = fatfsjs_ensure_mounted();
    if (err) {
        return err;
    }

    char normalized[FATFSJS_MAX_NAME];
    err = fatfsjs_normalize_file_path(path, normalized, sizeof(normalized));
    if (err) {
        return err;
    }

    fatfs_dirent_t entry;
    err = fatfsjs_find_entry(normalized, &entry);
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

    char normalized[FATFSJS_MAX_NAME];
    err = fatfsjs_normalize_file_path(path, normalized, sizeof(normalized));
    if (err) {
        return err;
    }

    fatfs_dirent_t entry;
    err = fatfsjs_find_entry(normalized, &entry);
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
