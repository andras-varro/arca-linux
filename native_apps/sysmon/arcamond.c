/* arcamond — the Monitor page's history daemon.
 *
 * Once a second it samples the CPU busy share, memory in use and the SoC
 * temperature, keeps the last MON_RING_N samples, and publishes them to
 * MON_RING_PATH (format: mon_ring.h) so control_panel's Monitor page can
 * draw the last two minutes the moment it opens.  Started at boot by
 * /etc/init.d/arcamond.
 *
 * What it must never cost or touch:
 *   - CPU: niced to 19, no threads, no SCHED_RR; one wakeup a second on an
 *     absolute monotonic deadline, so it never drifts into a busy loop.
 *   - the framebuffer, input devices, audio: none are opened.  The one LED
 *     it touches is red_led, and only while over-temperature (below).
 *   - a missing sensor is a "-" field, never an exit: a kernel without the
 *     bandgap driver has no thermal zone, and that is a normal unit.
 * SIGTERM / SIGINT: the ring file is removed and the process exits 0, so a
 * stopped daemon cannot leave a file a reader would take for live data (the
 * reader's freshness check is the second guard).
 *
 * The CPU figure is cpu_load.c's: busy ticks over WALL time, never over
 * /proc/stat's grand total, with the daemon's own ticks taken out.
 *
 * Over-temperature: overtemp_latch() (common/overtemp.h) with hysteresis
 * sets MON_FLAG_OVERTEMP in the ring header, which framebuffer.c turns into a
 * red square on every present, and lights red_led — re-asserted each second,
 * since apps also write that LED, and put back to its earlier value when the
 * latch clears or the daemon stops.  While FORCE_PATH exists the flag is set
 * regardless of the reading: the SSH-only way to test the square and the LED
 * without heating the SoC (touch it to force, rm it to release).
 */
#define FORCE_PATH "/var/run/arcamond.force_overtemp"
#include "control_panel/cpu_load.h"
#include "control_panel/soc_temp.h"
#include "sysmon/mon_ring.h"
#include "common/overtemp.h"
#include "common/hardware.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_ZONES 8

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* Whole small file into buf, NUL-terminated.  Bytes read, or -1. */
static int read_text(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

static void snap(CpuSnap *s) {
    char stat[256], self[512], up[64];
    s->valid = read_text("/proc/stat", stat, sizeof(stat)) > 0
            && read_text("/proc/self/stat", self, sizeof(self)) > 0
            && read_text("/proc/uptime", up, sizeof(up)) > 0
            && cpu_parse_stat(stat, &s->busy) == 0
            && cpu_parse_self(self, &s->self) == 0
            && cpu_parse_uptime(up, &s->wall_cs) == 0;
}

static int temp_read(int zone, int *mc) {
    char path[64], raw[32];
    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
    return read_text(path, raw, sizeof(raw)) > 0 && soc_temp_parse(raw, mc) == 0 ? 0 : -1;
}

/* The first zone whose temp reads, as monitor_page.c chooses; -1 for none. */
static int temp_find(void) {
    int mc;
    for (int n = 0; n < MAX_ZONES; n++)
        if (temp_read(n, &mc) == 0) return n;
    return -1;
}

/* Write whole, then rename over the old one: a reader sees one version. */
static void publish(const char *text, int len) {
    static const char tmp[] = MON_RING_PATH ".tmp";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    int ok = write(fd, text, (size_t)len) == len;
    close(fd);
    if (!ok || rename(tmp, MON_RING_PATH) != 0) unlink(tmp);
}

/* red_led while warning: led_saved is the RAW sysfs text to put back, empty
 * when the warning does not own the LED.  Lit through hw_set_led() so the
 * configured LED maximum applies; saved and restored raw, because the
 * helpers' percent scaling does not round-trip (30 came back as 29 at 70 %,
 * measured on .188). */
#define RED_LED_SYSFS "/sys/class/leds/red_led/brightness"
static char led_saved[16];

static void warn_led(int on) {
    if (on) {
        if (!led_saved[0] && read_text(RED_LED_SYSFS, led_saved, sizeof(led_saved)) <= 0)
            strcpy(led_saved, "0");
        if (hw_get_led(LED_RED) != 100) hw_set_led(LED_RED, 100);
    } else if (led_saved[0]) {
        int fd = open(RED_LED_SYSFS, O_WRONLY);
        if (fd >= 0) {
            if (write(fd, led_saved, strlen(led_saved)) < 0) { /* best effort */ }
            close(fd);
        }
        led_saved[0] = '\0';
    }
}

int main(void) {
    static MonRing ring;
    static char text[MON_RING_TEXT_MAX];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    setpriority(PRIO_PROCESS, 0, 19);
    setvbuf(stdout, NULL, _IOLBF, 0);

    long tck = sysconf(_SC_CLK_TCK);
    unsigned clk_tck = tck > 0 ? (unsigned)tck : 100;
    int zone = temp_find();
    printf("arcamond: started, %d s ring at %s, thermal zone %d\n",
           MON_RING_N, MON_RING_PATH, zone);

    mon_ring_clear(&ring);
    CpuSnap prev, cur;
    snap(&prev);
    int hot = 0, warned = 0;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (running) {
        next.tv_sec += 1;
        while (running && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
        if (!running) break;

        MonSample s = { MON_ABSENT, MON_ABSENT, MON_ABSENT };
        int others, self;
        snap(&cur);
        if (cpu_share(&prev, &cur, clk_tck, &others, &self) == 0) s.cpu_pm = others;
        prev = cur;

        char mem[1024];
        int used;
        if (read_text("/proc/meminfo", mem, sizeof(mem)) > 0 && mon_parse_meminfo(mem, &used) == 0)
            s.mem_kb = used;

        int mc;
        if (zone >= 0 && temp_read(zone, &mc) == 0) s.temp_mc = mc;

        mon_ring_push(&ring, &s);
        hot = overtemp_latch(hot, s.temp_mc);
        int warn = hot || access(FORCE_PATH, F_OK) == 0;
        if (warn != warned)
            printf("arcamond: over-temperature warning %s (%d mC%s)\n", warn ? "on" : "off",
                   s.temp_mc, hot == warn ? "" : ", forced");
        warned = warn;
        warn_led(warn);
        unsigned flags = warn ? MON_FLAG_OVERTEMP : 0u;
        int len = cur.valid ? mon_ring_format(&ring, cur.wall_cs, flags, text, sizeof(text)) : -1;
        if (len > 0) publish(text, len);

        /* A suspend or a stopped process must not make us replay missed
         * seconds back to back: re-anchor if we fell more than a tick behind. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;
    }
    unlink(MON_RING_PATH);
    warn_led(0);
    printf("arcamond: stopped\n");
    return 0;
}
