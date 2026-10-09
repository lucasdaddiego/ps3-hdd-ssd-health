/* The freeze journal: a file in the app folder that records each journaled
 * call before it runs, so that a call which froze the console never runs
 * again (journal.c has the file format). */
#ifndef JOURNAL_H
#define JOURNAL_H

#include <ppu-types.h>

/* Journal state of one named call. SKIPPED = declined at the first-run prompt,
 * kept until SQUARE twice clears the journal. */
enum { J_NONE, J_PENDING, J_OK, J_BAD, J_FROZEN, J_SKIPPED };

#define JOURNAL_NAMES 32             /* the table: 32 names of at most 23 characters; more share the last slot */

void journal_init(const char *dir);                    /* the app folder: the file is <dir>/journal.txt */
void journal_load(void);                               /* reads the tail of the file (the last 32 KB); a "start" without a "done" becomes froze; the others call it once, at their first use */
int journal_clear(void);                               /* the clear gesture: an empty journal, so the next start runs every call again; 0, or the write rc */
int journal_state(const char *name);                   /* the J_* state of a named call */
void journal_skip(const char *name);                   /* the first-run prompt was declined: remembered until the journal is cleared */
int journal_start(const char *name);                   /* "start name" before the call: 1, or 0 for a frozen or skipped entry (do not run it) */
void journal_done(const char *name, s32 rc, int ok);   /* "done name rc= ok=" after the call */
int journal_needed(const char *name);                  /* a polled call is journaled when its state is not yet known in this session */

#endif
