#include "tarfs.h"
#include "string.h"
#include "serial.h"

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];     /* "ustar\0" or "ustar  " */
    char version[2];   /* "00" */
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static uint64_t parse_octal(const char *str, size_t max_len) {
    uint64_t val = 0;
    size_t i = 0;

    /* Skip leading spaces */
    while (i < max_len && str[i] == ' ') {
        i++;
    }

    while (i < max_len && str[i] >= '0' && str[i] <= '7') {
        /* Check for overflow before shift */
        if (val > (0xFFFFFFFFFFFFFFFFULL >> 3)) {
            return 0xFFFFFFFFFFFFFFFFULL; /* Overflow */
        }
        val = (val << 3) + (uint64_t)(str[i] - '0');
        i++;
    }

    return val;
}

static bool is_zero_block(const uint8_t *block) {
    for (size_t i = 0; i < 512; i++) {
        if (block[i] != 0) return false;
    }
    return true;
}

int tarfs_init(const void *archive_data, size_t archive_size) {
    if (!archive_data || archive_size < 512) {
        serial_puts("[FAIL] TarFS: Invalid archive memory or size\n");
        return -1;
    }

    vfs_init();

    const uint8_t *ptr = (const uint8_t *)archive_data;
    size_t offset = 0;
    int files_loaded = 0;

    serial_puts("--> Parsing USTAR Initramfs Archive (Size: ");
    serial_print_dec(archive_size);
    serial_puts(" bytes)...\n");

    while (offset + 512 <= archive_size) {
        const struct ustar_header *hdr = (const struct ustar_header *)(ptr + offset);

        /* Two consecutive zero blocks mark the end of the tar archive */
        if (is_zero_block((const uint8_t *)hdr)) {
            break;
        }

        /* Verify USTAR magic */
        if (memcmp(hdr->magic, "ustar", 5) != 0) {
            serial_puts("[FAIL] TarFS: Non-USTAR archive entry at offset ");
            serial_print_hex(offset);
            serial_puts("\n");
            return -2;
        }

        uint64_t file_size = parse_octal(hdr->size, sizeof(hdr->size));
        if (file_size == 0xFFFFFFFFFFFFFFFFULL) {
            serial_puts("[FAIL] TarFS: Octal size overflow at offset ");
            serial_print_hex(offset);
            serial_puts("\n");
            return -3;
        }

        /* Build full path from prefix and name */
        char full_path[VFS_MAX_PATH];
        size_t p_idx = 0;

        if (hdr->prefix[0] != '\0') {
            size_t pfx_len = 0;
            while (pfx_len < sizeof(hdr->prefix) && hdr->prefix[pfx_len] != '\0') {
                full_path[p_idx++] = hdr->prefix[pfx_len++];
            }
            if (p_idx > 0 && full_path[p_idx - 1] != '/') {
                full_path[p_idx++] = '/';
            }
        }

        size_t name_len = 0;
        while (name_len < sizeof(hdr->name) && hdr->name[name_len] != '\0' && p_idx + 1 < sizeof(full_path)) {
            full_path[p_idx++] = hdr->name[name_len++];
        }
        full_path[p_idx] = '\0';

        /* Calculate data offset and padded size */
        size_t data_offset = offset + 512;
        uint64_t padded_size = (file_size + 511) & ~511ULL;

        /* Overflow-safe archive boundary check */
        if (data_offset > archive_size || padded_size > archive_size - data_offset) {
            serial_puts("[FAIL] TarFS: File payload extends beyond archive bounds: ");
            serial_puts(full_path);
            serial_puts("\n");
            return -4;
        }

        const void *file_data = (const void *)(ptr + data_offset);

        /* Enforce supported entry types: only regular files ('0' or '\0') and directories ('5') */
        if (hdr->typeflag == '5') {
            /* Directory */
            vfs_create_node(full_path, VFS_DIRECTORY, 0, NULL);
            serial_puts("       [DIR ] ");
            serial_puts(full_path);
            serial_puts("\n");
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            /* Regular File */
            vfs_node_t *node = vfs_create_node(full_path, VFS_FILE, file_size, file_data);
            if (!node) {
                serial_puts("[FAIL] TarFS: Failed to register file: ");
                serial_puts(full_path);
                serial_puts("\n");
                return -5;
            }
            files_loaded++;
            serial_puts("       [FILE] ");
            serial_puts(full_path);
            serial_puts(" (");
            serial_print_dec(file_size);
            serial_puts(" bytes)\n");
        } else {
            serial_puts("[FAIL] TarFS: Unsupported tar entry typeflag ('");
            serial_putc(hdr->typeflag);
            serial_puts("') in: ");
            serial_puts(full_path);
            serial_puts("\n");
            return -6;
        }

        offset = data_offset + (size_t)padded_size;
    }

    serial_puts("[ OK ] TarFS initialized: ");
    serial_print_dec(files_loaded);
    serial_puts(" files mounted into VFS\n");
    return 0;
}
