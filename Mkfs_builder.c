#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/types.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u

#pragma pack(push,1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must be 116 bytes");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode must be 128 bytes");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent must be 64 bytes");

/* Simple CRC32 implementation (table) */
static uint32_t CRC32_TAB[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRC32_TAB[i] = c;
    }
}
static uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = CRC32_TAB[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* Superblock CRC over struct bytes before checksum field */
static void superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *)sb, offsetof(superblock_t, checksum));
    sb->checksum = s;
}

/* Inode CRC: zero crc area and compute over first 120 bytes */
static void inode_crc_finalize(inode_t *ino) {
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

/* Dirent checksum: XOR of first 63 bytes */
static void dirent_checksum_finalize(dirent64_t *d) {
    const uint8_t *p = (const uint8_t *)d;
    uint8_t x = 0;
    for (int i = 0; i < 63; ++i) x ^= p[i];
    d->checksum = x;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --image <file> --size-kib <size> --inodes <count>\n", prog);
    fprintf(stderr, "  --image: output image filename\n");
    fprintf(stderr, "  --size-kib: size in KiB (180-4096, multiple of 4)\n");
    fprintf(stderr, "  --inodes: number of inodes (128-512)\n");
}

/* Helper: write `count` bytes of zero to file stream in chunks */
static int write_zeros_in_chunks(FILE *f, uint64_t count) {
    const size_t CHUNK = 64 * 1024; /* 64 KiB */
    uint8_t *buf = NULL;
    if (count == 0) return 0;
    buf = calloc(1, CHUNK);
    if (!buf) return -1;
    while (count > 0) {
        size_t to_write = (count > CHUNK) ? CHUNK : (size_t)count;
        if (fwrite(buf, 1, to_write, f) != to_write) { free(buf); return -1; }
        count -= to_write;
    }
    free(buf);
    return 0;
}

int main(int argc, char *argv[]) {
    crc32_init();

    char *image_name = NULL;
    int size_kib = 0;
    int inodes = 0;

    static struct option long_options[] = {
        {"image", required_argument, 0, 'i'},
        {"size-kib", required_argument, 0, 's'},
        {"inodes", required_argument, 0, 'n'},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:s:n:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': image_name = optarg; break;
            case 's': size_kib = atoi(optarg); break;
            case 'n': inodes = atoi(optarg); break;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (!image_name || size_kib < 180 || size_kib > 4096 || inodes < 128 || inodes > 512) {
        fprintf(stderr, "Invalid parameters\n");
        print_usage(argv[0]);
        return 1;
    }
    if (size_kib % 4 != 0) {
        fprintf(stderr, "Size must be a multiple of 4\n");
        return 1;
    }

    uint64_t total_blocks = (uint64_t)size_kib * 1024ULL / BS;
    uint64_t inode_table_blocks = (inodes * INODE_SIZE + BS - 1) / BS;
    uint64_t inode_table_bytes = inode_table_blocks * (uint64_t)BS;
    uint64_t total_bytes = total_blocks * (uint64_t)BS;

    /* Validate that we have enough space for the file system structure */
    if (3 + inode_table_blocks > total_blocks) {
        fprintf(stderr, "Error: File system too small for %d inodes\n", inodes);
        fprintf(stderr, "Need at least %lu blocks, but only have %lu blocks\n",
                3 + inode_table_blocks, total_blocks);
        return 1;
    }

    uint64_t data_region_blocks = total_blocks - 3 - inode_table_blocks;
    if (data_region_blocks < 1) {
        fprintf(stderr, "Error: No space for data blocks\n");
        return 1;
    }

    superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = 0x4D565346u;
    sb.version = 1u;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = inodes;
    sb.inode_bitmap_start = 1;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = 2;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = 3;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = 3 + inode_table_blocks;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = (uint64_t)time(NULL);
    sb.flags = 0;
    sb.checksum = 0;

    /* finalize checksum over struct (excluding checksum field) */
    superblock_crc_finalize(&sb);

    /* create and open image file */
    FILE *img = fopen(image_name, "wb");
    if (!img) { perror("fopen"); return 1; }

    /* write full 4KiB superblock block (struct at front, rest zeros) */
    uint8_t sb_block[BS];
    memset(sb_block, 0, BS);
    memcpy(sb_block, &sb, sizeof(sb));
    if (fwrite(sb_block, BS, 1, img) != 1) { perror("write sb"); fclose(img); return 1; }

    /* write inode bitmap block (mark inode 1 used) */
    uint8_t *inode_b = calloc(1, BS);
    if (!inode_b) { perror("malloc"); fclose(img); return 1; }
    inode_b[0] = 0x01; /* inode #1 allocated */
    if (fwrite(inode_b, BS, 1, img) != 1) { perror("write ib"); free(inode_b); fclose(img); return 1; }
    free(inode_b);

    /* write data bitmap block (mark first data block used for root dir) */
    uint8_t *data_b = calloc(1, BS);
    if (!data_b) { perror("malloc"); fclose(img); return 1; }
    data_b[0] = 0x01; /* first data block in data region allocated */
    if (fwrite(data_b, BS, 1, img) != 1) { perror("write db"); free(data_b); fclose(img); return 1; }
    free(data_b);

    /* prepare inode table buffer (zero padded) and initialize root inode */
    uint8_t *itab = calloc(1, inode_table_bytes);
    if (!itab) { perror("malloc"); fclose(img); return 1; }

    inode_t *itable = (inode_t *)itab;
    memset(&itable[0], 0, sizeof(inode_t));
    itable[0].mode = 0x4000; /* directory */
    itable[0].links = 2;
    itable[0].size_bytes = 2 * sizeof(dirent64_t);
    itable[0].atime = itable[0].mtime = itable[0].ctime = (uint64_t)time(NULL);
    /* root's first data block pointer is absolute block number = data_region_start */
    itable[0].direct[0] = (uint32_t)sb.data_region_start;
    itable[0].proj_id = 0;
    inode_crc_finalize(&itable[0]);

    if (fwrite(itab, inode_table_bytes, 1, img) != 1) { perror("write it"); free(itab); fclose(img); return 1; }
    free(itab);

    /* write root directory entries (at start of first data block) */
    dirent64_t entries[2];
    memset(entries, 0, sizeof(entries));
    entries[0].inode_no = 1; entries[0].type = 2;
    strncpy(entries[0].name, ".", sizeof(entries[0].name)-1);
    entries[0].name[sizeof(entries[0].name)-1] = '\0';
    entries[1].inode_no = 1; entries[1].type = 2;
    strncpy(entries[1].name, "..", sizeof(entries[1].name)-1);
    entries[1].name[sizeof(entries[1].name)-1] = '\0';
    dirent_checksum_finalize(&entries[0]);
    dirent_checksum_finalize(&entries[1]);

    /* seek to absolute first data block and write entries (rest of block remains zero) */
    off_t data_block_off = (off_t)sb.data_region_start * (off_t)BS;
    if (fseeko(img, data_block_off, SEEK_SET) != 0) { perror("seek data region"); fclose(img); return 1; }
    if (fwrite(entries, sizeof(entries), 1, img) != 1) { perror("write entries"); fclose(img); return 1; }

    /* pad the rest of the image with zeros in chunks (avoid single huge allocation) */
    uint64_t used = ((uint64_t)sb.data_region_start * (uint64_t)BS) + sizeof(entries);
    if (total_bytes > used) {
        uint64_t rem = total_bytes - used;
        /* seek to current position (should already be after entries) to ensure correct location */
        if (fseeko(img, (off_t)used, SEEK_SET) != 0) { perror("seek pad"); fclose(img); return 1; }
        if (write_zeros_in_chunks(img, rem) != 0) { perror("write zeros"); fclose(img); return 1; }
    }

    if (fflush(img) != 0) { perror("fflush"); fclose(img); return 1; }
    if (fclose(img) != 0) { perror("fclose"); return 1; }

    printf("File system image created: %s\nSize: %d KiB, Inodes: %d, Blocks: %" PRIu64 "\n",
           image_name, size_kib, inodes, total_blocks);
    printf("Data region blocks: %" PRIu64 "\n", data_region_blocks);
    return 0;
}