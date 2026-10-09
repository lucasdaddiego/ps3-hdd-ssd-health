/* Whole files through the sysLv2Fs* syscalls: the sysFs* calls are stubs for
 * the cellFs module, which an app must load first. */
#include <stddef.h>
#include <sys/file.h>
#include "fs.h"
#include "ata.h"                             /* APP_DIR, until fs_selftest takes the folder */

int fs_write_file(const char *path, const void *data, u64 len, int append)
{
    s32 fd, rc;
    u64 w = 0;
    rc = sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (append ? SYS_O_APPEND : SYS_O_TRUNC), &fd, 0666, NULL, 0);
    if (rc) return rc;
    rc = len ? sysLv2FsWrite(fd, data, len, &w) : 0;     /* the empty journal: O_TRUNC did it */
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
    return rc ? rc : w == len ? 0 : -1;
}

/* Reads up to n bytes; returns the count, -1 when the file is missing. */
int fs_read_file(const char *path, void *data, int n)
{
    s32 fd;
    u64 r = 0;
    if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0)) return -1;
    sysLv2FsRead(fd, data, n, &r);
    sysLv2FsClose(fd);
    return (int)r;
}

/* File self-test before anything reaches the drive: the journal is the only
 * record of a freeze, so drive_probe must not run when writes fail. */
int fs_selftest(int *mkdir_rc, int *open_rc, int *write_rc)
{
    s32 fd;
    u64 w = 0;
    *mkdir_rc = sysLv2FsMkdir(APP_DIR, 0777);
    *write_rc = -1;
    *open_rc = sysLv2FsOpen(APP_DIR "/starts.txt", SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, 0666, NULL, 0);
    if (*open_rc) return -1;
    *write_rc = sysLv2FsWrite(fd, "start\n", 6, &w);
    sysLv2FsFsync(fd);
    sysLv2FsClose(fd);
    return *write_rc == 0 && w == 6 ? 0 : -1;
}
