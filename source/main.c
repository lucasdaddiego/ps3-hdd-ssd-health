/* PS3 Health: the hardware of a PS3 checked from a HEN app. Six modules behind
 * a home screen: Drive, Cooling, Controller, Display, Transfer, Memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ui.h"
#include "report.h"
#include "version.h"

int fs_ok, fs_mk, fs_op, fs_wr;

/* Every exit path: START, Quit Game from the PS button menu, and exit(). */
static void leave(void)
{
    drive_close(&D);
    ui_end();
}

int main(void)
{
    ui_init(&(const ui_app){"PS3 Health", APP_VERSION, APP_DIR});
    fs_ok = fs_selftest(APP_DIR, &fs_mk, &fs_op, &fs_wr) == 0;
    D.cpu_temp = D.rsx_temp = -1;
    if (!safe_load() && fs_ok) mod_display_calibrate(1);   /* the first start: the visible area of this TV */
    state_load();
    console_firmware();
    atexit(leave);
    int sel = 0;
    while (!quit) {
        sel = home_screen(sel);
        if (sel < 0) break;
        armed = 0;
        ui_module = modules[sel].name;
        int r = modules[sel].open();
        write_report();                      /* every module's result reaches the file */
        ui_module = NULL;
        ui_demo = 0;
        armed = 0;
        if (r < 0) break;
    }
    return 0;                                /* exit() runs leave() */
}
