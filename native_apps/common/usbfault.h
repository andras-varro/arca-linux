/* usbfault.h — the USB-overload recovery's pure decisions.
 *
 * The kernel logs "VBUS_ERROR" when an overcurrent drops the musb session and
 * then never raises VBUS again.  arcamond (sysmon/arcamond.c) feeds this
 * state machine once a second and acts on its answer; it publishes the latch
 * as MON_FLAG_USBFAULT in the ring header, which framebuffer.c paints and
 * which arcamond also flashes on red_led (usbf_led below).
 * Header-only and pure: no file, clock or process access; the caller passes
 * a uint32_t millisecond clock (wraps are handled by signed differences).
 * tests/usbfault_test.c drives it.
 *
 * Sequence: an error arms the machine; USBF_FIRST_MS later the caller's
 * "non-hub device present" answer decides — present means the drop did not
 * happen (clear), absent latches the fault and asks for a recover.  While
 * latched, retries come at 5 s, 15 s and 45 s after the error and then every
 * USBF_REPEAT_MS, never faster, with no end; the latch clears the moment a
 * non-hub device is present.  Only one recover runs at a time.  Three errors
 * inside 30 s (the overload loop, where the devices are back before every
 * check) cut the port instead: see usbf_step.
 */
#ifndef USBFAULT_H
#define USBFAULT_H

#include <stdint.h>

#define USBF_FIRST_MS   5000u
#define USBF_SECOND_MS  15000u
#define USBF_THIRD_MS   45000u
#define USBF_REPEAT_MS  120000u
#define USBF_CUT_COUNT  3
#define USBF_CUT_WIN_MS 30000u
#define USBF_ACT_RECOVER 1   /* usbf_step: run usb-host recover */
#define USBF_ACT_OFF     2   /* usbf_step: run usb-host off (the port loop) */

typedef struct {
    int armed;        /* error seen, first check not yet made */
    int latched;      /* fault confirmed: indicator on, retries running */
    int tries;        /* recovers started since the error */
    uint32_t err_ms;  /* when the error was seen */
    uint32_t next_ms; /* when the next check or recover is due */
    int nerr;         /* recent error stamps held (0..3) */
    uint32_t errs[3]; /* the newest error times, oldest first */
} UsbFault;

static inline void usbf_init(UsbFault *f) {
    f->armed = f->latched = f->tries = 0;
    f->err_ms = f->next_ms = 0;
    f->nerr = 0;
}

/* Back to idle, keeping the error history the loop rule counts. */
static inline void usbf_clear(UsbFault *f) {
    int n = f->nerr; uint32_t e[3] = { f->errs[0], f->errs[1], f->errs[2] };
    usbf_init(f);
    f->nerr = n; f->errs[0] = e[0]; f->errs[1] = e[1]; f->errs[2] = e[2];
}

/* The ring flag: the confirmed fault only, not the 5 s waiting window. */
static inline int usbf_flag(const UsbFault *f) { return f->latched; }

/* The fault's flash cadence, shared by the screen's sidebars (framebuffer.c)
 * and the red LED (arcamond.c): lit in the even 200 ms slots of the first
 * 1200 ms of every 10 s of the monotonic millisecond clock, so both flash
 * together. */
#define USBF_FLASH_PERIOD_MS 10000u
#define USBF_FLASH_PHASE_MS  200u

static inline int usbf_flash_on(uint32_t now_ms) {
    const uint32_t t = now_ms % USBF_FLASH_PERIOD_MS;
    return t < 6u * USBF_FLASH_PHASE_MS && ((t / USBF_FLASH_PHASE_MS) & 1u) == 0;
}

/* What arcamond does with red_led: give it back, light it, or hold it dark
 * (a flash's off phase; the saved value stays saved).  Over-temperature is
 * solid and wins over a latched fault. */
enum { USBF_LED_RELEASE = 0, USBF_LED_LIT = 1, USBF_LED_DARK = 2 };

static inline int usbf_led(int overtemp, int latched, uint32_t now_ms) {
    if (overtemp) return USBF_LED_LIT;
    if (!latched) return USBF_LED_RELEASE;
    return usbf_flash_on(now_ms) ? USBF_LED_LIT : USBF_LED_DARK;
}

/* Whether the caller must look at /sys/bus/usb/devices this second. */
static inline int usbf_wants_scan(const UsbFault *f, uint32_t now_ms) {
    if (f->latched) return 1;
    return f->armed && (int32_t)(now_ms - f->next_ms) >= 0;
}

/* The single-drop machine (header comment).  Returns 1 for a recover. */
static inline int usbf_step_drop(UsbFault *f, uint32_t now_ms, int error_seen,
                            int dev_present, int recover_running) {
    if (error_seen && !f->armed && !f->latched) {
        f->armed = 1;
        f->tries = 0;
        f->err_ms = now_ms;
        f->next_ms = now_ms + USBF_FIRST_MS;
        return 0;
    }
    if (!f->armed && !f->latched) return 0;
    if (f->latched && dev_present) { usbf_clear(f); return 0; }
    if ((int32_t)(now_ms - f->next_ms) < 0) return 0;
    if (f->armed && dev_present) { usbf_clear(f); return 0; }
    if (recover_running) return 0;   /* due, but wait for the child */
    f->armed = 0;
    f->latched = 1;
    f->tries++;
    uint32_t nxt;
    if (f->tries == 1)      nxt = f->err_ms + USBF_SECOND_MS;
    else if (f->tries == 2) nxt = f->err_ms + USBF_THIRD_MS;
    else                    nxt = now_ms + USBF_REPEAT_MS;
    if ((int32_t)(nxt - (now_ms + USBF_FIRST_MS)) < 0) nxt = now_ms + USBF_FIRST_MS;
    f->next_ms = nxt;
    return 1;
}


/* One step.  error_seen: how many VBUS_ERROR records arrived since the last
 * step.  dev_present: a non-hub device is enumerated (only read when
 * usbf_wants_scan() said so).  recover_running: a recover child is alive.
 *
 * The loop rule: USBF_CUT_COUNT errors within USBF_CUT_WIN_MS mean the bus is
 * overloaded and the kernel is re-asserting the session forever, with the
 * devices back before every T+5 s check.  The port is turned off (return
 * USBF_ACT_OFF), the fault latches, and the next recover follows USBF_REPEAT_MS
 * later; if that one overloads again the same rule cuts again.  A cut that is
 * due while a recover child runs waits for it.
 * Returns USBF_ACT_RECOVER / USBF_ACT_OFF / 0. */
static inline int usbf_step(UsbFault *f, uint32_t now_ms, int error_seen,
                            int dev_present, int recover_running) {
    for (int i = 0; i < error_seen && i < USBF_CUT_COUNT; i++) {
        if (f->nerr == USBF_CUT_COUNT) {
            f->errs[0] = f->errs[1]; f->errs[1] = f->errs[2]; f->nerr--;
        }
        f->errs[f->nerr++] = now_ms;
    }
    if (f->nerr == USBF_CUT_COUNT && !recover_running &&
        (uint32_t)(now_ms - f->errs[0]) <= USBF_CUT_WIN_MS) {
        f->nerr = 0;
        f->armed = 0;
        f->latched = 1;
        f->tries = 3;
        f->err_ms = now_ms;
        f->next_ms = now_ms + USBF_REPEAT_MS;
        return USBF_ACT_OFF;
    }
    return usbf_step_drop(f, now_ms, error_seen, dev_present, recover_running);
}
#endif
