/* The app: six modules behind a home screen, one report, one journal. */
#ifndef APP_H
#define APP_H

#include "ata.h"
#include "fs.h"
#include "journal.h"

/* A module: a tile on the home screen and its own screens. open() returns 0
 * to go back home, -1 to exit the app. report() appends its section with
 * rep_out() and writes nothing when it has no result. */
typedef struct {
    const char *name;        /* the tile and the title bar */
    const char *key;         /* the key in state.txt */
    const char *desc;        /* one sentence in the tile: what the module checks */
    int (*open)(void);
    void (*report)(void);
} module;

#define MODULE_COUNT 6
extern const module modules[MODULE_COUNT];

/* The last result of each module, kept in APP_DIR/state.txt for the home
 * screen: a dot (DOT_*), two lines of at most 44 characters, the date. */
enum { DOT_NONE, DOT_OK, DOT_WARN, DOT_BAD };
typedef struct {
    int dot;
    char l1[48], l2[48], when[16];
} tile_state;
void state_load(void);
void state_set(const char *key, int dot, const char *l1, const char *l2);

extern int fs_ok;                  /* APP_DIR is writable: the journal works */
extern int fs_mk, fs_op, fs_wr;    /* the file test return codes, for the home screen when fs_ok is 0 */

int home_screen(int sel);          /* the module chosen, -1 to exit */
int help_screen(void);             /* TRIANGLE on the home screen: the QR code and a short guide; 0 back, -1 exit */
void help_keep(void);              /* the QR text of the drive result to APP_DIR/compat.txt, for Help in a later session */

/* modules */
extern drive_state D;
int mod_drive_open(void);
void mod_drive_report(void);
int mod_drive_probed(void);        /* 1 when the drive (or the demo data) was read in this session */
int mod_display_open(void);
void mod_display_report(void);
int mod_pad_open(void);
void mod_pad_report(void);
int mod_mem_open(void);
void mod_mem_report(void);
int mod_net_open(void);
void mod_net_report(void);
int mod_cool_open(void);
void mod_cool_report(void);

#endif
