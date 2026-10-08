/* The Transfer module: the 64 MB file test on a USB stick, a download from
 * the internet over plain HTTP, and a LAN test where the app is the server
 * and a PC sends it data with one command. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ppu-lv2.h>
#include <sys/file.h>
#include <lv2/systime.h>
#include <sys/systime.h>
#include <net/net.h>
#include <net/netctl.h>
#include <sysmodule/sysmodule.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "app.h"
#include "ui.h"
#include "report.h"

#define DEFAULT_URL "http://speed.cloudflare.com/__down?bytes=50000000"
#define LAN_PORT 5201

/* usb.rc: file_speed_test's rc (0, an LV2 error, -1), or one of these, all above 0 */
enum { USB_NO_STICK = 1, USB_FROZEN, USB_NO_JOURNAL };
static struct { int done, rc; double mb, wsec, rsec; char dir[32]; } usb;
static struct { int done, rc, http, port; double bytes, sec; char host[96], path[160], note[96]; } dl;
static struct { int done, rc; double bytes, sec; char note[96]; } lan;
static int net_up;
static char my_ip[32];
static char lan_msg[160];

static void net_init(void)
{
    if (!net_up) {
        if (netInitialize() == 0) net_up = 1;
        sysModuleLoad(SYSMODULE_NETCTL);     /* libnetctl does not load its module itself */
        netCtlInit();
    }
    union net_ctl_info info;                 /* read again on every open: DHCP may have answered since */
    if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) == 0) snprintf(my_ip, sizeof my_ip, "%s", info.ip_address);
    else snprintf(my_ip, sizeof my_ip, "no address");
}

/* ---- USB ------------------------------------------------------------------ */

static void job_usb(void)
{
    usb.done = 0;
    usb.dir[0] = 0;
    if (!fs_ok) { usb.rc = USB_NO_JOURNAL; usb.done = 1; return; }   /* a freeze would go unrecorded */
    if (usb_find(usb.dir, sizeof usb.dir)) { usb.rc = USB_NO_STICK; usb.done = 1; return; }
    int js = journal_state("speed_usb");
    if (js == J_FROZEN || js == J_SKIPPED) { usb.rc = USB_FROZEN; usb.done = 1; return; }
    progress("Writing and reading a 64 MB file on the USB stick");
    usb.rc = file_speed_test(usb.dir, "speed_usb", &usb.mb, &usb.wsec, &usb.rsec);
    usb.done = 1;
}

/* ---- HTTP download -------------------------------------------------------- */

/* host and path from APP_DIR/net_url.txt when present, else the default. */
static void pick_url(void)
{
    char url[256];
    int n = fs_read_file(APP_DIR "/net_url.txt", url, sizeof url - 1);
    if (n <= 0) snprintf(url, sizeof url, "%s", DEFAULT_URL);
    else {
        url[n] = 0;
        for (char *c = url; *c; c++) if (*c == '\n' || *c == '\r' || *c == ' ') { *c = 0; break; }
    }
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    const char *slash = strchr(h, '/');
    snprintf(dl.host, sizeof dl.host, "%.*s", (int)(slash ? slash - h : (long)strlen(h)), h);
    snprintf(dl.path, sizeof dl.path, "%s", slash ? slash : "/");
    dl.port = 80;
    char *colon = strchr(dl.host, ':');
    if (colon) { dl.port = atoi(colon + 1); *colon = 0; }
    if (dl.port <= 0 || dl.port > 65535) dl.port = 80;
}

static int resolve(const char *host, in_addr_t *out)
{
    *out = inet_addr(host);
    if (*out != (in_addr_t)-1) return 0;
    struct net_hostent *he = netGetHostByName(host);
    if (!he || !he->h_addr_list) return -1;
    uint32_t *list = (uint32_t *)(uintptr_t)he->h_addr_list;     /* 32-bit pointers in the network library */
    if (!list[0]) return -1;
    *out = *(uint32_t *)(uintptr_t)list[0];
    return 0;
}

static int tcp_connect(in_addr_t ip, int port)
{
    int s = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct timeval tv = {15, 0};
    netSetSockOpt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    netSetSockOpt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    if (netConnect(s, (struct sockaddr *)&a, sizeof a) < 0) { netClose(s); return -2; }
    return s;
}

static char netbuf[65536] __attribute__((aligned(128)));

static void job_download(void)
{
    in_addr_t ip;
    dl.done = 0;
    dl.http = 0;
    dl.bytes = 0;
    dl.note[0] = 0;
    pick_url();
    net_init();
    if (!net_up) { dl.rc = -1; snprintf(dl.note, sizeof dl.note, "network not initialised"); dl.done = 1; return; }
    progress("Resolving the host name");
    if (resolve(dl.host, &ip)) { dl.rc = -2; snprintf(dl.note, sizeof dl.note, "DNS failed for %.60s", dl.host); dl.done = 1; return; }
    progress("Connecting");
    int s = tcp_connect(ip, dl.port);
    if (s < 0) { dl.rc = -3; snprintf(dl.note, sizeof dl.note, "connect failed (%d)", s); dl.done = 1; return; }
    int n = snprintf(netbuf, sizeof netbuf, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PS3-Health\r\nConnection: close\r\n\r\n",
                     dl.path, dl.host);
    if (netSend(s, netbuf, n, 0) != n) { netClose(s); dl.rc = -4; snprintf(dl.note, sizeof dl.note, "send failed"); dl.done = 1; return; }
    progress("Downloading");
    s64 t0 = sysGetSystemTime(), tbody = 0, tlast = 0;
    int header_done = 0, hlen = 0, closed = 0;
    double expect = 0, total = 0;            /* every byte received, headers included */
    while (!job_cancel) {
        ssize_t r = netRecv(s, netbuf, sizeof netbuf, 0);
        if (r == 0) closed = 1;              /* the server ended the file */
        if (r <= 0) break;
        tlast = sysGetSystemTime();
        total += r;
        if (!header_done) {
            /* the first packets hold the status line and the headers; only the
             * head copy is capped, the byte count is not */
            static char head[4096];
            int take = (int)r < (int)sizeof head - 1 - hlen ? (int)r : (int)sizeof head - 1 - hlen;
            memcpy(head + hlen, netbuf, take);
            hlen += take;
            head[hlen] = 0;
            char *end = strstr(head, "\r\n\r\n");
            if (!end && hlen >= (int)sizeof head - 1) { snprintf(dl.note, sizeof dl.note, "headers over 4 KB"); break; }
            if (!end) continue;
            header_done = 1;
            sscanf(head, "HTTP/%*d.%*d %d", &dl.http);
            char *cl = strstr(head, "Content-Length:");
            if (!cl) cl = strstr(head, "content-length:");
            if (cl) expect = atof(cl + 15);
            dl.bytes = total - (end + 4 - head);
            tbody = sysGetSystemTime();
            if (dl.http != 200) {
                char *loc = strstr(head, "Location:");
                if (loc) {
                    loc += 9;
                    while (*loc == ' ') loc++;
                    int ll = (int)strcspn(loc, "\r\n");
                    snprintf(dl.note, sizeof dl.note, "HTTP %d, redirect to %.*s", dl.http, ll > 60 ? 60 : ll, loc);
                } else snprintf(dl.note, sizeof dl.note, "HTTP %d", dl.http);
                break;
            }
            continue;
        }
        dl.bytes += r;
        if (expect > 0) job_set_percent((int)(dl.bytes * 100 / expect));
        if (dl.bytes >= 50e6) break;
    }
    netClose(s);
    dl.sec = (double)((tlast ? tlast : sysGetSystemTime()) - (tbody ? tbody : t0)) / 1e6;   /* until the last byte, not the timeout */
    int complete = dl.bytes >= 50e6 || (expect > 0 ? dl.bytes >= expect : closed);   /* the test stops at 50 MB of a larger file */
    dl.rc = dl.http == 200 && dl.bytes > 0 && complete && !job_cancel ? 0 : -5;
    if (job_cancel) snprintf(dl.note, sizeof dl.note, "stopped");
    else if (dl.http == 200 && !complete)
        snprintf(dl.note, sizeof dl.note, "cut short: %.1f of %.1f MB in %.1f s", dl.bytes / 1e6, expect / 1e6, dl.sec);
    dl.done = 1;
}

/* ---- LAN receive ---------------------------------------------------------- */

static void job_lan(void)
{
    lan.done = 0;
    lan.bytes = 0;
    lan.note[0] = 0;
    net_init();
    if (!net_up) { lan.rc = -1; snprintf(lan.note, sizeof lan.note, "network not initialised"); lan.done = 1; return; }
    int ls = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) { lan.rc = -2; snprintf(lan.note, sizeof lan.note, "socket failed"); lan.done = 1; return; }
    int one = 1;
    netSetSockOpt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    netSetSockOpt(ls, SOL_SOCKET, SO_NBIO, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons(LAN_PORT);
    a.sin_addr.s_addr = INADDR_ANY;
    if (netBind(ls, (struct sockaddr *)&a, sizeof a) < 0 || netListen(ls, 1) < 0) {
        netClose(ls);
        lan.rc = -3;
        snprintf(lan.note, sizeof lan.note, "bind or listen failed on port %d", LAN_PORT);
        lan.done = 1;
        return;
    }
    snprintf(lan_msg, sizeof lan_msg, "Waiting for the PC. On it, run:   nc %s %d < /dev/zero", my_ip, LAN_PORT);
    progress(lan_msg);
    int s = -1;
    s64 t0 = sysGetSystemTime();
    while (!job_cancel && sysGetSystemTime() - t0 < 120000000) {
        struct sockaddr_in from;
        memset(&from, 0, sizeof from);
        socklen_t fl = sizeof from;
        s = netAccept(ls, (struct sockaddr *)&from, &fl);
        if (s >= 0) break;
        sysUsleep(50000);
    }
    netClose(ls);
    if (s < 0) { lan.rc = -4; snprintf(lan.note, sizeof lan.note, job_cancel ? "stopped" : "no PC connected in 2 min"); lan.done = 1; return; }
    int zero = 0;
    netSetSockOpt(s, SOL_SOCKET, SO_NBIO, &zero, sizeof zero);
    struct timeval tv = {5, 0};
    netSetSockOpt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    progress("Receiving for 10 seconds");
    s64 t1 = sysGetSystemTime(), now = t1;
    while (!job_cancel && (now = sysGetSystemTime()) - t1 < 10000000) {
        ssize_t r = netRecv(s, netbuf, sizeof netbuf, 0);
        if (r <= 0) break;
        lan.bytes += r;
        job_set_percent((int)((now - t1) / 100000));
    }
    netClose(s);
    lan.sec = (double)(now - t1) / 1e6;
    lan.rc = lan.bytes > 0 ? 0 : -5;
    if (lan.bytes == 0) snprintf(lan.note, sizeof lan.note, "connected, but no data arrived");
    lan.done = 1;
}

/* ---- screen --------------------------------------------------------------- */

static double mbit(double bytes, double sec) { return sec > 0 ? bytes * 8 / sec / 1e6 : 0; }
static double mbs(double mb, double sec) { return sec > 0 ? mb / sec : 0; }   /* MB/s; 0 when the timer saw no time */

static const char *usb_why(void)
{
    return usb.rc == USB_NO_STICK ? "no stick" : usb.rc == USB_FROZEN ? "froze before" : usb.rc == USB_NO_JOURNAL ? "no journal" : "failed";
}

static void net_tile(void)
{
    char l1[40], l2[40];
    if (!usb.done && !dl.done && !lan.done) return;
    int failed = (usb.done && usb.rc) + (dl.done && dl.rc) + (lan.done && lan.rc);
    if (usb.done && usb.rc == 0) snprintf(l1, sizeof l1, "USB %.0f/%.0f MB/s", mbs(usb.mb, usb.wsec), mbs(usb.mb, usb.rsec));
    else if (usb.done) snprintf(l1, sizeof l1, "USB test: %s", usb_why());
    else snprintf(l1, sizeof l1, "USB not tested");
    int j = 0;
    l2[0] = 0;
    if (dl.done && dl.rc == 0) j += snprintf(l2 + j, sizeof l2 - j, "net %.0f", mbit(dl.bytes, dl.sec));
    else if (dl.done) j += snprintf(l2 + j, sizeof l2 - j, "net failed");
    if (lan.done && lan.rc == 0) j += snprintf(l2 + j, sizeof l2 - j, "%sLAN %.0f", j ? ", " : "", mbit(lan.bytes, lan.sec));
    else if (lan.done) j += snprintf(l2 + j, sizeof l2 - j, "%sLAN failed", j ? ", " : "");
    if ((dl.done && dl.rc == 0) || (lan.done && lan.rc == 0)) snprintf(l2 + j, sizeof l2 - j, " Mbit/s");
    if (!l2[0]) snprintf(l2, sizeof l2, "network not tested");
    state_set("transfer", failed ? DOT_WARN : DOT_OK, l1, l2);
}

static void result(float x, float y, float w, u32 c, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void result(float x, float y, float w, u32 c, const char *fmt, ...)
{
    char v[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    text_fit(x + 32, y + TEST_RESULT_Y, F_MED, c, w - 64, "%s", v);
}

static void detail(float x, float y, float w, int k, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void detail(float x, float y, float w, int k, const char *fmt, ...)
{
    char v[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    text_fit(x + 32, y + TEST_RESULT_Y + 52 + k * 38, F_BODY, GREY, w - 64, "%s", v);
}

int mod_net_open(void)
{
    net_init();
    pick_url();
    while (1) {
        read_pad();
        int back = ui_leave();
        if (back <= 0) { net_tile(); return back; }
        if (pressed & BTN_CROSS) { run_job("USB stick speed test", job_usb); write_report(); }
        if (pressed & BTN_TRIANGLE) { run_job_cancelable("Internet download", job_download); write_report(); }
        if (pressed & BTN_SQUARE) { run_job_cancelable("LAN test", job_lan); write_report(); }
        begin_frame();
        title();
        title_right("Console address: %s", my_ip);
        float w = (SW - 2 * MG - 64) / 3, h = BOTTOM - TOP - 56 > 500 ? 500 : BOTTOM - TOP - 56, y = TOP;
        float x = MG;
        test_card(x, y, w, h, BTN_CROSS, "USB stick", "Writes a 64 MB file on the first USB stick, reads it back and deletes it.");
        if (usb.done && usb.rc == 0) {
            result(x, y, w, GREEN, "%.0f / %.0f MB/s", mbs(usb.mb, usb.wsec), mbs(usb.mb, usb.rsec));
            detail(x, y, w, 0, "write %.0f MB/s, read %.0f MB/s", mbs(usb.mb, usb.wsec), mbs(usb.mb, usb.rsec));
            detail(x, y, w, 1, "%.0f MB file on %s", usb.mb, usb.dir);
        } else if (usb.done && usb.rc == USB_NO_STICK) result(x, y, w, YELLOW, "No USB stick found");
        else if (usb.done && usb.rc == USB_FROZEN) {
            result(x, y, w, RED, "Not run");
            detail(x, y, w, 0, "it froze this console before");
        } else if (usb.done && usb.rc == USB_NO_JOURNAL) {
            result(x, y, w, YELLOW, "Not run");
            detail(x, y, w, 0, "the app folder is not writable: no journal");
        } else if (usb.done) {
            result(x, y, w, YELLOW, "Failed");
            detail(x, y, w, 0, "%s: rc 0x%08x", usb.dir, (unsigned)usb.rc);
        } else result(x, y, w, DIM, "Not tested");

        x += w + 32;
        char desc[240];
        snprintf(desc, sizeof desc, "Downloads 50 MB over plain HTTP from %.60s. Another file: its http:// URL in net_url.txt "
                 "in the app folder.", dl.host);
        test_card(x, y, w, h, BTN_TRIANGLE, "Internet", desc);
        if (dl.done && dl.rc == 0) {
            result(x, y, w, GREEN, "%.0f Mbit/s", mbit(dl.bytes, dl.sec));
            detail(x, y, w, 0, "%.1f MB in %.1f s, HTTP %d", dl.bytes / 1e6, dl.sec, dl.http);
        } else if (dl.done) {
            result(x, y, w, YELLOW, "Failed");
            detail(x, y, w, 0, "%s", dl.note);
        } else result(x, y, w, DIM, "Not tested");

        x += w + 32;
        test_card(x, y, w, h, BTN_SQUARE, "Local network",
                 "The app listens on port 5201 for two minutes and receives for 10 s. On a PC in the same network, run:");
        text_fit(x + 32, y + 236, F_BODY, BLUE, w - 64, "nc %s %d < /dev/zero", my_ip, LAN_PORT);
        text_fit(x + 32, y + 276, F_SMALL, GREY, w - 64, "%s", "macOS, Linux; on Windows ncat from Nmap.");
        if (lan.done && lan.rc == 0) {
            result(x, y, w, GREEN, "%.0f Mbit/s", mbit(lan.bytes, lan.sec));
            detail(x, y, w, 0, "%.1f MB in %.1f s from the PC", lan.bytes / 1e6, lan.sec);
        } else if (lan.done) {
            result(x, y, w, YELLOW, "Failed");
            detail(x, y, w, 0, "%s", lan.note);
        } else result(x, y, w, DIM, "Not tested");

        text_fit(MG, TOP + h + 24, F_BODY, GREY, SW - 2 * MG, "%s",
                 "The wired port is 1 Gbit/s on every PS3. Wi-Fi is 802.11g: 54 Mbit/s at most, often half of it.");
        footer("CROSS USB stick  TRIANGLE internet  SQUARE LAN from a PC  CIRCLE home  START exit");
        report_footnote();
        ui_flip();
    }
}

void mod_net_report(void)
{
    if (!usb.done && !dl.done && !lan.done) return;
    rep_out("--- transfer ---\n");
    if (usb.done) {
        if (usb.rc == 0) rep_out("USB stick %s: write %.1f MB/s, read %.1f MB/s (%.0f MB file)\n", usb.dir, mbs(usb.mb, usb.wsec), mbs(usb.mb, usb.rsec), usb.mb);
        else if (usb.rc > 0) rep_out("USB stick: not run, %s\n", usb_why());
        else rep_out("USB stick %s: test failed, rc 0x%08x\n", usb.dir, (unsigned)usb.rc);
    }
    if (dl.done) {
        rep_out("internet: console %s, http://%s:%d%s: ", my_ip, dl.host, dl.port, dl.path);
        if (dl.rc == 0) rep_out("%.1f Mbit/s, %.0f bytes in %.2f s, HTTP %d\n", mbit(dl.bytes, dl.sec), dl.bytes, dl.sec, dl.http);
        else rep_out("failed, %s\n", dl.note);
    }
    if (lan.done) {
        if (lan.rc == 0) rep_out("LAN receive on port %d: %.1f Mbit/s, %.0f bytes in %.2f s\n", LAN_PORT, mbit(lan.bytes, lan.sec), lan.bytes, lan.sec);
        else rep_out("LAN receive: failed, %s\n", lan.note);
    }
}
