/* The report file: one header, one section per module with a result. */
#ifndef REPORT_H
#define REPORT_H

#include <ppu-types.h>

void rep_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *rep_when(void);                /* "2026-10-07 22:52 UTC" of the report being written */
void write_report(void);                   /* header, console, every module, then the file */
void report_footnote(void);                /* the report path at the foot of a screen */
u32 report_to_usb(char *msg, int n);       /* SELECT: the report and the journal to a USB stick; msg and its colour */

#endif
