#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Hardware Control Library for Arca
 * 
 * Provides control over:
 * - LED indicators (red and green)
 * - Backlight brightness
 * 
 * All brightness values are 0-100, expressed as a percentage OF THE USER'S
 * CONFIGURED MAXIMUM (led_brightness / backlight_brightness in
 * /opt/games/arca_config.conf) — not of the raw sysfs duty cycle. Setters scale
 * down before writing and getters scale back up, so a value round-trips:
 * hw_set_backlight(hw_get_backlight()) leaves the panel where it was.
 */

// LED color enumeration
typedef enum {
    LED_RED = 0,
    LED_GREEN = 1
} LEDColor;

// LED state structure (for combined control)
typedef struct {
    uint8_t red_brightness;    // 0-100
    uint8_t green_brightness;  // 0-100
} LEDState;

/**
 * Initialize hardware control subsystem
 * Returns: 0 on success, -1 on error
 */
int hw_init(void);

/**
 * Reload hardware configuration from disk.
 * Call this after config may have changed (e.g., after a child process exits
 * that may have modified the config file).
 */
void hw_reload_config(void);

/* Screen blanking after an idle time (config key blank_minutes).  The input
 * paths call note_activity (true = this input woke a dark panel and MUST be
 * dropped, with its matching release) or keepalive (refreshes the timer, never
 * wakes); the present and poll paths call poll.  set_enabled(false) is the
 * opt-out for video-like processes: never blank, and restore a dark panel. */
bool hw_blank_note_activity(void);
void hw_blank_keepalive(void);
void hw_blank_poll(void);
void hw_blank_set_enabled(bool enabled);

/**
 * Set individual LED brightness
 * @param led: LED_RED or LED_GREEN
 * @param brightness: 0-100 (0=off, 100=full brightness)
 * Returns: 0 on success, -1 on error
 */
int hw_set_led(LEDColor led, uint8_t brightness);

/**
 * Get individual LED brightness
 * @param led: LED_RED or LED_GREEN
 * Returns: brightness value 0-100 in the same space hw_set_led() takes
 *          (percent of the configured maximum), or -1 on error
 */
int hw_get_led(LEDColor led);

/**
 * Set red LED brightness
 * @param brightness: 0-100
 * Returns: 0 on success, -1 on error
 */
int hw_set_red_led(uint8_t brightness);

/**
 * Set green LED brightness
 * @param brightness: 0-100
 * Returns: 0 on success, -1 on error
 */
int hw_set_green_led(uint8_t brightness);

/**
 * Set both LEDs at once
 * @param red: Red LED brightness 0-100
 * @param green: Green LED brightness 0-100
 * Returns: 0 on success, -1 on error
 */
int hw_set_leds(uint8_t red, uint8_t green);

/**
 * Get current LED state
 * @param state: Pointer to LEDState structure to fill
 * Returns: 0 on success, -1 on error
 */
int hw_get_led_state(LEDState *state);

/**
 * Turn off all LEDs
 * Returns: 0 on success, -1 on error
 */
int hw_leds_off(void);

/**
 * Set backlight brightness
 * @param brightness: 0-100 (0=off, 100=full brightness)
 * Returns: 0 on success, -1 on error
 */
int hw_set_backlight(uint8_t brightness);

/**
 * Set backlight brightness WITHOUT applying the configured maximum.
 * @param brightness: 0-100, written straight to the panel
 * Returns: 0 on success, -1 on error
 *
 * For the settings sliders only.  Those are choosing the scale factor that
 * hw_set_backlight() applies, so previewing a value through the scaling setter
 * would multiply it by the factor being replaced.  Everything else wants
 * hw_set_backlight().
 *
 * This exists so a preview does not need its own copy of the sysfs path:
 * control_panel and a since-deleted settings tool each had one, both naming
 * /sys/class/backlight/pwm-backlight/brightness, which does not exist on this
 * device — /sys/class/backlight is empty and the panel is a LED class device.
 * So both previews silently did nothing.
 */
int hw_set_backlight_raw(uint8_t brightness);

/**
 * Get backlight brightness
 * Returns: brightness value 0-100 in the same space hw_set_backlight() takes
 *          (percent of the configured maximum), or -1 on error.
 *          Safe to feed straight back into hw_set_backlight().
 */
int hw_get_backlight(void);

/**
 * Pulse an LED (fade in/out effect)
 * @param led: LED_RED or LED_GREEN
 * @param duration_ms: Total duration of pulse in milliseconds
 * @param max_brightness: Peak brightness 0-100
 * Returns: 0 on success, -1 on error
 * 
 * Note: This is a blocking function
 */
int hw_pulse_led(LEDColor led, uint32_t duration_ms, uint8_t max_brightness);

/**
 * Blink an LED
 * @param led: LED_RED or LED_GREEN
 * @param count: Number of blinks
 * @param on_ms: Time LED is on (milliseconds)
 * @param off_ms: Time LED is off (milliseconds)
 * @param brightness: LED brightness when on (0-100)
 * Returns: 0 on success, -1 on error
 * 
 * Note: This is a blocking function
 */
int hw_blink_led(LEDColor led, int count, uint32_t on_ms, uint32_t off_ms, uint8_t brightness);

/**
 * Non-blocking LED blink — the one to use from a game.
 *
 * hw_blink_led() above cannot be called from an update or input path: it sleeps
 * for its whole duration, so the panel freezes with no touch poll, no redraw and
 * no gamepad poll. Tetris and Pong hand-rolled the same loop for their
 * game-over/win flourish and each froze for 1.2 s *before* the game-over screen
 * was drawn, which also swallowed every tap made during it.
 *
 * Start one, then call hw_led_pulse_update() once per frame until it reports
 * inactive; it writes the LED only on the frames the phase actually changes.
 *
 * The current time is a parameter rather than a get_time_ms() call because
 * hardware.c is linked by vnc_client, which does not link common.c — and
 * because a caller-supplied clock is testable on the host.
 */
typedef struct {
    bool     active;
    LEDColor led;
    int      flashes;          /* number of on-phases remaining to play out */
    uint32_t half_period_ms;   /* duration of one on- or off-phase */
    uint32_t start_ms;
    int      phase;            /* phase index last written, -1 = none yet */
} LedPulse;

/**
 * Begin a blink of `flashes` on/off pairs, each half lasting half_period_ms.
 * Lights the LED immediately, so a one-frame effect still shows.
 * @param now_ms: current millisecond clock (get_time_ms() in native_apps)
 */
void hw_led_pulse_start(LedPulse *p, LEDColor led, int flashes,
                        uint32_t half_period_ms, uint32_t now_ms);

/**
 * Advance the blink. Call once per frame. Turns the LED off and clears
 * `active` when the last phase has elapsed. Safe to call on an inactive pulse.
 */
void hw_led_pulse_update(LedPulse *p, uint32_t now_ms);

/** True while a pulse still owes the LED at least one phase change. */
bool hw_led_pulse_active(const LedPulse *p);

/**
 * Cancel a pulse and turn its LED off. Call this when the state that started
 * the pulse goes away — restarting a game mid-flourish would otherwise leave
 * the LED flashing into the new round, or stuck on. Safe on an inactive pulse.
 */
void hw_led_pulse_stop(LedPulse *p);

/**
 * Set LED color by mixing red and green
 * Predefined color combinations:
 * - Red only: (100, 0)
 * - Green only: (0, 100)
 * - Yellow/Orange: (100, 100) or (100, 50)
 * - Off: (0, 0)
 */
#define HW_LED_COLOR_OFF        0,   0
#define HW_LED_COLOR_RED        100, 0
#define HW_LED_COLOR_GREEN      0,   100
#define HW_LED_COLOR_YELLOW     100, 100
#define HW_LED_COLOR_ORANGE     100, 50

#endif // HARDWARE_H
