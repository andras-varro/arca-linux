/* network_page.c — control_panel's Network page: how this unit is connected.
 *
 * Opened from the home grid's Network tile, and the one home for the network
 * figures: the default gateway, the DNS servers, and every interface under
 * /sys/class/net except loopback, each with its operstate, IPv4 address and
 * MAC.  Exposed only as cp_network_page (cp_page.h); its state lives here.
 * The hostname is the Information page's, so it is not repeated here.
 *
 * Live, but a static network costs nothing: the figures are read on entry and
 * then every NET_REFRESH_MS from input(), which returns CP_PAGE_REDRAW only
 * when the new reading differs from the one on screen.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_REFRESH_MS  2000
#define NET_MAX_IFACES  16
#define NET_MAX_DNS     3

/* ── Reading ───────────────────────────────────────────────────────────── */

typedef struct {
    char name[20];
    char state[20];      /* operstate, upper-cased: UP, DOWN, UNKNOWN, ... */
    char ip[20];         /* first IPv4 address, or "NO IPV4" */
    char mac[24];
} NetIface;

/* One reading.  Zeroed before every fill, so two readings of the same network
 * are byte-identical and memcmp() is the change test. */
typedef struct {
    char     gateway[48];
    char     dns[NET_MAX_DNS * 16 + 8];
    NetIface ifaces[NET_MAX_IFACES];
    int      iface_count;
} NetReading;

static NetReading net;
static uint32_t   sampled_ms;

static void upcase(char *s) {
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static int cmp_iface(const void *a, const void *b) {
    return strcmp(((const NetIface *)a)->name, ((const NetIface *)b)->name);
}

/* The default route's gateway and device, from /proc/net/route. */
static void read_gateway(NetReading *r) {
    snprintf(r->gateway, sizeof(r->gateway), "NONE");
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {          /* skip the header */
        while (fgets(line, sizeof(line), f)) {
            char iface[32];
            unsigned long dest, gateway;
            if (sscanf(line, "%31s %lx %lx", iface, &dest, &gateway) != 3 || dest != 0)
                continue;
            struct in_addr addr;
            char ip[INET_ADDRSTRLEN];
            addr.s_addr = (in_addr_t)gateway;
            inet_ntop(AF_INET, &addr, ip, sizeof(ip));
            snprintf(r->gateway, sizeof(r->gateway), "%s (%.15s)", ip, iface);   /* IFNAMSIZ - 1 */
            break;
        }
    }
    fclose(f);
}

/* Up to NET_MAX_DNS nameservers from /etc/resolv.conf, space-separated. */
static void read_dns(NetReading *r) {
    int n = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[256];
        while (n < NET_MAX_DNS && fgets(line, sizeof(line), f)) {
            char ns[64];
            if (sscanf(line, "nameserver %63s", ns) != 1) continue;
            size_t len = strlen(r->dns);
            snprintf(r->dns + len, sizeof(r->dns) - len, "%s%s", n ? "  " : "", ns);
            n++;
        }
        fclose(f);
    }
    if (n == 0) snprintf(r->dns, sizeof(r->dns), "NONE");
}

static void read_sys_attr(const char *ifname, const char *attr, char *buf, size_t len) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", ifname, attr);
    if (read_file_line(path, buf, len) < 0 || !buf[0]) snprintf(buf, len, "N/A");
}

/* Every interface but lo, sorted by name so the order is stable between reads. */
static void read_ifaces(NetReading *r) {
    DIR *d = opendir("/sys/class/net");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && r->iface_count < NET_MAX_IFACES) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, "lo") == 0) continue;
        NetIface *it = &r->ifaces[r->iface_count++];
        snprintf(it->name, sizeof(it->name), "%.15s", e->d_name);   /* IFNAMSIZ - 1 */
    }
    closedir(d);
    qsort(r->ifaces, (size_t)r->iface_count, sizeof(r->ifaces[0]), cmp_iface);

    for (int i = 0; i < r->iface_count; i++) {
        NetIface *it = &r->ifaces[i];
        read_sys_attr(it->name, "operstate", it->state, sizeof(it->state));
        upcase(it->state);
        read_sys_attr(it->name, "address", it->mac, sizeof(it->mac));
        snprintf(it->ip, sizeof(it->ip), "NO IPV4");
    }

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) == -1) return;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        for (int i = 0; i < r->iface_count; i++) {
            NetIface *it = &r->ifaces[i];
            if (strcmp(it->name, ifa->ifa_name) != 0 || strcmp(it->ip, "NO IPV4") != 0)
                continue;
            inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                      it->ip, sizeof(it->ip));
        }
    }
    freeifaddrs(ifaddr);
}

static void net_read(NetReading *r) {
    memset(r, 0, sizeof(*r));
    read_gateway(r);
    read_dns(r);
    read_ifaces(r);
}

/* Read here as well as in enter() so the receipt measures this unit's strings. */
static void network_page_load(const Config *cfg) {
    (void)cfg;
    net_read(&net);
}

static void network_page_enter(void) {
    net_read(&net);
    sampled_ms = get_time_ms();
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define NET_ROW_H      24   /* a scale-2 label/value row */
#define NET_HEADER_H   26   /* draw_section_header() and the space under it */
#define NET_GAP         6   /* between sections, and between interface blocks */
#define NET_WIDE_LABEL "INTERFACE:"   /* the widest label: values align after it */
#define NET_IFACE_ROWS  3   /* INTERFACE, IP ADDR, MAC */

static bool stacked;          /* portrait: value on the line under its label */
static int  value_x, row_step, block_h;
static int  sec_route_y, gw_y, dns_y, sec_if_y, if_y0;
static int  if_slots;         /* interface blocks that fit */

/* A label/value row's height: portrait puts the value on its own line. */
static int row_height(void) { return stacked ? 2 * NET_ROW_H : NET_ROW_H; }

/* Re-run whenever the logical screen changes (rebuild_ui()).  ROUTING sits at
 * the top at a fixed height; whatever is left under it goes to interface
 * blocks, and a list longer than the space ends in "+N MORE". */
static void network_page_layout(void) {
    stacked  = CONTENT_WIDTH < 600;
    value_x  = stacked ? CONTENT_LEFT + 30
                       : CONTENT_LEFT + 10 + text_measure_width(NET_WIDE_LABEL, 2) + 12;
    row_step = row_height();
    block_h  = NET_IFACE_ROWS * row_step + NET_GAP;

    int y = CONTENT_Y + CONTENT_TOP_GAP;
    sec_route_y = y;  y += NET_HEADER_H;
    gw_y  = y;        y += row_step;
    dns_y = y;        y += row_step;
    y += NET_GAP;
    sec_if_y = y;     y += NET_HEADER_H;
    if_y0 = y;

    if_slots = (CONTENT_Y + CONTENT_H - y) / block_h;
    if (if_slots > NET_MAX_IFACES) if_slots = NET_MAX_IFACES;
    if (if_slots < 1) if_slots = 1;
    y += if_slots * block_h;

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a row pushed past
     * the content rect looks fine on one unit and clips on another.  The bottom
     * is the last interface block the space allows.  Values are cut at
     * CONTENT_RIGHT by fit_value(), so the right edge cannot pass it; what the
     * receipt reports instead is how many of THIS unit's values were cut, with
     * the fixed-format rows also measured at their widest (a 15-character IPv4
     * address, a MAC). */
    {
        char s[160], cut[160];
        int bottom = y - CONTENT_Y;
        int right  = CONTENT_LEFT + 10 + text_measure_width(NET_WIDE_LABEL, 2);
        int clipped = 0;
        const char *vals[2 + 3 * NET_MAX_IFACES + 2];
        int nv = 0;
        vals[nv++] = net.gateway;
        vals[nv++] = net.dns;
        for (int i = 0; i < net.iface_count; i++) {
            vals[nv++] = net.ifaces[i].ip;
            vals[nv++] = net.ifaces[i].mac;
        }
        vals[nv++] = "255.255.255.255";
        vals[nv++] = "ff:ff:ff:ff:ff:ff";
        for (int i = 0; i < nv; i++) {
            if (fit_value(vals[i], value_x, 2, cut, sizeof(cut))) clipped++;
            int w = value_x + text_measure_width(cut, 2);
            if (w > right) right = w;
        }
        for (int i = 0; i < net.iface_count; i++) {
            snprintf(s, sizeof(s), "%s  %s", net.ifaces[i].name, net.ifaces[i].state);
            if (fit_value(s, value_x, 2, cut, sizeof(cut))) clipped++;
            int w = value_x + text_measure_width(cut, 2);
            if (w > right) right = w;
        }
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: network stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d value(s) cut, %d interface slot(s) "
               "for %d interface(s) (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT, clipped,
               if_slots, net.iface_count, SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               stacked ? "portrait" : "landscape");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

static void draw_row(Framebuffer *fb, int y, const char *label, const char *value,
                     uint32_t color) {
    char cut[160];
    fb_draw_text(fb, CONTENT_LEFT + 10, y, label, COLOR_LABEL, 2);
    fit_value(value, value_x, 2, cut, sizeof(cut));
    fb_draw_text(fb, value_x, stacked ? y + NET_ROW_H : y, cut, color, 2);
}

static uint32_t state_color(const char *state) {
    if (strcmp(state, "UP") == 0)   return COLOR_GREEN;
    if (strcmp(state, "DOWN") == 0) return COLOR_RED;
    return COLOR_YELLOW;
}

static void network_page_draw(Framebuffer *fb) {
    char s[64];

    draw_section_header(fb, sec_route_y, "ROUTING");
    draw_row(fb, gw_y,  "GATEWAY:", net.gateway, COLOR_WHITE);
    draw_row(fb, dns_y, "DNS:",     net.dns,     COLOR_WHITE);

    draw_section_header(fb, sec_if_y, "INTERFACES");
    if (net.iface_count == 0) {
        fb_draw_text(fb, CONTENT_LEFT + 10, if_y0, "NO INTERFACES", COLOR_LABEL, 2);
        return;
    }
    /* All that fit; if some do not, the last slot says how many are missing. */
    int shown = net.iface_count <= if_slots ? net.iface_count : if_slots - 1;
    for (int i = 0; i < shown; i++) {
        const NetIface *it = &net.ifaces[i];
        int y = if_y0 + i * block_h;
        snprintf(s, sizeof(s), "%s  %s", it->name, it->state);
        draw_row(fb, y,                "INTERFACE:", s,      state_color(it->state));
        draw_row(fb, y + row_step,     "  IP ADDR:", it->ip, COLOR_WHITE);
        draw_row(fb, y + 2 * row_step, "  MAC:",     it->mac, COLOR_WHITE);
    }
    if (shown < net.iface_count) {
        snprintf(s, sizeof(s), "+%d MORE", net.iface_count - shown);
        fb_draw_text(fb, CONTENT_LEFT + 10, if_y0 + shown * block_h, s, COLOR_LABEL, 2);
    }
}

/* No widgets.  Re-reads every NET_REFRESH_MS and asks for a repaint only when
 * the reading changed, so an idle page on a static network paints nothing. */
static CpPageResult network_page_input(Config *cfg, int tx, int ty,
                                       bool touching, uint32_t now) {
    (void)cfg; (void)tx; (void)ty; (void)touching;
    if (now - sampled_ms < NET_REFRESH_MS) return CP_PAGE_IDLE;
    sampled_ms = now;
    static NetReading fresh;   /* ~1 KB: off the stack */
    net_read(&fresh);
    if (memcmp(&fresh, &net, sizeof(net)) == 0) return CP_PAGE_IDLE;
    net = fresh;
    return CP_PAGE_REDRAW;
}

const CpPage cp_network_page = {
    .name   = "Network",
    .icon   = "cp_network",
    .load   = network_page_load,
    .layout = network_page_layout,
    .enter  = network_page_enter,
    .draw   = network_page_draw,
    .input  = network_page_input,
};
