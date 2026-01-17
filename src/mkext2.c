#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "iomanX_port.h"
#include "mkext2.h"

// --- CONSTANTS ---
#define SUB_PART_HEADERS_SECTORS 8      // 0x1000 bytes / 512 = 8 sectors
#define MAIN_PART_HEADER_SECTORS 0x2000 // 4MB / 512 = 8192 sectors

// --- EXT2 & SWAP Definitions ---
// --- EXT2 Definitions ---
#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_ROOT_INO    2
#define EXT2_S_IFDIR     0x4000
#define EXT2_S_IRWXU     0x01C0
#define EXT2_S_IRGRP     0x0020
#define EXT2_S_IROTH     0x0004
#define EXT2_S_ISGID     0x0400
#define EXT2_FT_DIR      2

#define SWAP_VERSION      1
#define SWAP_UUID_LENGTH  16
#define SWAP_LABEL_LENGTH 16
#define SWAP_SIGNATURE    "SWAPSPACE2"
#define SWAP_ZERO_OFFSET  3968
#define SWAP_SIG_OFFSET   4086

// --- APA STRUCTURES ---
typedef struct
{
    uint32_t start;  // LBA start
    uint32_t length; // Length in sectors
} apa_sub_t;

typedef struct hddIoctl2Transfer_
{
    unsigned int sub;
    unsigned int sector;
    unsigned int size;
    unsigned int mode;
    void *buffer;
} hddIoctl2Transfer_t;

typedef struct
{
    uint32_t checksum;
    uint32_t magic;
    uint32_t next;
    uint32_t prev;
    char id[32];
    char rpwd[8];
    char fpwd[8];
    uint32_t start;
    uint32_t length;
    uint16_t type;
    uint16_t flags;
    uint32_t nsub;
    uint32_t created_date; // simplified
    uint32_t main;
    uint32_t number;
    uint32_t mod_date; // simplified
    uint32_t padding[7];
    char pwc_hash[8];
    char pwc_key[8];
    char unknown[32];
    // Array of subs starts at offset 0x44 + ... logic.
    // In standard APA header, subs structs are located after the main header fields.
    // The struct above is simplified. We will read subs by offset.
} apa_header_t;


typedef struct
{
    uint32_t start_lba;     // Absolute LBA of the chunk start
    uint32_t data_offset;   // Data offset within the chunk (4MB or 2KB)
    uint32_t data_length;   // Data length in bytes
    uint64_t logical_start; // Logical offset from the start of the "file" partition
} part_chunk_t;

typedef struct
{
    int fd; // Descriptor for hdd0:
    int num_chunks;
    uint64_t total_size;
    part_chunk_t *chunks;
} raw_part_handle_t;

// --- Structures ---
struct swap_header_v1
{
    uint32_t padding1[256];
    uint32_t version;
    uint32_t last_page;
    uint32_t nr_badpages;
    uint8_t uuid[SWAP_UUID_LENGTH];
    char volume_name[SWAP_LABEL_LENGTH];
    uint32_t padding2[117];
    uint32_t badpages[1];
};

struct ext2_super_block
{
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t s_prealloc_blocks;
    uint8_t s_prealloc_dir_blocks;
    uint16_t s_padding1;
    uint8_t s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t s_def_hash_version;
    uint8_t s_reserved_char_pad;
    uint16_t s_reserved_word_pad;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_reserved[190];
};

struct ext2_group_desc
{
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};

struct ext2_inode
{
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t i_osd2[12];
};

struct ext2_dir_entry
{
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
};

static int write_part_old(int fd, uint64_t sector, void *buf, size_t size)
{
    int64_t offset = sector * 512;
    int result = 0;
    // int ret = 0;
    // if (ret == iomanX_lseek(fd, offset, SEEK_SET) != offset) {
    //     printf("MKEXT2: seek error %d, offset %lld\n", ret, offset);
    //     return -1;
    // }
    hddIoctl2Transfer_t CmdData;
    CmdData.sub = 0; // main partition
    CmdData.sector = sector;
    CmdData.size = size / 512;
    CmdData.mode = 1; // write mode
    CmdData.buffer = buf;

    if ((result = iomanX_ioctl2(fd, HIOCTRANSFER, &CmdData, 0, NULL, 0)) < 0) {
        printf("MKEXT2: I/O error %d occurred at partition %u, sector 0x%08x, num sectors: %u, code: %d\n", result, CmdData.sub, CmdData.sector, CmdData.size, result);
        return result;
    }

    return 0;
}

static int write_part(int fd, uint64_t sector_offset, void *buffer, size_t size)
{
    hddIoctl2Transfer_t CmdData;
    int result;

    // Checks
    if ((size % 512 != 0) || (buffer == NULL)) {
        printf("MKEXT2: Invalid arguments! Size must be aligned to 512.\n");
        return -22; // EINVAL
    }

    uint32_t SectorsRemaining = size / 512;
    uint32_t CurrentLogicalSector = sector_offset;
    uint8_t *BufPtr = (uint8_t *)buffer;

    // 1. Get the number of sub-partitions
    int num_subs = iomanX_ioctl2(fd, HIOCNSUB, NULL, 0, NULL, 0);
    if (num_subs < 0)
        return num_subs;

    // 2. Find the starting partition (Seek)
    // We need to skip partitions that come BEFORE our offset
    unsigned int CurrentPartNum = 0;
    unsigned int ReservedSectors = 0;
    unsigned int PartRawSize = 0;
    unsigned int PartUsableSize = 0;

    for (CurrentPartNum = 0; CurrentPartNum <= (unsigned int)num_subs; CurrentPartNum++) {
        // Get the size of the current chunk
        PartRawSize = iomanX_ioctl2(fd, HIOCGETSIZE, &CurrentPartNum, sizeof(CurrentPartNum), NULL, 0);

        // Main partition (0) has a 4MB (0x2000) header
        // Sub-partitions have a 2KB (4 sectors) header
        ReservedSectors = (CurrentPartNum == 0) ? MAIN_PART_HEADER_SECTORS : SUB_PART_HEADERS_SECTORS;

        PartUsableSize = PartRawSize - ReservedSectors;


        // If our offset falls INSIDE this partition
        if (CurrentLogicalSector < PartUsableSize) {
            break; // We found the starting point!
        }

        // If not, subtract the size of this partition from the offset and move to the next one
        CurrentLogicalSector -= PartUsableSize;
    }

    if (CurrentPartNum > (unsigned int)num_subs) {
        printf("MKEXT2: Write offset beyond partition size.\n");
        return -28; // ENOSPC
    }

    // 3. Write loop
    while (SectorsRemaining > 0) {
        // How much space is left in the CURRENT chunk (starting from CurrentLogicalSector)
        // PartRawSize and ReservedSectors are already up-to-date from the loop above or the update below

        // Recalculate just in case we entered the loop not from the search
        if (PartUsableSize == 0) { // Если цикл продолжился на след. итерации
            PartRawSize = iomanX_ioctl2(fd, HIOCGETSIZE, &CurrentPartNum, sizeof(CurrentPartNum), NULL, 0);
            ReservedSectors = (CurrentPartNum == 0) ? MAIN_PART_HEADER_SECTORS : SUB_PART_HEADERS_SECTORS;
            PartUsableSize = PartRawSize - ReservedSectors;
        }

        unsigned int SectorsSpaceInPart = PartUsableSize - CurrentLogicalSector;
        unsigned int ChunkToWrite = (SectorsRemaining < SectorsSpaceInPart) ? SectorsRemaining : SectorsSpaceInPart;

        if (ChunkToWrite > 0) {
            CmdData.sub = CurrentPartNum;
            CmdData.sector = ReservedSectors + CurrentLogicalSector; // Physical sector (Header + Offset)
            CmdData.size = ChunkToWrite;
            CmdData.mode = 1; // Write
            CmdData.buffer = BufPtr;

            if ((result = iomanX_ioctl2(fd, HIOCTRANSFER, &CmdData, 0, NULL, 0)) < 0) {
                printf("MKEXT2: I/O error part %u sec %u: %d\n", CurrentPartNum, CmdData.sector, result);
                return result;
            }

            SectorsRemaining -= ChunkToWrite;
            BufPtr += (ChunkToWrite * 512);
        }

        // If there is still data to write, it means the current partition has ended
        if (SectorsRemaining > 0) {
            CurrentPartNum++;
            if (CurrentPartNum > (unsigned int)num_subs) {
                return -28; // ENOSPC (Disk full)
            }
            // Reset local offset for the new partition
            CurrentLogicalSector = 0;
            // Reset cached size to recalculate at the beginning of the loop
            PartUsableSize = 0;
        }
    }

    return size;
}

static uint32_t find_partition_lba(const char *name)
{
    int dh = iomanX_dopen("hdd0:");
    if (dh < 0)
        return 0;

    iox_dirent_t dirent;
    uint32_t lba = 0;

    while (iomanX_dread(dh, &dirent) > 0) {
        if (strcmp(dirent.name, name) == 0 && dirent.stat.attr == 0) {
            // stat.private_5 = Start LBA
            lba = dirent.stat.private_5;
            break;
        }
    }
    iomanX_close(dh);
    return lba;
}

static uint32_t get_apa_partition_size(int fd)
{
    int32_t num_subs;
    uint32_t i;
    uint32_t total_usable_sectors = 0;

    num_subs = iomanX_ioctl2(fd, HIOCNSUB, NULL, 0, NULL, 0);
    if (num_subs < 0)
        return 0;

    for (i = 0; i < (uint32_t)num_subs + 1; i++) {
        uint32_t raw_size = 0;
        uint32_t reserved = 0;
        int res = iomanX_ioctl2(fd, HIOCGETSIZE, &i, sizeof(i), NULL, 0);
        if (res < 0)
            return 0;

        raw_size = (uint32_t)res;
        reserved = (i == 0) ? MAIN_PART_HEADER_SECTORS : SUB_PART_HEADERS_SECTORS;

        if (raw_size > reserved) {
            total_usable_sectors += (raw_size - reserved);
        }
    }
    return total_usable_sectors;
}

int format_ext2_partition(const char *mount_point)
{
    char path[256];
    sprintf(path, "hdd0:%s", mount_point);

    printf("MKEXT2: Opening %s...\n", path);

    int fd = iomanX_open(path, FIO_O_RDWR, 0666);
    if (fd < 0) {
        printf("MKEXT2: Error opening partition (err: %d)\n", fd);
        return -1;
    }

    uint32_t partition_size_sectors = get_apa_partition_size(fd);
    if (partition_size_sectors == 0) {
        printf("MKEXT2: Size detection failed.\n");
        iomanX_close(fd);
        return -1;
    }

    printf("MKEXT2: Usable size: %.2f MB (%u sectors)\n", (float)partition_size_sectors * 512.0 / 1024.0 / 1024.0, partition_size_sectors);

    // --- SETUP GEOMETRY ---
    uint32_t block_size = 4096;
    uint32_t sector_per_block = block_size / 512;
    uint32_t blocks_count = partition_size_sectors / sector_per_block;
    uint32_t inodes_per_group = 8192;
    uint32_t blocks_per_group = block_size * 8; // 32768

    uint32_t groups_count = (blocks_count + blocks_per_group - 1) / blocks_per_group;

    struct ext2_super_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count = groups_count * inodes_per_group;
    sb.s_blocks_count = blocks_count;
    sb.s_r_blocks_count = blocks_count / 20;

    sb.s_free_inodes_count = sb.s_inodes_count - 10;

    sb.s_first_data_block = 0;
    sb.s_log_block_size = 2; // 4096
    sb.s_log_frag_size = 2;
    sb.s_blocks_per_group = blocks_per_group;
    sb.s_frags_per_group = blocks_per_group;
    sb.s_inodes_per_group = inodes_per_group;
    sb.s_magic = EXT2_SUPER_MAGIC;
    sb.s_state = 1;
    sb.s_rev_level = 1;
    sb.s_first_ino = 11;
    sb.s_inode_size = 128;
    sb.s_feature_incompat = 0x0002;
    sb.s_max_mnt_count = 20;
    sb.s_wtime = time(NULL);

    uint32_t desc_blocks = (groups_count * sizeof(struct ext2_group_desc) + block_size - 1) / block_size;
    struct ext2_group_desc *gds = (struct ext2_group_desc *)calloc(desc_blocks * block_size, 1);

    uint32_t inode_table_blocks = (inodes_per_group * 128) / block_size;
    uint32_t total_free_blocks = 0;

    // --- FILL GDT ---
    for (uint32_t i = 0; i < groups_count; i++) {
        uint32_t group_start_block = i * blocks_per_group;
        uint32_t current_blk = group_start_block;

        current_blk += 1;           // SB
        current_blk += desc_blocks; // GDT

        gds[i].bg_block_bitmap = current_blk++;
        gds[i].bg_inode_bitmap = current_blk++;
        gds[i].bg_inode_table = current_blk;
        current_blk += inode_table_blocks;

        uint32_t used_in_group = current_blk - group_start_block;
        uint32_t free_in_group = blocks_per_group - used_in_group;

        if (i == groups_count - 1) {
            uint32_t group_end = group_start_block + blocks_per_group;
            if (group_end > sb.s_blocks_count) {
                uint32_t overflow = group_end - sb.s_blocks_count;
                if (free_in_group > overflow)
                    free_in_group -= overflow;
                else
                    free_in_group = 0;
            }
        }

        gds[i].bg_free_blocks_count = free_in_group;
        gds[i].bg_free_inodes_count = inodes_per_group;
        gds[i].bg_used_dirs_count = 0;

        if (i == 0) {
            gds[i].bg_free_inodes_count -= 10;
        }

        total_free_blocks += free_in_group;
    }

    gds[0].bg_free_blocks_count--;
    gds[0].bg_used_dirs_count = 1;
    total_free_blocks--;
    sb.s_free_blocks_count = total_free_blocks;


    // --- WRITE TO DISK ---
    uint8_t *block_buf = (uint8_t *)malloc(block_size);
    uint8_t *zero_buf = (uint8_t *)calloc(1, block_size);

    for (uint32_t i = 0; i < groups_count; i++) {
        uint64_t group_start_sec = (uint64_t)(sb.s_first_data_block + i * blocks_per_group) * sector_per_block;

        // 1. SB
        memset(block_buf, 0, block_size);
        if (i == 0)
            memcpy(block_buf + 1024, &sb, sizeof(sb));
        else
            memcpy(block_buf, &sb, sizeof(sb));
        write_part(fd, group_start_sec, block_buf, block_size);

        // 2. GDT
        uint64_t gdt_sec = group_start_sec + sector_per_block;
        for (uint32_t b = 0; b < desc_blocks; b++) {
            void *ptr = (uint8_t *)gds + (b * block_size);
            write_part(fd, gdt_sec + (b * sector_per_block), ptr, block_size);
        }

        // 3. Block Bitmap
        memset(block_buf, 0, block_size); // 0 = Free

        uint32_t metadata_blocks = 1 + desc_blocks + 1 + 1 + inode_table_blocks;
        if (i == 0)
            metadata_blocks++; // + Root Data

        for (uint32_t k = 0; k < metadata_blocks; k++)
            block_buf[k / 8] |= (1 << (k % 8));

        // Masking last group (Bits beyond disk end must be 1)
        if (i == groups_count - 1) {
            uint32_t group_start = i * blocks_per_group;
            if (group_start + blocks_per_group > sb.s_blocks_count) {
                uint32_t valid_count = sb.s_blocks_count - group_start;
                // Set bits from valid_count to end of block to 1
                uint32_t start_byte = valid_count / 8;
                uint32_t start_bit = valid_count % 8;

                if (start_bit != 0) {
                    for (int b = start_bit; b < 8; b++)
                        block_buf[start_byte] |= (1 << b);
                    start_byte++;
                }
                if (start_byte < block_size) {
                    memset(block_buf + start_byte, 0xFF, block_size - start_byte);
                }
            }
        }
        write_part(fd, (uint64_t)gds[i].bg_block_bitmap * sector_per_block, block_buf, block_size);

        // 4. Inode Bitmap
        memset(block_buf, 0, block_size);
        if (i == 0) {
            // Reserved 1-10 used
            for (int k = 0; k < 10; k++)
                block_buf[k / 8] |= (1 << (k % 8));
        }

        uint32_t inode_bytes_used = inodes_per_group / 8;
        if (inode_bytes_used < block_size) {
            memset(block_buf + inode_bytes_used, 0xFF, block_size - inode_bytes_used);
        }

        write_part(fd, (uint64_t)gds[i].bg_inode_bitmap * sector_per_block, block_buf, block_size);

        // 5. Inode Table
        for (uint32_t k = 0; k < inode_table_blocks; k++) {
            if (i == 0 && k == 0) {
                memset(block_buf, 0, block_size);
                struct ext2_inode *root = (struct ext2_inode *)(block_buf + 128); // Inode 2

                root->i_mode = EXT2_S_IFDIR | EXT2_S_IRWXU | EXT2_S_IRGRP | EXT2_S_IROTH | EXT2_S_ISGID;
                root->i_size = block_size;
                root->i_links_count = 2;
                root->i_blocks = sector_per_block;
                root->i_atime = root->i_ctime = root->i_mtime = time(NULL);
                root->i_block[0] = gds[0].bg_inode_table + inode_table_blocks;

                write_part(fd, (uint64_t)(gds[0].bg_inode_table) * sector_per_block, block_buf, block_size);
            } else {
                write_part(fd, (uint64_t)(gds[i].bg_inode_table + k) * sector_per_block, zero_buf, block_size);
            }
        }
    }

    // --- WRITE ROOT DATA ---
    memset(block_buf, 0, block_size);
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)block_buf;
    de->inode = EXT2_ROOT_INO;
    de->name_len = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0] = '.';
    de->rec_len = 12;

    struct ext2_dir_entry *de2 = (struct ext2_dir_entry *)((char *)de + de->rec_len);
    de2->inode = EXT2_ROOT_INO;
    de2->name_len = 2;
    de2->file_type = EXT2_FT_DIR;
    de2->name[0] = '.';
    de2->name[1] = '.';
    de2->rec_len = block_size - 12;

    uint32_t root_data_blk = gds[0].bg_inode_table + inode_table_blocks;
    write_part(fd, (uint64_t)root_data_blk * sector_per_block, block_buf, block_size);

    free(block_buf);
    free(zero_buf);
    free(gds);

    iomanX_close(fd);
    printf("EXT2 partition format done.\n");
    return 0;
}

int format_swap_partition(const char *mount_point)
{
    char path[256];
    sprintf(path, "hdd0:%s", mount_point);

    printf("MKEXT2: Formatting %s as Linux SWAP (v1)...\n", path);
    int result;
    iox_stat_t stat;
    int fd;

    fd = iomanX_open(path, FIO_O_RDWR, 0666);
    // 1. Get partition size
    uint32_t partition_size_sectors = get_apa_partition_size(fd);
    if (partition_size_sectors == 0) {
        printf("MKEXT2: Size detection failed.\n");
        iomanX_close(fd);
        return -1;
    }

    unsigned int lba = find_partition_lba(path);

    printf("MKEXT2: Usable size: %.2f MB (%u sectors)\n",
           (float)partition_size_sectors * 512.0 / 1024.0 / 1024.0, partition_size_sectors);

    // 2. Prepare Buffer (1 Page = 4096 bytes)
    uint32_t block_size = 4096;
    uint8_t *block_buf = (uint8_t *)calloc(1, block_size);

    if (!block_buf) {
        printf("MKEXT2: Failed to allocate memory.\n");
        iomanX_close(fd);
        return -1;
    }
    memset(block_buf, 0xFF, block_size);

    // 3. Fill Header
    struct swap_header_v1 *sh = (struct swap_header_v1 *)block_buf;

    // Pages calculation: Total 512-byte sectors / 8 = Total 4096-byte pages
    uint32_t total_pages = partition_size_sectors / 8;

    memset(sh->padding1, 0, sizeof(sh->padding1));
    sh->version = SWAP_VERSION; // 1
    // last_page - this is the index of the last available page (starting from 0)
    // Since the first page is occupied by the header, total pages N, last is N-1
    sh->last_page = total_pages - 1;
    sh->nr_badpages = 0;
    memset(sh->padding2, 0, sizeof(sh->padding2));
    memset(sh->badpages, 0, sizeof(sh->badpages));

    // Generate simple pseudo-random UUID
    // srand(time(NULL));
    // for (int i = 0; i < SWAP_UUID_LENGTH; i++) {
    //     sh->uuid[i] = rand() % 256;
    // }
    // Assign fixed UUID: 25FFC62E7ADC4F358FFA237BC045C1AD
    const uint8_t fixed_uuid[SWAP_UUID_LENGTH] = {0x25, 0xFF, 0xC6, 0x2E, 0x7A, 0xDC, 0x4F, 0x35, 0x8F, 0xFA, 0x23, 0x7B, 0xC0, 0x45, 0xC1, 0xAD};
    memcpy(sh->uuid, fixed_uuid, SWAP_UUID_LENGTH);

    // Volume label (optional, empty)
    memset(sh->volume_name, 0, SWAP_LABEL_LENGTH);

    // 4. Write Signature at the END of the page
    memset(block_buf + SWAP_ZERO_OFFSET, 0, block_size - SWAP_ZERO_OFFSET);
    memcpy(block_buf + SWAP_SIG_OFFSET, SWAP_SIGNATURE, 10);

    // 5. Write to disk
    if (write_part(fd, 0, block_buf, block_size) < 0) {
        printf("MKEXT2: Failed to write SWAP header.\n");
        free(block_buf);
        iomanX_close(fd);
        return -1;
    }

    free(block_buf);
    iomanX_close(fd);

    printf("SWAP partition format done.\n");
    return 0;
}
