#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#define timegm _mkgmtime
#endif

#include "iomanX_port.h"

#define IOMANX_PATH_MAX    256
#define IOMANX_MOUNT_POINT "pfs0:"

/* Mostly based on musl libc's nftw implementation 35e9831156efc1b54e1a91917ba0f787d5df3353 */

typedef struct path_info
{
    int orig_len;
    const char *path_prefix;
} path_info_t;

typedef int (*wrapped_ftw_callback)(path_info_t *pi, const char *path, const iox_stat_t *st);

static int do_wrapped_ftw(path_info_t *pi, char *path, wrapped_ftw_callback fn)
{
    size_t l = strlen(path), j = l && path[l - 1] == '/' ? l - 1 : l;
    iox_stat_t st;
    int r;

    if (iomanX_getstat(path, &st) < 0) {
        return -1;
    }

    if ((r = fn(pi, path, &st))) {
        return r;
    }

    if (FIO_S_ISDIR(st.mode)) {
        int d = iomanX_dopen(path);
        if (d >= 0) {
            int result;
            iox_dirent_t de;

            while ((result = iomanX_dread(d, &de)) && result != -1) {
                if (de.name[0] == '.' && (!de.name[1] || (de.name[1] == '.' && !de.name[2])))
                    continue;
                if (strlen(de.name) >= IOMANX_PATH_MAX - l) {
                    iomanX_close(d);
                    return -1;
                }
                path[j] = '/';
                strcpy(path + j + 1, de.name);
                printf("%s\r", path);
                if ((r = do_wrapped_ftw(pi, path, fn))) {
                    iomanX_close(d);
                    return r;
                }
            }
            iomanX_close(d);
        } else {
            return -1;
        }
    }

    path[l] = 0;

    return 0;
}

static int wrapped_ftw(const char *path_prefix, const char *path, wrapped_ftw_callback fn)
{
    int r, cs;
    size_t l;
    path_info_t pi;
    char pathbuf[IOMANX_PATH_MAX + 1];

    l = strlen(path);
    if (l > IOMANX_PATH_MAX) {
        return -1;
    }
    memcpy(pathbuf, path, l + 1);

    pi.orig_len = l;
    pi.path_prefix = path_prefix;

    r = do_wrapped_ftw(&pi, pathbuf, fn);
    return r;
}

/* Mostly based on sltar code */

static time_t convert_iox_stat_time_to_posix(const unsigned char *iomanx_time)
{
    struct tm timeinfo;
    timeinfo.tm_sec = iomanx_time[1];
    timeinfo.tm_min = iomanx_time[2];
    timeinfo.tm_hour = iomanx_time[3];
    timeinfo.tm_mday = iomanx_time[4];
    timeinfo.tm_mon = iomanx_time[5] - 1;                               // month 1 (January) is 0
    timeinfo.tm_year = (iomanx_time[6] | (iomanx_time[7] << 8)) - 1900; // year 1900 is 0
    time_t rawtime = timegm(&timeinfo);
    // convert UTC->JST
    rawtime += (9 * 60 * 60);
    return rawtime;
}

static unsigned int convert_mode_to_posix(unsigned int iomanx_mode)
{
    unsigned int posix_mode = 0;
    if (FIO_S_ISDIR(iomanx_mode)) {
        posix_mode |= /* S_IFDIR */ 0040000;
    }
    if (FIO_S_ISREG(iomanx_mode)) {
        posix_mode |= /* S_IFREG */ 0100000;
    }
    if (FIO_S_ISLNK(iomanx_mode)) {
        posix_mode |= /* S_IFLNK */ 0120000;
    }
    if (iomanx_mode & FIO_S_IRUSR) {
        posix_mode |= /* S_IRUSR */ 0400;
    }
    if (iomanx_mode & FIO_S_IWUSR) {
        posix_mode |= /* S_IWUSR */ 0200;
    }
    if (iomanx_mode & FIO_S_IXUSR) {
        posix_mode |= /* S_IXUSR */ 0100;
    }
    if (iomanx_mode & FIO_S_IRGRP) {
        posix_mode |= /* S_IRGRP */ 0040;
    }
    if (iomanx_mode & FIO_S_IWGRP) {
        posix_mode |= /* S_IWGRP */ 0020;
    }
    if (iomanx_mode & FIO_S_IXGRP) {
        posix_mode |= /* S_IXGRP */ 0010;
    }
    if (iomanx_mode & FIO_S_IROTH) {
        posix_mode |= /* S_IROTH */ 0004;
    }
    if (iomanx_mode & FIO_S_IWOTH) {
        posix_mode |= /* S_IWOTH */ 0002;
    }
    if (iomanx_mode & FIO_S_IXOTH) {
        posix_mode |= /* S_IXOTH */ 0001;
    }
    if (iomanx_mode & FIO_S_ISUID) {
        posix_mode |= /* S_ISUID */ 04000;
    }
    if (iomanx_mode & FIO_S_ISGID) {
        posix_mode |= /* S_ISGID */ 02000;
    }
    if (iomanx_mode & FIO_S_ISVTX) {
        posix_mode |= /* S_ISVTX */ 01000;
    }
    return posix_mode;
}

enum TarHeader {
    NAME = 0,
    MODE = 100,
    UID = 108,
    GID = 116,
    SIZE = 124,
    MTIME = 136,
    CHK = 148,
    TYPE = 156,
    LINK = 157,
    MAGIC = 257,
    VERS = 263,
    UNAME = 265,
    GNAME = 297,
    DEVMAJOR = 329,
    DEVMINOR = 337,
    NAME2 = 345,
    END = 500 // ?? 512
};

static void tar_checksum(const char b[END], char *chk)
{
    unsigned sum = 0, i;
    for (i = 0; i < END; i++)
        sum += (i >= CHK && i < CHK + 8) ? ' ' : b[i];
    snprintf(chk, 8, "%.7o", sum);
}

static FILE *tarfile_handle = NULL;

/**
 * Function to tar a file
 *
 * @param pi - path_info_t pointer
 * @param in_path - input file path
 * @param st - iox_stat_t pointer
 * @return int - 0 on success
 */
static int tar_c_file(path_info_t *pi, const char *in_path, const iox_stat_t *st)
{
    int l = END;       // Initialize l
    char b[END] = {0}; // Initialize b array
    int f = -1;        // Initialize f
    int pathlen;       // Initialize pathlen
    char path[512];    // Initialize path array

    /* TODO: pax header for longer filenames and larger files */
    /* https://pubs.opengroup.org/onlinepubs/9699919799/utilities/pax.html#tag_20_92_13 */

    // Check if file is regular and size is within limits
    if ((FIO_S_ISREG(st->mode)) && (st->hisize != 0)) {
        printf("(!) %s: too large file. Skipping.\n", in_path); // Print message for large file
        /* The file is over 4GB, which we don't support (currently) */
        return 0;
    }

    // Construct full path
    snprintf(path, sizeof(path), "%s%s", pi->path_prefix, in_path + pi->orig_len);

    pathlen = strlen(path); // Get length of path

    // Check for empty path
    if (pathlen == 0) {
        printf("(!) %s: invalid path. Skipping.\n", in_path); // Print message for invalid path
        /* We don't need to archive the root */
        return 0;
    }

    // Set ustar magic and version
    memset(b + SIZE, '0', 11);
    memcpy(b + MAGIC, "ustar\x00", 6);
    memcpy(b + VERS, "00", 2);

    // Check for long path
    if (pathlen > 100) {
        const char *path_separate = strchr(path, '/');

        if (path_separate == NULL) {
            printf("(!) %s: path is too long. Skipping.\n", in_path); // Print message for long path
            return 0;
        }

        while ((path_separate - path) < sizeof(path)) {
            if (*path_separate == '\x00') {
                break;
            }
            const char *new_path_separate = strchr(path_separate + 1, '/');
            if (new_path_separate == NULL) {
                break;
            }
            if ((new_path_separate - path) >= 155) {
                break;
            }
            path_separate = new_path_separate;
        }

        if ((path_separate - path) >= 155) {
            printf("(!) %s: path is too long. Skipping.\n", in_path); // Print message for long path
            /* Path is too long */
            return 0;
        }

        {
            int prefix_pathlen = path_separate - path;

            // Set prefix path and name in header
            memcpy(b + NAME2, path, prefix_pathlen);
            memcpy(b + NAME, path + prefix_pathlen, pathlen - prefix_pathlen);
        }

    } else {
        // Set path in header
        memcpy(b + NAME, path, pathlen);
    }

    // Set mode and modification time in header
    snprintf(b + MODE, 8, "%.7o", (unsigned int)(convert_mode_to_posix(st->mode)));
    snprintf(b + MTIME, 12, "%.11o", (unsigned int)(convert_iox_stat_time_to_posix(st->mtime)));

    // Process based on file type
    if (FIO_S_ISREG(st->mode)) {
        b[TYPE] = '0';
        snprintf(b + SIZE, 12, "%.11o", (unsigned int)(st->size));
        f = iomanX_open(in_path, FIO_O_RDONLY);
    } else if (FIO_S_ISDIR(st->mode)) {
        b[TYPE] = '5';
    } else if (FIO_S_ISLNK(st->mode)) {
        b[TYPE] = '2';
        iomanX_readlink(in_path, b + LINK, 100);
    }
    tar_checksum(b, b + CHK); // Calculate checksum
    do {
        if (l < END) {
            memset(b + l, 0, END - l);
        }
        fwrite(b, END, 1, tarfile_handle);
    } while ((f >= 0) && ((l = iomanX_read(f, b, END)) > 0));
    iomanX_close(f);
    return 0;
}

static int tar_part(void)
{
    int retval = 0;
    int dh = iomanX_dopen("hdd0:");
    if (dh >= 0) {
        int result;
        iox_dirent_t de;
        while ((result = iomanX_dread(dh, &de)) && result != -1) {
            if (de.stat.mode == 0x0100) {
                char mount_point[256];
                char prefix_path[256];

                snprintf(mount_point, sizeof(mount_point), "hdd0:%s", de.name);

                result = iomanX_mount(IOMANX_MOUNT_POINT, mount_point, FIO_MT_RDONLY, NULL, 0);
                if (result < 0) {
                    fprintf(stderr, "(!) %s: %s.\n", mount_point, strerror(-result));
                    continue;
                }

                printf("\rpartition: %s                                                \n", de.name);
                snprintf(prefix_path, sizeof(prefix_path), "%s/", de.name);
                wrapped_ftw(prefix_path, IOMANX_MOUNT_POINT "/", tar_c_file);

                iomanX_umount(IOMANX_MOUNT_POINT);
            }
        }

        result = iomanX_close(dh);
        if (result < 0) {
            printf("(!) dclose: failed with %d\n", result);
            retval = -1;
        }
    } else {
        printf("(!) dopen: failed with %d\n", dh);
        retval = dh;
    }
    return retval;
}

static void show_help(const char *progname)
{
    printf("usage: %s <device_path> <tar_path>\n", progname);
}

/* where (image of) PS2 HDD is; in fake_sdk/atad.c */
extern void set_atad_device_path(const char *path);

extern void atad_close(void); /* fake_sdk/atad.c */

int main(int argc, char *argv[])
{
    int result;

    if (argc < 3) {
        show_help(argv[0]);
        return 1;
    }

    set_atad_device_path(argv[1]);

    static const char *apa_args[] =
        {
            "ps2hdd.irx",
            "-o", "2",
            NULL};

    /* mandatory */
    result = _init_apa(3, (char **)apa_args);
    if (result < 0) {
        fprintf(stderr, "(!) init_apa: failed with %d (%s)\n", result,
                strerror(-result));
        return 1;
    }

    static const char *pfs_args[] =
        {
            "pfs.irx",
            "-m", "1",
            "-o", "32",
            "-n", "127",
            NULL};

    /* mandatory */
    result = _init_pfs(7, (char **)pfs_args);
    if (result < 0) {
        fprintf(stderr, "(!) init_pfs: failed with %d (%s)\n", result,
                strerror(-result));
        return 1;
    }

    tarfile_handle = fopen(argv[2], "wb");
    if (tarfile_handle == NULL) {
        fprintf(stderr, "(!) %s: %s.\n", argv[2], strerror(errno));
        return 1;
    }

    tar_part();

    fclose(tarfile_handle);

    atad_close();

    return 0;
}
