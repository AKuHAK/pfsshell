#ifndef MKEXT2_H
#define MKEXT2_H

// mount_point - the mount point of the partition to format as EXT2
int format_ext2_partition(const char *mount_point);

// format swap partition
int format_swap_partition(const char *mount_point);

#endif
