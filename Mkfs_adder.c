#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>

#define BS 4096u
#define INODE_SIZE 128u
#define DIRENT_SIZE 64u
#define ROOT_INO 1u
#define MAX_NAME 58

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

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t checksum;
} dirent64_t;
#pragma pack(pop)

/* CRC32 table (for inode CRC) */
static uint32_t CRC32_TAB[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        CRC32_TAB[i] = c;
    }
}
static uint32_t crc32(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = CRC32_TAB[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void inode_crc_finalize(inode_t *ino) {
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t cc = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)cc;
}
static void dirent_checksum_finalize(dirent64_t *d) {
    const uint8_t *p = (const uint8_t*)d;
    uint8_t x = 0;
    for (int i = 0; i < 63; ++i) x ^= p[i];
    d->checksum = x;
}

/* bitmap helpers (bit addressed) */
static inline int get_bit(uint8_t *bm, uint64_t idx) { return (bm[idx >> 3] >> (idx & 7)) & 1; }
static inline void set_bit(uint8_t *bm, uint64_t idx) { bm[idx >> 3] |= (1u << (idx & 7)); }

static void print_usage(const char *p) {
    fprintf(stderr, "Usage: %s --input <in.img> --output <out.img> --file <file>\n", p);
}

int main(int argc, char *argv[]) {
    crc32_init();

    char *input_name = NULL, *output_name = NULL, *file_name = NULL;
    static struct option long_opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"file", required_argument, 0, 'f'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:f:", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'i': input_name = optarg; break;
            case 'o': output_name = optarg; break;
            case 'f': file_name = optarg; break;
            default: print_usage(argv[0]); return 1;
        }
    }
    if (!input_name || !output_name || !file_name) { print_usage(argv[0]); return 1; }

    /* open input and read entire first block (superblock) */
    FILE *fin = fopen(input_name, "rb");
    if (!fin) { perror("fopen input"); return 1; }

    uint8_t sb_block[BS];
    if (fseeko(fin, 0, SEEK_SET) != 0) { perror("seek input start"); fclose(fin); return 1; }
    if (fread(sb_block, 1, BS, fin) != BS) { perror("read sb block"); fclose(fin); return 1; }

    superblock_t sb;
    memcpy(&sb, sb_block, sizeof(sb));

    /* create output and copy entire input into it */
    FILE *fout = fopen(output_name, "wb");
    if (!fout) { perror("fopen output"); fclose(fin); return 1; }

    if (fseeko(fin, 0, SEEK_SET) != 0) { perror("seek input start2"); fclose(fin); fclose(fout); return 1; }

    uint8_t block[BS];
    for (uint64_t b = 0; b < sb.total_blocks; ++b) {
        size_t r = fread(block, 1, BS, fin);
        if (r != BS) { fprintf(stderr, "read image block: expected %u got %zu\n", BS, r); fclose(fin); fclose(fout); return 1; }
        if (fwrite(block, 1, BS, fout) != BS) { perror("write image block"); fclose(fin); fclose(fout); return 1; }
    }
    fflush(fout);
    fclose(fin);

    /* reopen output as read+write */
    fclose(fout);
    fout = fopen(output_name, "r+b");
    if (!fout) { perror("reopen output"); return 1; }

    /* read inode bitmap (full blocks) */
    uint64_t ib_bytes = sb.inode_bitmap_blocks * BS;
    uint8_t *inode_bm = malloc(ib_bytes);
    if (!inode_bm) { perror("malloc inode_bm"); fclose(fout); return 1; }
    if (fseeko(fout, (off_t)sb.inode_bitmap_start * BS, SEEK_SET) != 0) { perror("seek inode bitmap"); free(inode_bm); fclose(fout); return 1; }
    if (fread(inode_bm, 1, ib_bytes, fout) != ib_bytes) { perror("read inode bitmap"); free(inode_bm); fclose(fout); return 1; }

    /* pick first free inode (0-based index), set bit */
    int chosen_inode = -1;
    for (uint64_t i = 0; i < sb.inode_count; ++i) {
        if (!get_bit(inode_bm, i)) { chosen_inode = (int)i; set_bit(inode_bm, i); break; }
    }
    if (chosen_inode == -1) { fprintf(stderr, "No free inode available\n"); free(inode_bm); fclose(fout); return 1; }

    /* read data bitmap (full blocks) */
    uint64_t db_bytes = sb.data_bitmap_blocks * BS;
    uint8_t *data_bm = malloc(db_bytes);
    if (!data_bm) { perror("malloc data_bm"); free(inode_bm); fclose(fout); return 1; }
    if (fseeko(fout, (off_t)sb.data_bitmap_start * BS, SEEK_SET) != 0) { perror("seek data bitmap"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fread(data_bm, 1, db_bytes, fout) != db_bytes) { perror("read data bitmap"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    /* open file to add and get size */
    FILE *fadd = fopen(file_name, "rb");
    if (!fadd) { perror("open file to add"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fseeko(fadd, 0, SEEK_END) != 0) { perror("seek end file"); fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    off_t fsize_off = ftello(fadd);
    if (fsize_off < 0) { perror("ftello"); fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    size_t file_sz = (size_t)fsize_off;
    rewind(fadd);

    /* compute required blocks and ensure <= 12 */
    size_t required_blocks = (file_sz + BS - 1) / BS;
    if (required_blocks > 12) {
        fprintf(stderr, "File too large: requires %zu blocks (>12)\n", required_blocks);
        fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1;
    }

    /* allocate data blocks (first-fit), write full BS blocks (zero padded) */
    inode_t newino;
    memset(&newino, 0, sizeof(newino));
    newino.mode = 0x8000; /* file */
    newino.links = 1;
    newino.size_bytes = (uint64_t)file_sz;
    newino.atime = newino.mtime = newino.ctime = (uint64_t)time(NULL);

    uint8_t write_block[BS];
    size_t remaining = file_sz;
    int allocated = 0;
    for (uint64_t i = 0; i < sb.data_region_blocks && allocated < (int)required_blocks; ++i) {
        if (!get_bit(data_bm, i)) {
            set_bit(data_bm, i);
            uint64_t block_no = sb.data_region_start + i;
            newino.direct[allocated++] = (uint32_t)block_no;

            /* read chunk from file */
            size_t toread = remaining > BS ? BS : remaining;
            memset(write_block, 0, BS);
            if (fread(write_block, 1, toread, fadd) != toread) {
                perror("fread file chunk");
                fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1;
            }

            if (fseeko(fout, (off_t)block_no * BS, SEEK_SET) != 0) { perror("seek data block"); fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1; }
            if (fwrite(write_block, 1, BS, fout) != BS) { perror("write data block"); fclose(fadd); free(inode_bm); free(data_bm); fclose(fout); return 1; }

            remaining -= toread;
        }
    }
    fclose(fadd);
    if (remaining > 0) { fprintf(stderr, "Not enough free data blocks available\n"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    inode_crc_finalize(&newino);

    /* write the new inode into inode table (in-place) */
    off_t inode_off = (off_t)sb.inode_table_start * BS + (off_t)chosen_inode * INODE_SIZE;
    if (fseeko(fout, inode_off, SEEK_SET) != 0) { perror("seek inode"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fwrite(&newino, 1, sizeof(newino), fout) != sizeof(newino)) { perror("write inode"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    /* update root directory block: read root inode, find free dirent slot, write dirent there */
    inode_t rootino;
    off_t root_inode_off = (off_t)sb.inode_table_start * BS + (off_t)(ROOT_INO - 1) * INODE_SIZE;
    if (fseeko(fout, root_inode_off, SEEK_SET) != 0) { perror("seek root inode"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fread(&rootino, 1, sizeof(rootino), fout) != sizeof(rootino)) { perror("read root inode"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    if (rootino.direct[0] == 0) { fprintf(stderr, "Root has no data block\n"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    uint32_t root_block_no = rootino.direct[0];
    off_t root_block_off = (off_t)root_block_no * BS;

    uint8_t root_block[BS];
    if (fseeko(fout, root_block_off, SEEK_SET) != 0) { perror("seek root block"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fread(root_block, 1, BS, fout) != BS) { perror("read root block"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    int slot = -1;
    int nslots = BS / DIRENT_SIZE;
    for (int s = 0; s < nslots; ++s) {
        dirent64_t *d = (dirent64_t *)(root_block + s * DIRENT_SIZE);
        if (d->inode_no == 0) { slot = s; break; }
    }
    if (slot == -1) { fprintf(stderr, "No free dirent slot in root\n"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    dirent64_t newd;
    memset(&newd, 0, sizeof(newd));
    newd.inode_no = (uint32_t)(chosen_inode + 1); /* store 1-indexed inode number */
    newd.type = 1; /* file */
    strncpy(newd.name, file_name, MAX_NAME - 1);
    newd.name[MAX_NAME - 1] = '\0';
    dirent_checksum_finalize(&newd);

    memcpy(root_block + slot * DIRENT_SIZE, &newd, DIRENT_SIZE);
    if (fseeko(fout, root_block_off, SEEK_SET) != 0) { perror("seek root block write"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fwrite(root_block, 1, BS, fout) != BS) { perror("write root block back"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    /* update root inode metadata (links and size), recompute CRC and write back */
    rootino.links += 1;
    rootino.size_bytes += DIRENT_SIZE;
    inode_crc_finalize(&rootino);
    if (fseeko(fout, root_inode_off, SEEK_SET) != 0) { perror("seek root inode write"); free(inode_bm); free(data_bm); fclose(fout); return 1; }
    if (fwrite(&rootino, 1, sizeof(rootino), fout) != sizeof(rootino)) { perror("write root inode"); free(inode_bm); free(data_bm); fclose(fout); return 1; }

    /* write back inode bitmap and data bitmap (full blocks) */
    if (fseeko(fout, (off_t)sb.inode_bitmap_start * BS, SEEK_SET) != 0) perror("seek inode bitmap write");
    if (fwrite(inode_bm, 1, ib_bytes, fout) != ib_bytes) perror("write inode bitmap");
    if (fseeko(fout, (off_t)sb.data_bitmap_start * BS, SEEK_SET) != 0) perror("seek data bitmap write");
    if (fwrite(data_bm, 1, db_bytes, fout) != db_bytes) perror("write data bitmap");

    fflush(fout);
    free(inode_bm);
    free(data_bm);
    fclose(fout);

    printf("Added '%s' as inode %d -> output: %s\n", file_name, chosen_inode + 1, output_name);
    return 0;
}
