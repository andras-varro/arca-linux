#ifndef VNC_CONFIG_H
#define VNC_CONFIG_H

/* ── VNCConfig struct (shared between vnc_client.c and vnc_settings.c) ─ */
typedef struct VNCConfig {
    char host[256];
    int  port;
    char password[256];
    char encodings[256];
    int  compress_level;
    int  quality_level;
    /* content_area: 0 = 'safe' (letterbox the remote desktop into the
     * touch-safe rectangle so every pixel of it is reachable — the default),
     * 1 = 'visible' (use the whole visible surface and accept that a band at
     * each end cannot be tapped).  Affects the PICTURE only; this component's
     * own touch UI is always inside the safe rect.  See vnc_renderer.h. */
    int  content_full;
} VNCConfig;

// VNC Server Configuration (compile-time defaults, overridden by config file)
#define VNC_DEFAULT_HOST "192.168.50.56"
#define VNC_DEFAULT_PORT 5900
#define VNC_DEFAULT_PASSWORD ""
#define VNC_DEFAULT_CONTENT_FULL 0

// Runtime config file (key=value format, lines starting with # are comments)
#define VNC_CONFIG_FILE "/opt/vnc_client/vnc_client.conf"

// Device Paths
#define FB_DEVICE "/dev/fb0"
/* The udev link, as every other touch user opens: on our kernel the touch module
 * loads after USB HID, so the touchscreen is not event0 (measured event6 on .188). */
#define TOUCH_DEVICE "/dev/input/touchscreen0"
#define WATCHDOG_DEVICE "/dev/watchdog"

// Watchdog Configuration
#define WATCHDOG_FEED_INTERVAL_SEC 30

// Display Configuration
//
// These bound the PHYSICAL panel and exist only to size fixed arrays.  They are
// NOT the screen size: fb_init() shrinks the drawing surface to the rectangle
// the bezel leaves visible (800x450 at the shipped 15/15/0/0 margins), so the
// logical screen is fb->width x fb->height and changes with the bezel config.
// Always draw and hit-test against those; any logical surface is <= these.
#define PANEL_MAX_WIDTH  800
#define PANEL_MAX_HEIGHT 480

// Scaling Modes
typedef enum {
    SCALING_LETTERBOX,  // Maintain aspect ratio with black bars
    SCALING_STRETCH,    // Stretch to fill screen
    SCALING_CROP        // Crop to fit
} ScalingMode;

#define DEFAULT_SCALING_MODE SCALING_LETTERBOX

// Visual Feedback
#define SHOW_REMOTE_CURSOR 1
#define TOUCH_FEEDBACK_RADIUS 10
#define TOUCH_FEEDBACK_COLOR 0xFF0000  // Red
#define TOUCH_FEEDBACK_DURATION_MS 200  // 200ms

// Performance Settings
#define TARGET_FPS 30
#define ENABLE_DOUBLE_BUFFER 1

// Debug Settings
#define DEBUG_ENABLED 1

#if DEBUG_ENABLED
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, "[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) do {} while(0)
#endif

// Display Settings
#define SHOW_FPS_COUNTER 1
#define SHOW_CONNECTION_STATUS 1

// VNC Encoding - request compressed encodings (most preferred first)
// This is the single biggest performance win: 10-100x bandwidth reduction
#define VNC_ENCODINGS "tight zrle copyrect hextile zlib raw"
#define VNC_COMPRESS_LEVEL 6    // 1-9 (higher = more compression, more server CPU)
#define VNC_QUALITY_LEVEL 5     // 1-9 (JPEG quality for Tight encoding)

// Framebuffer format - 16bpp RGB565 halves memory bandwidth
// (Same approach as ScummVM Arca backend)
#define USE_16BPP 1

// Frame interval derived from TARGET_FPS
#define FRAME_INTERVAL_US (1000000 / TARGET_FPS)

// Exit gesture: long-press in top-left corner
// Zone size in screen pixels, hold duration in milliseconds
#define EXIT_ZONE_SIZE 60
#define EXIT_HOLD_MS 3000

// Auto-reconnect on connection drop
// Backoff: 2s, 5s, 10s, 30s, 60s (then stay at 60s)
#define RECONNECT_ENABLED       1
#define RECONNECT_MAX_ATTEMPTS  0       // 0 = unlimited
#define RECONNECT_DELAYS        {2, 5, 10, 30, 60}
#define RECONNECT_DELAYS_COUNT  5
#define RECONNECT_INITIAL_CONNECT_TIMEOUT 10  // seconds before first-connect gives up
#define RECONNECT_CONNECT_TIMEOUT    5   // seconds TCP connect timeout during reconnect

// Respawn integration: on exit gesture, switch default-app back to launcher
#define RESPAWN_CONFIG_FILE    "/opt/arca/default-app"
#define APP_LAUNCHER_PATH     "/opt/arca/app_launcher"

// Reconnect UI button geometry.  The row is anchored to the bottom of the
// TOUCH-SAFE rectangle at runtime (SCREEN_SAFE_BOTTOM - RECONNECT_BTN_H -
// RECONNECT_BTN_BOTTOM_MARGIN), which keeps it pressable at any bezel setting
// and on any panel: the digitizer saturates before the panel edge, and
// SCREEN_SAFE_BOTTOM is where that band is measured to start.
#define RECONNECT_BTN_W         160
#define RECONNECT_BTN_H         44
#define RECONNECT_BTN_BOTTOM_MARGIN 10
#define RECONNECT_BTN_CANCEL_X   60
#define RECONNECT_BTN_SETTINGS_X 320
#define RECONNECT_BTN_CONNECT_X  580

/* Mouse defaults, the /etc/input_config.conf path and the hot-plug check
 * interval are common/input_scan.h's: input_config_defaults(),
 * INPUT_CONFIG_PATH and INPUT_SIG_CHECK_MS. */

#endif // VNC_CONFIG_H
