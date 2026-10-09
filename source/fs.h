/* Whole files through the sysLv2Fs* syscalls (the sysFs* calls are stubs for
 * the cellFs module, which an app must load first), and the write test at
 * start. */
#ifndef FS_H
#define FS_H

#include <stdint.h>

int fs_write_file(const char *path, const void *data, uint64_t len, int append);   /* 0, or the LV2 rc (-1: short write) */
int fs_read_file(const char *path, void *data, int n);       /* bytes read, -1 when missing */
int fs_selftest(const char *dir, int *mkdir_rc, int *open_rc, int *write_rc);   /* makes dir, appends "start" to dir/starts.txt: 0, or -1 with the rcs */

#endif
