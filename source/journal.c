/* The freeze journal: journal.txt in the app folder. "start X" (synced) goes
 * in before a journaled call and "done X" after it. A "start" without a
 * "done" means the console froze inside X: the next start writes "froze X"
 * and never runs X again. "skip X" is a first-run prompt declined. SQUARE
 * twice on the home screen empties the file. */
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include "fs.h"
#include "journal.h"

static char jpath[128];                      /* <dir>/journal.txt, from journal_init */
static struct { char name[24]; int state; } jst[JOURNAL_NAMES];
static int jcount, jloaded;

void journal_init(const char *dir) { snprintf(jpath, sizeof jpath, "%s/journal.txt", dir); }

static int *jstate(const char *name)
{
    for (int i = 0; i < jcount; i++)
        if (!strcmp(jst[i].name, name)) return &jst[i].state;
    if (jcount == JOURNAL_NAMES) return &jst[JOURNAL_NAMES - 1].state;
    snprintf(jst[jcount].name, sizeof jst[jcount].name, "%s", name);
    jst[jcount].state = J_NONE;
    return &jst[jcount++].state;
}

static void jappend(const char *fmt, const char *name, s32 rc, int ok)
{
    char line[96];
    snprintf(line, sizeof line, fmt, name, (unsigned)rc, ok);
    fs_write_file(jpath, line, strlen(line), 1);
}

/* The file is read once, at the first journal call of the session; drive_probe
 * reloads it explicitly. */
static void jready(void) { if (!jloaded) journal_load(); }

/* Reads the tail of the journal (the last 32 KB), so a long session cannot
 * push the newest "start"/"done" pair past the end of the buffer; the cut
 * line at the head of the tail is dropped. */
void journal_load(void)
{
    static char text[32768];
    s32 fd;
    u64 n = 0, size = 0, pos = 0;
    char *first = text;
    jcount = 0;
    jloaded = 1;
    if (sysLv2FsOpen(jpath, SYS_O_RDONLY, &fd, 0, NULL, 0)) return;
    if (sysLv2FsLSeek64(fd, 0, SEEK_END, &size) == 0 && size > sizeof text - 1) {
        sysLv2FsLSeek64(fd, size - (sizeof text - 1), SEEK_SET, &pos);
        first = NULL;                    /* the first line of the tail is cut */
    } else {
        sysLv2FsLSeek64(fd, 0, SEEK_SET, &pos);
    }
    sysLv2FsRead(fd, text, sizeof text - 1, &n);
    sysLv2FsClose(fd);
    text[n] = 0;
    if (!first) {
        first = strchr(text, '\n');
        first = first ? first + 1 : text + n;
    }
    for (char *line = strtok(first, "\n"); line; line = strtok(NULL, "\n")) {
        char verb[8], name[24];
        int ok = 0;
        if (sscanf(line, "%7s %23s", verb, name) != 2) continue;
        char *okp = strstr(line, "ok=");
        if (okp) ok = okp[3] == '1';
        int *s = jstate(name);
        if (!strcmp(verb, "start")) *s = J_PENDING;
        else if (!strcmp(verb, "done") && *s != J_FROZEN) *s = ok ? J_OK : J_BAD;
        else if (!strcmp(verb, "froze")) *s = J_FROZEN;
        else if (!strcmp(verb, "skip")) *s = J_SKIPPED;
    }
    for (int i = 0; i < jcount; i++)
        if (jst[i].state == J_PENDING) {
            jst[i].state = J_FROZEN;
            jappend("froze %s\n", jst[i].name, 0, 0);
        }
    if (size > sizeof text / 2) {    /* compact: one line per call, the frozen ones kept */
        char line[64];
        text[0] = 0;
        for (int i = 0; i < jcount; i++) {
            if (jst[i].state == J_FROZEN) snprintf(line, sizeof line, "froze %s\n", jst[i].name);
            else if (jst[i].state == J_SKIPPED) snprintf(line, sizeof line, "skip %s\n", jst[i].name);
            else snprintf(line, sizeof line, "done %s rc=0x00000000 ok=%d\n", jst[i].name, jst[i].state == J_OK);
            strcat(text, line);
        }
        fs_write_file(jpath, text, strlen(text), 0);
    }
}

/* The clear gesture: an empty journal, so the next start runs every call again. */
int journal_clear(void)
{
    jready();
    int rc = fs_write_file(jpath, "", 0, 0);
    if (rc == 0) jcount = 0;             /* a failed write keeps the old entries, in the file and here */
    return rc;
}

int journal_state(const char *name)
{
    jready();
    return *jstate(name);
}

/* The first-run prompt was declined: remembered until the journal is cleared. */
void journal_skip(const char *name)
{
    jready();
    jappend("skip %s\n", name, 0, 0);
    *jstate(name) = J_SKIPPED;
}

int journal_start(const char *name)
{
    jready();
    int *s = jstate(name);
    if (*s == J_FROZEN || *s == J_SKIPPED) return 0;
    jappend("start %s\n", name, 0, 0);
    *s = J_PENDING;
    return 1;
}

void journal_done(const char *name, s32 rc, int ok)
{
    jready();
    jappend("done %s rc=0x%08x ok=%d\n", name, rc, ok);
    *jstate(name) = ok ? J_OK : J_BAD;
}

/* A polled call (the Cooling module reads every 2 s) is journaled when its
 * state is not yet known in this session: the first call, and the first call
 * after SQUARE twice cleared the journal. */
int journal_needed(const char *name)
{
    jready();
    int s = *jstate(name);
    return s != J_OK && s != J_BAD;
}
