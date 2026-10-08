#ifndef PV_SYS_FILE_H
#define PV_SYS_FILE_H
#include "../ppu-types.h"
#define SYS_O_RDONLY 0x000000
#define SYS_O_WRONLY 0x000001
#define SYS_O_RDWR   0x000002
#define SYS_O_CREAT  0x000040
#define SYS_O_TRUNC  0x000200
#define SYS_O_APPEND 0x000400
typedef struct { s32 st_mode, st_uid, st_gid; u64 st_atime_, st_mtime_, st_ctime_, st_size, st_blksize; } sysFSStat;
s32 sysLv2FsOpen(const char *path, s32 oflags, s32 *fd, u32 mode, const void *arg, u64 argsize);
s32 sysLv2FsClose(s32 fd);
s32 sysLv2FsRead(s32 fd, void *buf, u64 len, u64 *nread);
s32 sysLv2FsWrite(s32 fd, const void *buf, u64 len, u64 *nwritten);
s32 sysLv2FsLSeek64(s32 fd, s64 off, s32 whence, u64 *pos);
s32 sysLv2FsFsync(s32 fd);
s32 sysLv2FsStat(const char *path, sysFSStat *st);
s32 sysLv2FsMkdir(const char *path, s32 mode);
s32 sysLv2FsUnlink(const char *path);
#endif
