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
 *     it touches is red_led, and only while over-temperature or a USB fault.
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
 *
 * A latched USB fault (usbfault.h) flashes the same LED in the cadence of the
 * screen's red sidebars: three 200 ms flashes at the start of every 10 s,
 * the wait cut into 200 ms slots only while it flashes.  Over-temperature, solid,
 * wins; one save and one restore serve both.
 */
#define FORCE_PATH "/var/run/arcamond.force_overtemp"
#include "control_panel/cpu_load.h"
#include "control_panel/soc_temp.h"
#include "sysmon/mon_ring.h"
#include "common/overtemp.h"
#include "common/usbfault.h"
#include "common/hardware.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
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
 * when no warning owns the LED.  Lit or darkened through hw_set_led() so the
 * configured LED maximum applies; saved and restored raw, because the
 * helpers' percent scaling does not round-trip (30 came back as 29 at 70 %,
 * measured on .188).  One owner for both warnings: mode is usbf_led()'s
 * answer (release, lit, or dark for a fault flash's off phase). */
#define RED_LED_SYSFS "/sys/class/leds/red_led/brightness"
static char led_saved[16];

static void warn_led(int mode) {
    if (mode != USBF_LED_RELEASE) {
        int want = mode == USBF_LED_LIT ? 100 : 0;
        if (!led_saved[0] && read_text(RED_LED_SYSFS, led_saved, sizeof(led_saved)) <= 0)
            strcpy(led_saved, "0");
        if (hw_get_led(LED_RED) != want) hw_set_led(LED_RED, want);
    } else if (led_saved[0]) {
        int fd = open(RED_LED_SYSFS, O_WRONLY);
        if (fd >= 0) {
            if (write(fd, led_saved, strlen(led_saved)) < 0) { /* best effort */ }
            close(fd);
        }
        led_saved[0] = '\0';
    }
}

/* USB overload recovery (common/usbfault.h holds the decisions).  The kernel
 * logs VBUS_ERROR when an overcurrent drops the musb session and never raises
 * VBUS again; /etc/init.d/usb-host recover rebinds musb.  /dev/kmsg is opened
 * once and positioned at its end, so lines from before the daemon started are
 * ignored; EPIPE (records overwritten) just means read on. */
#define USB_RECOVER "/etc/init.d/usb-host"

static int kmsg_open(void) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) lseek(fd, 0, SEEK_END);
    return fd;
}

/* How many records read since the last call carry VBUS_ERROR. */
static int kmsg_vbus_error(int fd) {
    char rec[1024];
    int seen = 0;
    if (fd < 0) return 0;
    for (int i = 0; i < 64; i++) {
        ssize_t n = read(fd, rec, sizeof(rec) - 1);
        if (n < 0) {
            if (errno == EPIPE || errno == EINTR) continue;
            break;   /* EAGAIN: drained */
        }
        if (n == 0) break;
        rec[n] = '\0';
        if (strstr(rec, "VBUS_ERROR")) seen++;
    }
    return seen;
}

/* A non-hub device is enumerated: skip root hubs (usbN), interfaces (a name
 * with ':') and devices whose bDeviceClass is 09. */
static int usb_peripheral_present(void) {
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strncmp(e->d_name, "usb", 3) == 0
            || strchr(e->d_name, ':'))
            continue;
        char path[300], cls[16];
        snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/bDeviceClass", e->d_name);
        if (read_text(path, cls, sizeof(cls)) < 2) continue;
        if (strncmp(cls, "09", 2) != 0) found = 1;
    }
    closedir(d);
    return found;
}

static pid_t recover_pid;

/* Reaps a finished recover; 1 while one is alive. */
static int recover_running(void) {
    if (recover_pid > 0 && waitpid(recover_pid, NULL, WNOHANG) != 0)
        recover_pid = 0;   /* exited, or no such child */
    return recover_pid > 0;
}

static void recover_start(const char *verb) {
    pid_t p = fork();
    if (p == 0) {
        execl(USB_RECOVER, "usb-host", verb, (char *)NULL);
        _exit(127);
    }
    if (p > 0) recover_pid = p;
}

static uint32_t mono_ms(void) {
    struct timespec tn;
    clock_gettime(CLOCK_MONOTONIC, &tn);
    return (uint32_t)tn.tv_sec * 1000u + (uint32_t)(tn.tv_nsec / 1000000);
}

static void sleep_until(const struct timespec *t) {
    while (running && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR)
        ;
}

/* The wait for the next one-second deadline.  While a fault flashes the LED
 * it is cut at every 200 ms slot boundary of the monotonic clock, so the LED
 * follows usbf_led()'s slots (the sidebars' cadence) instead of the 1 s tick. */
static void wait_tick(const struct timespec *next, int flash) {
    while (flash && running) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long long left = ((long long)next->tv_sec - t.tv_sec) * 1000000000LL
                       + (next->tv_nsec - t.tv_nsec);
        long long step = (long long)(USBF_FLASH_PHASE_MS - mono_ms() % USBF_FLASH_PHASE_MS)
                       * 1000000LL;
        if (left <= step) break;
        t.tv_nsec += (long)(step % 1000000000LL);
        t.tv_sec += (time_t)(step / 1000000000LL);
        if (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; t.tv_sec++; }
        sleep_until(&t);
        if (running) warn_led(usbf_led(0, 1, mono_ms()));
    }
    sleep_until(next);
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
    int hot = 0, warned = 0, flashing = 0;
    int kmsg = kmsg_open();
    UsbFault uf;
    usbf_init(&uf);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (running) {
        next.tv_sec += 1;
        wait_tick(&next, flashing);
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
        {
            uint32_t now_ms = mono_ms();
            int err = kmsg_vbus_error(kmsg);
            int was_f = usbf_flag(&uf);
            int present = usbf_wants_scan(&uf, now_ms) ? usb_peripheral_present() : 0;
            if (err) puts("arcamond: VBUS_ERROR in the kernel log");
            int act = usbf_step(&uf, now_ms, err, present, recover_running());
            if (act == USBF_ACT_OFF) {
                puts("arcamond: VBUS_ERROR loop, running usb-host off");
                recover_start("off");
            } else if (act == USBF_ACT_RECOVER) {
                puts("arcamond: USB fault, running usb-host recover");
                recover_start("recover");
            }
            if (usbf_flag(&uf) != was_f)
                puts(usbf_flag(&uf) ? "arcamond: USB fault indicator on"
                                    : "arcamond: USB fault indicator off");
        }
        warn_led(usbf_led(warn, usbf_flag(&uf), mono_ms()));
        flashing = !warn && usbf_flag(&uf);
        unsigned flags = (warn ? MON_FLAG_OVERTEMP : 0u) | (usbf_flag(&uf) ? MON_FLAG_USBFAULT : 0u);
        int len = cur.valid ? mon_ring_format(&ring, cur.wall_cs, flags, text, sizeof(text)) : -1;
        if (len > 0) publish(text, len);

        /* A suspend or a stopped process must not make us replay missed
         * seconds back to back: re-anchor if we fell more than a tick behind. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now;
    }
    unlink(MON_RING_PATH);
    warn_led(USBF_LED_RELEASE);
    printf("arcamond: stopped\n");
    return 0;
}
