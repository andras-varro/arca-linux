/* monitor_page.c — control_panel's Monitor page: uptime, load, SoC temperature,
 * memory and storage as text, and CPU, memory and SoC temperature history as
 * graphs (beside the text in landscape, under it in portrait).
 *
 * Opened from the home grid's Monitor tile, and the one home for the live
 * system figures.  Exposed only as
 * cp_monitor_page (cp_page.h); its state lives in this file.
 *
 * Display only: nothing is saved, so there is no reset_defaults.  The figures
 * are sampled on entry and then once a second from input(), which the main
 * loop calls every iteration; draw() paints the last sample and reads nothing,
 * so a repaint for any other reason costs no /proc reads.
 *
 * The three graphs ride the same once-a-second sample: one point per sample,
 * so they add no repaint of their own.  One widget draws all three
 * (draw_graph); each series reaches it as permille of its axis (mon_graph.h)
 * in a cpu_load.c ring.  On entry the graphs are pre-filled from arcamond's
 * published history when the daemon is running (graphs_seed_from_daemon);
 * without it they start empty.
 *
 * The SoC TEMP row and graph exist only when a thermal zone reads
 * (soc_temp.h): on a kernel without the bandgap driver neither is drawn, and
 * what is below moves up — an absent figure is removed, not greyed.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "cpu_load.h"
#include "mon_graph.h"
#include "soc_temp.h"
#include "../sysmon/mon_ring.h"
#include "../common/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define MON_REFRESH_MS 1000
/* Each graph's span in minutes: one bar per refresh, CPU_HIST_N bars. */
#define MON_GRAPH_SPAN_MIN (CPU_HIST_N * MON_REFRESH_MS / 60000)

/* ── Sampling ──────────────────────────────────────────────────────────── */

typedef struct {
    unsigned long total_kb, free_kb, available_kb;
    unsigned long buffers_kb, cached_kb;
    unsigned long swap_total_kb, swap_free_kb;
} MemInfo;

typedef struct {
    unsigned long total_kb, free_kb, used_kb;
    int valid;
} DiskInfo;

static const char *mount_points[] = {
    "/",
    "/home/root/data",
    "/home/root/log"
};
#define NUM_MOUNT_POINTS ((int)(sizeof(mount_points) / sizeof(mount_points[0])))

static void read_meminfo(MemInfo *info) {
    memset(info, 0, sizeof(*info));
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long val = 0;
        if (sscanf(line, "MemTotal: %lu kB", &val) == 1) info->total_kb = val;
        else if (sscanf(line, "MemFree: %lu kB", &val) == 1) info->free_kb = val;
        else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1) info->available_kb = val;
        else if (sscanf(line, "Buffers: %lu kB", &val) == 1) info->buffers_kb = val;
        else if (sscanf(line, "Cached: %lu kB", &val) == 1) info->cached_kb = val;
        else if (sscanf(line, "SwapTotal: %lu kB", &val) == 1) info->swap_total_kb = val;
        else if (sscanf(line, "SwapFree: %lu kB", &val) == 1) info->swap_free_kb = val;
    }
    fclose(fp);
}

static int read_disk_usage(const char *mp, DiskInfo *info) {
    memset(info, 0, sizeof(*info));
    struct statvfs st;
    if (statvfs(mp, &st) != 0) { info->valid = 0; return -1; }
    info->total_kb = (unsigned long)((unsigned long long)st.f_blocks * st.f_frsize / 1024);
    info->free_kb  = (unsigned long)((unsigned long long)st.f_bfree  * st.f_frsize / 1024);
    info->used_kb  = info->total_kb - info->free_kb;
    info->valid    = 1;
    return 0;
}

/* The three averages only: the run-queue and last-PID fields that follow them
 * in /proc/loadavg do not fit a portrait row at scale 2. */
static void read_loadavg(char *buf, size_t len) {
    char raw[128], a[16], b[16], c[16];
    if (read_file_line("/proc/loadavg", raw, sizeof(raw)) < 0
        || sscanf(raw, "%15s %15s %15s", a, b, c) != 3)
        snprintf(buf, len, "N/A");
    else
        snprintf(buf, len, "%s %s %s", a, b, c);
}

/* Seconds as "Nd Nh Nm Ns"; the receipt formats its worst case through here. */
static void format_uptime(int t, char *buf, size_t len) {
    snprintf(buf, len, "%dd %dh %dm %ds",
             t / 86400, (t % 86400) / 3600, (t % 3600) / 60, t % 60);
}

static void read_uptime(char *buf, size_t len) {
    char raw[64];
    double secs = 0;
    if (read_file_line("/proc/uptime", raw, sizeof(raw)) < 0
        || sscanf(raw, "%lf", &secs) != 1)
        snprintf(buf, len, "N/A");
    else
        format_uptime((int)secs, buf, len);
}

static void format_bytes(unsigned long kb, char *buf, size_t len) {
    if (kb >= 1048576) snprintf(buf, len, "%.1f GB", (double)kb / 1048576.0);
    else if (kb >= 1024) snprintf(buf, len, "%.1f MB", (double)kb / 1024.0);
    else snprintf(buf, len, "%lu KB", kb);
}

/* The meter lines, one formatter each, shared by draw() and the receipt so the
 * width measured is the width drawn. */
static void fmt_used_of(const char *name, unsigned long used, unsigned long total,
                        char *buf, size_t len) {
    char u[32], t[32];
    format_bytes(used, u, sizeof(u));
    format_bytes(total, t, sizeof(t));
    snprintf(buf, len, "%s  USED %s OF %s", name, u, t);
}

static void fmt_ram_detail(unsigned long free_kb, unsigned long avail_kb,
                           unsigned long cache_kb, char *buf, size_t len) {
    char f[32], a[32], c[32];
    format_bytes(free_kb, f, sizeof(f));
    format_bytes(avail_kb, a, sizeof(a));
    format_bytes(cache_kb, c, sizeof(c));
    snprintf(buf, len, "FREE %s  AVAILABLE %s  BUF/CACHE %s", f, a, c);
}

static void fmt_disk(const char *mp, unsigned long used, unsigned long total,
                     unsigned long free_kb, char *buf, size_t len) {
    char u[32], t[32], f[32];
    format_bytes(used, u, sizeof(u));
    format_bytes(total, t, sizeof(t));
    format_bytes(free_kb, f, sizeof(f));
    snprintf(buf, len, "%s  %s / %s  (%s FREE)", mp, u, t, f);
}

/* The CPU graph's caption, permille in; -1 = no sample yet.  The panel's own
 * cost is shown here and kept out of the bars, so the graph is the rest of the
 * device (cpu_load.h). */
static void fmt_cpu(int others_pm, int self_pm, char *buf, size_t len) {
    if (others_pm < 0 || self_pm < 0)
        snprintf(buf, len, "CPU --  LAST %d MIN  (PANEL --, NOT GRAPHED)", MON_GRAPH_SPAN_MIN);
    else
        snprintf(buf, len, "CPU %d%%  LAST %d MIN  (PANEL %d%%, NOT GRAPHED)",
                 (others_pm + 5) / 10, MON_GRAPH_SPAN_MIN, (self_pm + 5) / 10);
}

/* The memory graph's caption.  The series is MemTotal - MemAvailable — the
 * figure arcamond records, so a seeded graph and a sampled one are one series;
 * it is not the RAM row's total - free - buffers - cached.  used < 0 = none. */
static void fmt_mem_graph(long used_kb, unsigned long total_kb, char *buf, size_t len) {
    char u[32], t[32];
    format_bytes(total_kb, t, sizeof(t));
    if (used_kb < 0) snprintf(u, sizeof(u), "--");
    else             format_bytes((unsigned long)used_kb, u, sizeof(u));
    snprintf(buf, len, "MEM IN USE %s OF %s  LAST %d MIN", u, t, MON_GRAPH_SPAN_MIN);
}

/* The temperature graph's caption: the newest reading and the fixed axis. */
static void fmt_temp_graph(bool ok, int mc, char *buf, size_t len) {
    char v[32];
    if (ok) soc_temp_format(mc, v, sizeof(v));
    else    snprintf(v, sizeof(v), "--");
    snprintf(buf, len, "SOC %s  AXIS %d-%d C  LAST %d MIN", v,
             MON_TEMP_AXIS_LO_MC / 1000, MON_TEMP_AXIS_HI_MC / 1000, MON_GRAPH_SPAN_MIN);
}

static struct {
    char     uptime[64];
    char     load[64];
    MemInfo  mem;
    DiskInfo disk[NUM_MOUNT_POINTS];
} sample;
static uint32_t sampled_ms;

/* One history graph: where layout() put it, and its series as permille of the
 * series' own axis (mon_graph.h), so draw_graph() is the same for all three. */
typedef struct {
    int        x, label_y, y, w, h;   /* caption at (x, label_y); frame at (x, y) */
    bool       shown;
    CpuHistory hist;
} MonGraph;

static MonGraph g_cpu, g_mem, g_temp;
static MonGraph *const graphs[] = { &g_cpu, &g_mem, &g_temp };
#define NUM_GRAPHS ((int)(sizeof(graphs) / sizeof(graphs[0])))

/* CPU: the last snap and the newest shares (-1 = none yet). */
static CpuSnap cpu_prev;
static int     cpu_others_pm = -1, cpu_self_pm = -1;
static long    mem_used_kb = -1;    /* the memory graph's newest, -1 = none */

static void cpu_snap(CpuSnap *s) {
    char stat[256], self[512], up[64];
    s->valid = read_file_line("/proc/stat", stat, sizeof(stat)) == 0
            && read_file_line("/proc/self/stat", self, sizeof(self)) == 0
            && read_file_line("/proc/uptime", up, sizeof(up)) == 0
            && cpu_parse_stat(stat, &s->busy) == 0
            && cpu_parse_self(self, &s->self) == 0
            && cpu_parse_uptime(up, &s->wall_cs) == 0;
}

/* One point per call: the share since the previous call. */
static void cpu_sample(void) {
    static long clk_tck;
    CpuSnap cur;
    if (clk_tck <= 0) clk_tck = sysconf(_SC_CLK_TCK);
    cpu_snap(&cur);
    if (clk_tck > 0
        && cpu_share(&cpu_prev, &cur, (unsigned)clk_tck,
                     &cpu_others_pm, &cpu_self_pm) == 0)
        cpu_hist_push(&g_cpu.hist, cpu_others_pm);
    cpu_prev = cur;
}

/* SoC temperature: which thermal zone, and its newest reading. */
#define MON_MAX_ZONES 8
static int  temp_zone = -2;      /* -2 not looked up yet, -1 none, else N */
static int  temp_mc;
static bool temp_ok;

static int temp_read_zone(int n, int *mc) {
    char path[64], raw[32];
    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", n);
    return read_file_line(path, raw, sizeof(raw)) == 0
        && soc_temp_parse(raw, mc) == 0 ? 0 : -1;
}

/* Whether the row and the graph exist.  The zone is looked up once — the
 * first whose temp reads and parses — because the sensor driver is built into
 * the image, so the answer cannot change within a boot; layout() and the
 * sampler both ask. */
static bool temp_present(void) {
    if (temp_zone == -2) {
        temp_zone = -1;
        for (int n = 0; n < MON_MAX_ZONES; n++)
            if (temp_read_zone(n, &temp_mc) == 0) { temp_zone = n; break; }
    }
    return temp_zone >= 0;
}

/* RTC backup cell: the TWL4030 MADC channel 9, in millivolts.  The iio device
 * is found by its name once, like the thermal zone; absent file, absent row. */
#define MON_MAX_IIO 8
static int  rtc_dev = -2;        /* -2 not looked up yet, -1 none, else N */
static int  rtc_mv;
static bool rtc_ok;

static int rtc_read_dev(int n, int *mv) {
    char path[80], raw[32];
    long v;
    char *end;
    snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%d/in_voltage9_input", n);
    if (read_file_line(path, raw, sizeof(raw)) != 0) return -1;
    v = strtol(raw, &end, 10);
    if (end == raw || v < 0 || v > 9999) return -1;
    *mv = (int)v;
    return 0;
}

static bool rtc_present(void) {
    if (rtc_dev == -2) {
        rtc_dev = -1;
        for (int n = 0; n < MON_MAX_IIO; n++) {
            char path[64], name[32];
            snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device%d/name", n);
            if (read_file_line(path, name, sizeof(name)) == 0
                && strstr(name, "madc") && rtc_read_dev(n, &rtc_mv) == 0) {
                rtc_dev = n;
                break;
            }
        }
    }
    return rtc_dev >= 0;
}

static void monitor_sample(void) {
    read_uptime(sample.uptime, sizeof(sample.uptime));
    read_loadavg(sample.load, sizeof(sample.load));
    temp_ok = temp_present() && temp_read_zone(temp_zone, &temp_mc) == 0;
    rtc_ok = rtc_present() && rtc_read_dev(rtc_dev, &rtc_mv) == 0;
    read_meminfo(&sample.mem);
    for (int i = 0; i < NUM_MOUNT_POINTS; i++)
        read_disk_usage(mount_points[i], &sample.disk[i]);
}

/* The memory and temperature points of the newest sample, taken on the same
 * tick as the CPU point.  A missing MemAvailable or a failed zone read adds
 * no point, rather than a zero. */
static void graphs_sample(void) {
    const MemInfo *mi = &sample.mem;
    if (mi->total_kb > 0 && mi->available_kb > 0 && mi->available_kb <= mi->total_kb) {
        mem_used_kb = (long)(mi->total_kb - mi->available_kb);
        cpu_hist_push(&g_mem.hist, mon_scale_pm(mem_used_kb, 0, (long long)mi->total_kb));
    } else {
        mem_used_kb = -1;
    }
    if (temp_ok)
        cpu_hist_push(&g_temp.hist,
                      mon_scale_pm(temp_mc, MON_TEMP_AXIS_LO_MC, MON_TEMP_AXIS_HI_MC));
}

/* arcamond's ring (sysmon/mon_ring.h), if the daemon is running: its three
 * columns pre-fill the three graphs so the page opens on the last two
 * minutes.  A file older than 3 s means the daemon is gone, and the page then
 * starts empty and samples for itself exactly as without it.  The daemon's CPU
 * figure excludes its own ticks where this page excludes the panel's; both
 * are the same wall-time share (cpu_load.h).  Memory is scaled against this
 * page's MemTotal, which monitor_sample() has read before this runs. */
static void graphs_seed_from_daemon(unsigned long long now_cs) {
    static char text[MON_RING_TEXT_MAX];
    static MonRing ring;
    unsigned long long file_cs;
    unsigned flags;
    FILE *f = fopen(MON_RING_PATH, "r");
    if (!f) return;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    if (mon_ring_parse(text, &ring, &file_cs, &flags) != 0
        || !mon_ring_fresh(file_cs, now_cs, 300))
        return;
    long long total = (long long)sample.mem.total_kb;
    bool temp = temp_present();
    for (int i = 0; i < ring.count; i++) {
        const MonSample *s = mon_ring_get(&ring, i);
        if (s->cpu_pm != MON_ABSENT) cpu_hist_push(&g_cpu.hist, s->cpu_pm);
        if (s->mem_kb != MON_ABSENT && total > 0)
            cpu_hist_push(&g_mem.hist, mon_scale_pm(s->mem_kb, 0, total));
        if (s->temp_mc != MON_ABSENT && temp)
            cpu_hist_push(&g_temp.hist,
                          mon_scale_pm(s->temp_mc, MON_TEMP_AXIS_LO_MC, MON_TEMP_AXIS_HI_MC));
    }
}

/* The page was closed, so nothing sampled the gap: start the graphs afresh
 * rather than draw one point averaged over however long that was — from
 * arcamond's history when it is fresh, else empty. */
static void graphs_restart(void) {
    for (int i = 0; i < NUM_GRAPHS; i++) cpu_hist_clear(&graphs[i]->hist);
    cpu_others_pm = cpu_self_pm = -1;
    mem_used_kb = -1;
    cpu_snap(&cpu_prev);
    if (cpu_prev.valid) graphs_seed_from_daemon(cpu_prev.wall_cs);
}

/* Nothing to read from the Config; sampling here only means no frame can ever
 * show the zeroed struct. */
static void monitor_page_load(const Config *cfg) {
    (void)cfg;
    monitor_sample();
    graphs_restart();
    sampled_ms = get_time_ms();
}

static void monitor_page_enter(void) {
    monitor_sample();
    graphs_restart();
    sampled_ms = get_time_ms();
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define MON_ROW_H      24   /* a scale-2 label/value row: natural pitch */
#define MON_ROW_MIN_H  20   /* tightest pitch (the text is 14-16 px tall) */
#define MON_HEADER_H   26   /* draw_section_header() and the space under it */
#define MON_LINE_H     12   /* a scale-1 text line and its gap to the next */
#define MON_BAR_H      16
#define MON_METER_GAP  10   /* natural gap between meters */
#define MON_GAP_MIN     6   /* tightest gap */
#define MON_LOAD_LABEL "LOAD AVG:"
#define MON_TEMP_LABEL "SOC TEMP:"   /* values align after the wider of these */
#define MON_RTC_LABEL  "RTC CELL:"
#define MON_CAP_H      (MON_LINE_H + 2)  /* a graph's caption line above it */
#define MON_GRAPH_GAP   8   /* between one graph's bottom and the next caption */
#define MON_GRAPH_MIN_H 24  /* below this a history graph is a smear */
#define MON_GRAPH_MAX_H 96  /* past this it only takes room from nothing */
#define MON_COL_GAP    20   /* text column to graph column */
#define MON_SIDE_MIN_W 240  /* graphs beside the text only if this wide */
#define MON_FIT_SLACK   4   /* left under the lowest thing when sizing graphs */
#define MON_TEXT_SLACK 12   /* left under the last storage bar when tightening the text */

static int  uptime_y, load_y, temp_y, rtc_y, value_x;
static int  mon_rows;   /* scale-2 rows at the top: 2, plus SOC TEMP and RTC CELL when present */
static bool temp_row, rtc_row;
static int  sec_mem_y, ram_y, swap_y, sec_disk_y;
static int  disk_y[NUM_MOUNT_POINTS];
static int  bar_x, bar_w, header_right;
static bool graphs_beside;

/* The worst-case uptime, formatted the way the row draws it. */
static void worst_uptime(char *buf, size_t len) {
    format_uptime(999 * 86400 + 23 * 3600 + 59 * 60 + 59, buf, len);
}

/* The text column's right edge: its widest line, each formatted through the
 * draw path's own formatter with worst-case values (1048575 kB is
 * format_bytes()'s longest, "1024.0 MB").  value_x and bar_x must be set. */
static int text_right_edge(void) {
    const unsigned long W = 1048575;
    char s[160];
    int right = 0, w;
    worst_uptime(s, sizeof(s));
    w = value_x + text_measure_width(s, 2);                       if (w > right) right = w;
    w = value_x + text_measure_width("99.99 99.99 99.99", 2);     if (w > right) right = w;
    if (temp_row) {
        soc_temp_format(SOC_TEMP_MIN_MC, s, sizeof(s));
        w = value_x + text_measure_width(s, 2);                   if (w > right) right = w;
    }
    fmt_used_of("SWAP", W, W, s, sizeof(s));
    w = bar_x + text_measure_width(s, 1);                         if (w > right) right = w;
    fmt_ram_detail(W, W, W, s, sizeof(s));
    w = bar_x + text_measure_width(s, 1);                         if (w > right) right = w;
    for (int i = 0; i < NUM_MOUNT_POINTS; i++) {
        fmt_disk(mount_points[i], W, W, W, s, sizeof(s));
        w = bar_x + text_measure_width(s, 1);                     if (w > right) right = w;
    }
    return right;
}

/* A meter is `lines` scale-1 text lines, then its bar; returns the y after it. */
static int meter_bottom(int y, int lines) {
    return y + lines * MON_LINE_H + MON_BAR_H;
}

/* The text column from the top: UPTIME, LOAD, SOC TEMP and RTC CELL (if present), then
 * MEMORY and STORAGE.  Returns the y under the last storage bar. */
static int place_text(int row_h, int gap) {
    int y = CONTENT_Y + CONTENT_TOP_GAP;
    uptime_y = y;
    load_y   = y + row_h;
    temp_y   = y + 2 * row_h;   /* each drawn only when its row is present */
    rtc_y    = y + (2 + temp_row) * row_h;
    y += mon_rows * row_h + 6;

    sec_mem_y = y;                y += MON_HEADER_H;
    ram_y     = y;                y = meter_bottom(y, 2) + gap;
    swap_y    = y;                y = meter_bottom(y, 1) + gap;

    sec_disk_y = y;               y += MON_HEADER_H;
    for (int i = 0; i < NUM_MOUNT_POINTS; i++) {
        disk_y[i] = y;
        y = meter_bottom(y, 1);
        if (i < NUM_MOUNT_POINTS - 1) y += gap;
    }
    return y;
}

static int mon_gap = MON_METER_GAP;   /* the meter gap place_text_fit() settled on */

/* place_text() at the natural spacing, tightened one pixel at a time only as far
 * as needed to end MON_TEXT_SLACK above the content bottom: the meter gap down to
 * MON_GAP_MIN first, then the row pitch down to MON_ROW_MIN_H.  Spacing is the
 * only thing that gives; nothing is dropped.  Returns the y under the last bar
 * (past the limit when even the tightest spacing does not fit, which the
 * receipt then reports). */
static int place_text_fit(void) {
    const int limit = CONTENT_Y + CONTENT_H - MON_TEXT_SLACK;
    int row_h = MON_ROW_H, gap = MON_METER_GAP;
    int y = place_text(row_h, gap);
    while (y > limit && (gap > MON_GAP_MIN || row_h > MON_ROW_MIN_H)) {
        if (gap > MON_GAP_MIN) gap--; else row_h--;
        y = place_text(row_h, gap);
    }
    mon_gap = gap;
    return y;
}

/* The shown graphs stacked from y0 in a column at x, w wide, sharing the
 * height down to the content bottom equally, each between MON_GRAPH_MIN_H and
 * MON_GRAPH_MAX_H.  Returns the y under the last graph. */
static int place_graphs(int x, int w, int y0) {
    int n = 0;
    for (int i = 0; i < NUM_GRAPHS; i++) if (graphs[i]->shown) n++;
    int avail = CONTENT_Y + CONTENT_H - MON_FIT_SLACK - y0;
    int h = (avail - n * MON_CAP_H - (n - 1) * MON_GRAPH_GAP) / n;
    if (h > MON_GRAPH_MAX_H) h = MON_GRAPH_MAX_H;
    if (h < MON_GRAPH_MIN_H) h = MON_GRAPH_MIN_H;
    int y = y0;
    for (int i = 0; i < NUM_GRAPHS; i++) {
        MonGraph *g = graphs[i];
        if (!g->shown) continue;
        g->x = x; g->w = w; g->h = h;
        g->label_y = y;
        g->y = y + MON_CAP_H;
        y = g->y + h + MON_GRAPH_GAP;
    }
    return y - MON_GRAPH_GAP;
}

/* Re-run whenever the logical screen changes (rebuild_ui()).  Two columns
 * when there is room — the text left, the graphs (CPU, memory, SoC
 * temperature) stacked right — which is landscape; otherwise one column, the
 * graphs under the text, full width, which is portrait.  The temperature
 * graph exists only with a thermal zone, like its row: absent is removed. */
static void monitor_page_layout(void) {
    temp_row = temp_present();
    rtc_row  = rtc_present();
    mon_rows = 2 + temp_row + rtc_row;
    g_cpu.shown = g_mem.shown = true;
    g_temp.shown = temp_row;
    int label_w = text_measure_width(MON_LOAD_LABEL, 2);
    if (temp_row && text_measure_width(MON_TEMP_LABEL, 2) > label_w)
        label_w = text_measure_width(MON_TEMP_LABEL, 2);
    if (rtc_row && text_measure_width(MON_RTC_LABEL, 2) > label_w)
        label_w = text_measure_width(MON_RTC_LABEL, 2);
    value_x = CONTENT_LEFT + 10 + label_w + 12;
    bar_x   = CONTENT_LEFT + 10;

    int text_right = text_right_edge();
    int col_x = text_right + MON_COL_GAP;
    graphs_beside = (CONTENT_RIGHT - 10) - col_x >= MON_SIDE_MIN_W;

    int text_bottom, graphs_bottom;
    if (graphs_beside) {
        bar_w = text_right - bar_x;
        header_right = text_right;
        text_bottom   = place_text_fit();
        graphs_bottom = place_graphs(col_x, (CONTENT_RIGHT - 10) - col_x, CONTENT_Y + CONTENT_TOP_GAP);
    } else {
        bar_w = CONTENT_WIDTH - 20;
        header_right = CONTENT_RIGHT;
        text_bottom   = place_text_fit();
        graphs_bottom = place_graphs(bar_x, bar_w, text_bottom + mon_gap + 6);
    }

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything here hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a row pushed past
     * the content rect looks fine on one unit and clips on another.  The lowest
     * thing drawn is the last storage bar or the last graph; the right edge is
     * the widest text line, bar, graph frame or caption, each caption
     * formatted through its draw path's formatter with worst-case values. */
    {
        const unsigned long W = 1048575;
        char s[160];
        int bottom = (text_bottom > graphs_bottom ? text_bottom : graphs_bottom) - CONTENT_Y;
        int right  = text_right, w, min_h = MON_GRAPH_MAX_H, gw = 0, n = 0;
        if (bar_x + bar_w > right) right = bar_x + bar_w;
        for (int i = 0; i < NUM_GRAPHS; i++) {
            const MonGraph *g = graphs[i];
            if (!g->shown) continue;
            n++;
            if (g->x + g->w > right) right = g->x + g->w;
            if (g->h < min_h) min_h = g->h;
            gw = g->w;
            if (g == &g_cpu)       fmt_cpu(1000, 1000, s, sizeof(s));
            else if (g == &g_mem)  fmt_mem_graph((long)W, W, s, sizeof(s));
            else                   fmt_temp_graph(true, SOC_TEMP_MIN_MC, s, sizeof(s));
            w = g->x + text_measure_width(s, 1);
            if (w > right) right = w;
        }
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : min_h < MON_GRAPH_MIN_H ? "⚠ GRAPH TOO SHORT"
                            : "fits";
        printf("control_panel: monitor stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d graphs %dx%d %s, soc temp %s, rtc cell %s (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT,
               n, gw, min_h, graphs_beside ? "beside" : "under",
               temp_row ? "shown" : "absent", rtc_row ? "shown" : "absent",
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

static void draw_row(Framebuffer *fb, int y, const char *label, const char *value) {
    fb_draw_text(fb, CONTENT_LEFT + 10, y, label, COLOR_LABEL, 2);
    fb_draw_text(fb, value_x, y, value, COLOR_WHITE, 2);
}

/* THE history graph, for all three series: the caption, then one column per
 * held sample, newest at the right edge, so a fresh page fills from the
 * right.  Coloured by level like a usage bar, with a dim rule at mid-axis.
 * Reads nothing — the samples are the samplers'. */
static void draw_graph(Framebuffer *fb, const MonGraph *g, const char *caption) {
    fb_draw_text(fb, g->x, g->label_y, caption, COLOR_WHITE, 1);
    fb_fill_rect(fb, g->x, g->y, g->w, g->h, RGB(8, 8, 14));
    fb_draw_rect(fb, g->x, g->y, g->w, g->h, RGB(70, 70, 90));
    int ix = g->x + 1, iy = g->y + 1, iw = g->w - 2, ih = g->h - 2;
    if (iw <= 0 || ih <= 0) return;
    fb_fill_rect(fb, ix, iy + ih / 2, iw, 1, RGB(45, 45, 60));

    int first = CPU_HIST_N - g->hist.count;    /* slot of the oldest sample */
    for (int i = 0; i < g->hist.count; i++) {
        int pm   = cpu_hist_get(&g->hist, i);
        int slot = first + i;
        int x0 = ix + slot * iw / CPU_HIST_N;
        int x1 = ix + (slot + 1) * iw / CPU_HIST_N;
        int h  = mon_bar_h(pm, ih);
        if (h <= 0 || x1 <= x0) continue;
        uint32_t c = pm >= 800 ? RGB(210, 70, 60)
                   : pm >= 500 ? RGB(220, 170, 40)
                   :             RGB(60, 180, 90);
        fb_fill_rect(fb, x0, iy + ih - h, x1 - x0, h, c);
    }
}

static void monitor_page_draw(Framebuffer *fb) {
    const MemInfo *mi = &sample.mem;
    char s[160];

    draw_row(fb, uptime_y, "UPTIME:", sample.uptime);
    draw_row(fb, load_y, MON_LOAD_LABEL, sample.load);
    if (temp_row) {
        /* The zone exists; a single failed read shows "--", not a stale figure. */
        if (temp_ok) soc_temp_format(temp_mc, s, sizeof(s));
        else         snprintf(s, sizeof(s), "--");
        draw_row(fb, temp_y, MON_TEMP_LABEL, s);
    }
    if (rtc_row) {
        if (rtc_ok) snprintf(s, sizeof(s), "%d.%02d V", rtc_mv / 1000, rtc_mv % 1000 / 10);
        else        snprintf(s, sizeof(s), "--");
        draw_row(fb, rtc_y, MON_RTC_LABEL, s);
    }

    fmt_cpu(cpu_others_pm, cpu_self_pm, s, sizeof(s));
    draw_graph(fb, &g_cpu, s);
    fmt_mem_graph(mem_used_kb, mi->total_kb, s, sizeof(s));
    draw_graph(fb, &g_mem, s);
    if (g_temp.shown) {
        fmt_temp_graph(temp_ok, temp_mc, s, sizeof(s));
        draw_graph(fb, &g_temp, s);
    }

    draw_section_header_to(fb, sec_mem_y, "MEMORY", header_right);
    unsigned long used = mi->total_kb - mi->free_kb - mi->buffers_kb - mi->cached_kb;
    if (used > mi->total_kb) used = 0;
    fmt_used_of("RAM", used, mi->total_kb, s, sizeof(s));
    fb_draw_text(fb, bar_x, ram_y, s, COLOR_WHITE, 1);
    fmt_ram_detail(mi->free_kb, mi->available_kb,
                   mi->buffers_kb + mi->cached_kb, s, sizeof(s));
    fb_draw_text(fb, bar_x, ram_y + MON_LINE_H, s, COLOR_LABEL, 1);
    draw_usage_bar(fb, bar_x, ram_y + 2 * MON_LINE_H, bar_w, MON_BAR_H,
                   used, mi->total_kb, "");

    if (mi->swap_total_kb == 0) {
        fb_draw_text(fb, bar_x, swap_y, "SWAP  NONE", COLOR_LABEL, 1);
    } else {
        unsigned long su = mi->swap_total_kb - mi->swap_free_kb;
        fmt_used_of("SWAP", su, mi->swap_total_kb, s, sizeof(s));
        fb_draw_text(fb, bar_x, swap_y, s, COLOR_WHITE, 1);
        draw_usage_bar(fb, bar_x, swap_y + MON_LINE_H, bar_w, MON_BAR_H,
                       su, mi->swap_total_kb, "");
    }

    draw_section_header_to(fb, sec_disk_y, "STORAGE", header_right);
    for (int i = 0; i < NUM_MOUNT_POINTS; i++) {
        const DiskInfo *di = &sample.disk[i];
        if (!di->valid) {
            snprintf(s, sizeof(s), "%s  N/A (NOT MOUNTED)", mount_points[i]);
            fb_draw_text(fb, bar_x, disk_y[i], s, RGB(200, 80, 80), 1);
            continue;
        }
        fmt_disk(mount_points[i], di->used_kb, di->total_kb, di->free_kb,
                 s, sizeof(s));
        fb_draw_text(fb, bar_x, disk_y[i], s, COLOR_WHITE, 1);
        draw_usage_bar(fb, bar_x, disk_y[i] + MON_LINE_H, bar_w, MON_BAR_H,
                       di->used_kb, di->total_kb, "");
    }
}

/* No widgets: the only thing that changes the screen is the clock.  The graph
 * points are taken on the same tick, so the graphs cost no repaint of their
 * own. */
static CpPageResult monitor_page_input(Config *cfg, int tx, int ty,
                                       bool touching, uint32_t now) {
    (void)cfg; (void)tx; (void)ty; (void)touching;
    if (now - sampled_ms < MON_REFRESH_MS) return CP_PAGE_IDLE;
    monitor_sample();
    cpu_sample();
    graphs_sample();
    sampled_ms = now;
    return CP_PAGE_REDRAW;
}

const CpPage cp_monitor_page = {
    .name   = "Monitor",
    .icon   = "cp_monitor",
    .load   = monitor_page_load,
    .layout = monitor_page_layout,
    .enter  = monitor_page_enter,
    .draw   = monitor_page_draw,
    .input  = monitor_page_input,
};
