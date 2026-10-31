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
 * non-hub device is present.  Only one recover runs at a time.
 */
#ifndef USBFAULT_H
#define USBFAULT_H

#include <stdint.h>

#define USBF_FIRST_MS   5000u
#define USBF_SECOND_MS  15000u
#define USBF_THIRD_MS   45000u
#define USBF_REPEAT_MS  120000u

typedef struct {
    int armed;        /* error seen, first check not yet made */
    int latched;      /* fault confirmed: indicator on, retries running */
    int tries;        /* recovers started since the error */
    uint32_t err_ms;  /* when the error was seen */
    uint32_t next_ms; /* when the next check or recover is due */
} UsbFault;

static inline void usbf_init(UsbFault *f) {
    f->armed = f->latched = f->tries = 0;
    f->err_ms = f->next_ms = 0;
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

/* One step.  error_seen: a VBUS_ERROR record arrived since the last step.
 * dev_present: a non-hub device is enumerated (only read when
 * usbf_wants_scan() said so).  recover_running: a recover child is alive.
 * Returns 1 when the caller must start a recover now. */
static inline int usbf_step(UsbFault *f, uint32_t now_ms, int error_seen,
                            int dev_present, int recover_running) {
    if (error_seen && !f->armed && !f->latched) {
        f->armed = 1;
        f->tries = 0;
        f->err_ms = now_ms;
        f->next_ms = now_ms + USBF_FIRST_MS;
        return 0;
    }
    if (!f->armed && !f->latched) return 0;
    if (f->latched && dev_present) { usbf_init(f); return 0; }
    if ((int32_t)(now_ms - f->next_ms) < 0) return 0;
    if (f->armed && dev_present) { usbf_init(f); return 0; }
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

#endif
