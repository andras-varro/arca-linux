/*
 * VNC Client for Arca - v2.2 (Auto-Reconnect)
 *
 * v2.2 changes:
 *   - Auto-reconnect with exponential backoff on connection drop
 *   - On-screen reconnect UI with countdown, attempt counter, buttons
 *   - Touch [Cancel] to return to launcher, [Connect Now] to skip wait
 *
 * v2.1 changes:
 *   - Encoding negotiation: requests Tight/ZRLE instead of Raw
 *   - 16bpp RGB565 framebuffer (halves swap bandwidth)
 *   - Partial region updates (only re-render dirty VNC rectangles)
 *   - Frame-rate-capped presentation (30 fps)
 *   - Runtime config file support (/opt/vnc_client/vnc_client.conf)
 *   - Bilinear interpolation for smooth downscaling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <rfb/rfbclient.h>

#include "config.h"
#include "vnc_renderer.h"
#include "vnc_input.h"
#include "vnc_settings.h"
#include "../native_apps/common/framebuffer.h"
#include "../native_apps/common/touch_input.h"
#include "../native_apps/common/hardware.h"
#include "../native_apps/common/logger.h"
#include "../native_apps/common/product.h"

static Logger g_logger;

/* ── Runtime configuration ─────────────────────────────────────────── */

static VNCConfig g_config;
static const char *g_config_path = NULL;

/* Trim leading/trailing whitespace in-place */
static char *str_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/*
 * Parse a config file with key=value lines.
 * Lines starting with # are comments; empty lines are ignored.
 * Supported keys: host, port, password, encodings, compress_level,
 *                 quality_level, content_area
 */
static void load_config_file(VNCConfig *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_INFO(&g_logger, "Config file not found: %s (using defaults)", path);
        return;
    }

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *trimmed = str_trim(line);
        if (*trimmed == '\0' || *trimmed == '#')
            continue;

        char *eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = str_trim(trimmed);
        char *val = str_trim(eq + 1);

        if (strcmp(key, "host") == 0) {
            strncpy(cfg->host, val, sizeof(cfg->host) - 1);
            count++;
        } else if (strcmp(key, "port") == 0) {
            cfg->port = atoi(val);
            count++;
        } else if (strcmp(key, "password") == 0) {
            strncpy(cfg->password, val, sizeof(cfg->password) - 1);
            count++;
        } else if (strcmp(key, "encodings") == 0) {
            strncpy(cfg->encodings, val, sizeof(cfg->encodings) - 1);
            count++;
        } else if (strcmp(key, "compress_level") == 0) {
            cfg->compress_level = atoi(val);
            count++;
        } else if (strcmp(key, "quality_level") == 0) {
            cfg->quality_level = atoi(val);
            count++;
        } else if (strcmp(key, "content_area") == 0) {
            /* 'safe' (default) letterboxes the remote desktop into the
             * touch-safe rectangle so all of it is reachable; 'visible' gives
             * it the whole surface and accepts an unreachable band. */
            if (strcmp(val, "visible") == 0) {
                cfg->content_full = 1;
                count++;
            } else if (strcmp(val, "safe") == 0) {
                cfg->content_full = 0;
                count++;
            } else {
                LOG_WARN(&g_logger, "content_area '%s' is not 'safe' or "
                         "'visible' — keeping 'safe'", val);
            }
        } else {
            LOG_WARN(&g_logger, "Unknown config key '%s'", key);
        }
    }
    fclose(f);
    LOG_INFO(&g_logger, "Loaded %d settings from %s", count, path);
}

/* ── Global state ──────────────────────────────────────────────────── */

static volatile bool g_running = true;
static Framebuffer   g_fb;
static TouchInput    g_touch;
static VNCRenderer   g_renderer;
static VNCInput      g_input;
static rfbClient    *g_vnc_client = NULL;
static pthread_t     g_watchdog_thread;
static bool          g_touch_ok = false;

/* ── Forward declarations ──────────────────────────────────────────── */
/* Both are defined next to the reconnect helpers they belong with, but
 * vnc_client_init() calls vnc_enable_keepalive() before that point. */
static void vnc_client_destroy(rfbClient **client);
static void vnc_enable_keepalive(rfbClient *client);

/* ── Signal handler ────────────────────────────────────────────────── */

static void signal_handler(int sig) {
    LOG_INFO(&g_logger, "Received signal %d, shutting down...", sig);
    g_running = false;
}

/* ── Watchdog feeder thread ────────────────────────────────────────── */

static void *watchdog_thread_func(void *arg) {
    (void)arg;
    int wd_fd = open(WATCHDOG_DEVICE, O_WRONLY);
    if (wd_fd < 0) {
        LOG_WARN(&g_logger, "Could not open watchdog device");
        return NULL;
    }
    LOG_DEBUG(&g_logger, "Watchdog thread started (interval %ds)", WATCHDOG_FEED_INTERVAL_SEC);
    while (g_running) {
        write(wd_fd, "1", 1);
        sleep(WATCHDOG_FEED_INTERVAL_SEC);
    }
    close(wd_fd);
    LOG_DEBUG(&g_logger, "Watchdog thread stopped");
    return NULL;
}

/* ── LibVNCClient callbacks ────────────────────────────────────────── */

/*
 * BUG-INPUT-005 FIX: Expected bits-per-pixel.  We always negotiate 32bpp
 * from the server so the renderer can rely on 4-byte pixels.
 */
static const int EXPECTED_BPP = 32;

/*
 * Password callback for VNC authentication (type 2, VncAuth).
 * Returns a malloc'd string that LibVNCClient will free.
 */
static char *vnc_get_password(rfbClient *client) {
    (void)client;
    LOG_DEBUG(&g_logger, "VNC password requested, providing configured password");
    return strdup(g_config.password);
}

/*
 * Called for each dirty rectangle the server sends.
 * With compressed encoding (Tight/ZRLE), these are typically small regions.
 * We convert + scale only the dirty pixels into the back buffer.
 */
static void vnc_fb_update(rfbClient *client, int x, int y, int w, int h) {
    int bpp = client->format.bitsPerPixel / 8;

    /*
     * BUG-INPUT-005 FIX: Guard against processing framebuffer data that
     * arrives in an unexpected pixel format.  After a server-initiated
     * format change (e.g. 32→16 bpp when a window closes with Alt+F4),
     * there may be a brief window where buffered updates still carry the
     * old (wrong) format before our re-asserted SetPixelFormat takes
     * effect.  Rendering such data would corrupt the display (quadrupled
     * image, wrong colours).  Skip these stale updates — a full
     * FramebufferUpdateRequest has already been sent by vnc_malloc_fb().
     */
    if (bpp != EXPECTED_BPP / 8) {
        LOG_WARN(&g_logger, "BUG-INPUT-005 FIX: Skipping framebuffer update in "
                 "%dbpp (expected %dbpp) — re-assertion pending",
                 bpp * 8, EXPECTED_BPP);
        return;
    }

    vnc_renderer_update_region(&g_renderer,
                               (const uint8_t *)client->frameBuffer,
                               x, y, w, h, bpp);
}

/*
 * BUG-INPUT-005 FIX: MallocFrameBuffer callback.
 *
 * LibVNCClient invokes this whenever:
 *   1. The initial connection is established (normal).
 *   2. The server sends a DesktopSize pseudo-encoding (desktop resize).
 *   3. The server switches pixel format mid-session.
 *
 * Problem scenario:
 *   When a remote application is closed with Alt+F4, some VNC servers
 *   (x11vnc, TightVNC, RealVNC) redraw the root window and may drop
 *   the colour depth to 16-bit.  Without this callback the default
 *   LibVNCClient handler silently accepts the new format, but the
 *   renderer still interprets every pixel as 32-bit — causing:
 *     • Wrong colours (different R/G/B bit layout)
 *     • Quadrupled image (half the bytes-per-pixel ⇒ stride mismatch)
 *
 * Fix:
 *   1. Always allocate the framebuffer for our expected 32bpp format.
 *   2. Detect when the server has changed the pixel format and re-send
 *      SetPixelFormat + SetEncodings to force 32bpp.
 *   3. If the desktop was resized, update the renderer's scaling LUT.
 *   4. Request a full non-incremental framebuffer update so the server
 *      redraws everything in the correct format.
 */
static rfbBool vnc_malloc_fb(rfbClient *client) {
    int width  = client->width;
    int height = client->height;
    int bpp    = client->format.bitsPerPixel;

    LOG_INFO(&g_logger, "BUG-INPUT-005 FIX: MallocFrameBuffer called — "
             "%dx%d %dbpp (expected %dbpp)", width, height, bpp, EXPECTED_BPP);

    /* ── Allocate framebuffer for 32bpp (4 bytes/pixel) regardless of
     *    what the server announced.  This ensures the renderer always
     *    has enough space for 32-bit pixel data. ─────────────────────── */
    size_t fb_size = (size_t)width * height * (EXPECTED_BPP / 8);

    if (client->frameBuffer)
        free(client->frameBuffer);
    client->frameBuffer = (uint8_t *)malloc(fb_size);
    if (!client->frameBuffer) {
        LOG_ERROR(&g_logger, "Failed to allocate VNC framebuffer (%zu bytes)", fb_size);
        return FALSE;
    }
    memset(client->frameBuffer, 0, fb_size);

    /* ── Detect pixel-format change and re-assert 32bpp ────────────── */
    bool need_full_update = false;

    if (bpp != EXPECTED_BPP) {
        LOG_WARN(&g_logger, "BUG-INPUT-005 FIX: Server changed pixel format to "
                 "%dbpp — forcing back to %dbpp", bpp, EXPECTED_BPP);

        /* Restore our preferred 32bpp format:
         *   Little-endian, true-colour, 8 bits per channel,
         *   byte layout R-G-B-X (shifts 0/8/16). */
        client->format.bitsPerPixel = 32;
        client->format.depth        = 24;
        client->format.bigEndian    = FALSE;
        client->format.trueColour   = TRUE;
        client->format.redMax       = 255;
        client->format.greenMax     = 255;
        client->format.blueMax      = 255;
        client->format.redShift     = 0;
        client->format.greenShift   = 8;
        client->format.blueShift    = 16;

        /* Re-send SetPixelFormat + SetEncodings to the server */
        SetFormatAndEncodings(client);
        need_full_update = true;
    }

    /* ── Handle desktop resize ─────────────────────────────────────── *
     * BUG-INPUT-005 FIX: Only update renderer/input if they have been
     * initialized.  The first MallocFrameBuffer call happens inside
     * rfbInitClient(), before vnc_renderer_init() runs — so g_renderer.fb
     * is still NULL.  The initial set_remote_size is done by vnc_session()
     * after rfbInitClient returns.  Subsequent calls (mid-session format
     * changes or resizes) arrive after init, so g_renderer.fb is valid.  */
    if (g_renderer.fb != NULL &&
        (g_renderer.remote_width  != width ||
         g_renderer.remote_height != height)) {
        LOG_INFO(&g_logger, "BUG-INPUT-005 FIX: Remote desktop resized from "
                 "%dx%d to %dx%d — updating renderer",
                 g_renderer.remote_width, g_renderer.remote_height,
                 width, height);
        vnc_renderer_set_remote_size(&g_renderer, width, height);

        /* Update input handler's remote size for mouse clamping */
        if (g_touch_ok)
            vnc_input_set_remote_size(&g_input, width, height);

        need_full_update = true;
    }

    /* ── Request a full (non-incremental) framebuffer update so the
     *    server redraws everything in our pixel format. ────────────── */
    if (need_full_update) {
        LOG_INFO(&g_logger, "BUG-INPUT-005 FIX: Requesting full framebuffer "
                 "update (%dx%d)", width, height);
        SendFramebufferUpdateRequest(client, 0, 0, width, height, FALSE);
    }

    return TRUE;
}

static rfbBool vnc_cursor_pos(rfbClient *client, int x, int y) {
    (void)client; (void)x; (void)y;
    return TRUE;
}

/* ── VNC client initialization with encoding negotiation ───────────── */

static rfbClient *vnc_client_init(const char *host, int port, unsigned int connect_timeout) {
    /* 8 bits/sample, 3 samples (RGB), 4 bytes/pixel → 32bpp from server */
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (!client) {
        LOG_ERROR(&g_logger, "Failed to create VNC client");
        return NULL;
    }

    /* Callbacks */
    client->GotFrameBufferUpdate = vnc_fb_update;
    client->HandleCursorPos      = vnc_cursor_pos;

    /*
     * BUG-INPUT-005 FIX: Register MallocFrameBuffer callback to intercept
     * server-initiated pixel format changes and desktop resizes.  Without
     * this, the default handler silently accepts format changes (e.g.
     * 32→16 bpp) which corrupts the display.
     */
    client->MallocFrameBuffer = vnc_malloc_fb;

    /* Password callback - always set, LibVNCClient calls it if server requires auth */
    client->GetPassword = vnc_get_password;

    /* Connection parameters */
    client->serverHost = strdup(host);
    client->serverPort = port;

    /*
     * ★ Phase 2.1: Encoding negotiation (biggest single optimization)
     *
     * Request compressed encodings in preference order.
     * Tight encoding with JPEG typically reduces bandwidth 10-100×
     * compared to Raw (1.5 MB/frame → 10-150 KB/frame).
     */
    client->appData.encodingsString = g_config.encodings;
    client->appData.compressLevel   = g_config.compress_level;
    client->appData.qualityLevel    = g_config.quality_level;

    /* Set connect timeout (default is 60s which freezes the UI) */
    client->connectTimeout = connect_timeout;

    /* Connect and negotiate */
    if (!rfbInitClient(client, NULL, NULL)) {
        LOG_ERROR(&g_logger, "Failed to connect to VNC server at %s:%d", host, port);
        return NULL;
    }

    LOG_INFO(&g_logger, "Remote desktop: %dx%d, %d bpp depth %d",
             client->width, client->height,
             client->format.bitsPerPixel, client->format.depth);
    LOG_DEBUG(&g_logger, "Encodings requested: %s", g_config.encodings);
    LOG_DEBUG(&g_logger, "Compress=%d Quality=%d", g_config.compress_level, g_config.quality_level);

    /* Socket exists only now that rfbInitClient() has connected. */
    vnc_enable_keepalive(client);

    return client;
}

/*
 * Tear down a session's rfbClient.
 *
 * ⚠️ rfbClientCleanup() frees raw_buffer, ultra_buffer, desktopName and
 * serverHost, but NOT client->frameBuffer — vnc_malloc_fb() allocated that, so
 * this side owns it and nothing else was freeing it.  With
 * RECONNECT_MAX_ATTEMPTS 0 (unlimited) against a 1080p host that is
 * 1920*1080*4 = ~8.3 MB leaked per drop on a 234 MB device: OOM after ~25
 * reconnects.  Both cleanup sites go through here so a third one cannot
 * reintroduce the leak.
 */
static void vnc_client_destroy(rfbClient **client) {
    if (!client || !*client)
        return;
    free((*client)->frameBuffer);
    (*client)->frameBuffer = NULL;
    rfbClientCleanup(*client);
    *client = NULL;
}

/*
 * B12: ask the kernel to detect a dead peer for us.
 *
 * libvncclient 0.9.14 sets no SO_KEEPALIVE and never pings, so a silent TCP
 * death (AP drop, NAT idle timeout, VM suspend) left a stale frame on screen
 * forever — WaitForMessage() just kept returning 0 and the loop spun.  With
 * keepalive armed the socket eventually errors, WaitForMessage() returns < 0,
 * and the existing "VNC connection lost" branch runs the reconnect UI.
 *
 * Deliberately NOT the other half of the change originally proposed — see the
 * B11/B12 entry.  "Break after N seconds with no server message" would
 * disconnect *healthy* sessions: steady-state requests are incremental, so a
 * static remote desktop correctly sends nothing for minutes, which is exactly
 * what a wall-mounted dashboard looks like.  Silence is not evidence of death
 * here; an unacknowledged TCP segment is.
 */
static void vnc_enable_keepalive(rfbClient *client) {
    if (!client || client->sock < 0)
        return;

    int on = 1;
    if (setsockopt(client->sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0) {
        LOG_WARN(&g_logger, "SO_KEEPALIVE failed — a dead peer will not be detected");
        return;
    }

    /* Defaults are 2 h idle + 9 probes 75 s apart (~11 min to notice). Tighten
     * to idle 20 s, 3 probes 10 s apart: a dead peer is detected in ~50 s,
     * which is inside the 60 s hardware watchdog window. Non-fatal if the
     * kernel refuses any of them — plain SO_KEEPALIVE is still better than none. */
    int idle = 20, intvl = 10, cnt = 3;
    if (setsockopt(client->sock, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle))  < 0 ||
        setsockopt(client->sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) < 0 ||
        setsockopt(client->sock, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt))   < 0) {
        LOG_WARN(&g_logger, "TCP keepalive tuning failed — falling back to kernel defaults (~11 min)");
        return;
    }
    LOG_DEBUG(&g_logger, "TCP keepalive: idle %ds, %d probes %ds apart", idle, cnt, intvl);
}

/* ── Reconnect UI helpers ──────────────────────────────────────────── */

/* Draw a centered text string at given Y position */
static void draw_centered_text(Framebuffer *fb, int y, const char *text,
                                uint16_t color, int scale) {
    int tw = vnc_renderer_text_width(text, scale);
    int x = ((int)fb->width - tw) / 2;
    if (x < 0) x = 0;
    vnc_renderer_draw_text(fb, x, y, text, color, scale);
}

/* Draw a button rectangle with centered label. */
static void draw_button(Framebuffer *fb, int bx, int by, int bw, int bh,
                         const char *label, uint16_t bg, uint16_t fg) {
    vnc_renderer_fill_rect(fb, bx, by, bw, bh, bg);
    /* 1-pixel border */
    vnc_renderer_fill_rect(fb, bx, by, bw, 1, fg);
    vnc_renderer_fill_rect(fb, bx, by + bh - 1, bw, 1, fg);
    vnc_renderer_fill_rect(fb, bx, by, 1, bh, fg);
    vnc_renderer_fill_rect(fb, bx + bw - 1, by, 1, bh, fg);
    /* Centered label (scale 2 → 12px char height) */
    int tw = vnc_renderer_text_width(label, 2);
    int lx = bx + (bw - tw) / 2;
    int ly = by + (bh - 14) / 2;
    vnc_renderer_draw_text(fb, lx, ly, label, fg, 2);
}

/* Hit test, deliberately separate from draw_button: the reconnect screen only
 * repaints when the countdown changes, and a hit test performed as a side effect
 * of drawing would then only run once a second. */
static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/*
 * Show the reconnect UI screen and wait for countdown expiry, user
 * pressing [CONNECT NOW], or user pressing [CANCEL].  Touch, a game pad
 * (d-pad / stick move the focus ring, A activates, B cancels), a USB keyboard
 * (arrows, Enter / Space, Esc) and a mouse (arrow, click acts on release) all
 * work.
 *
 * Returns:
 *   1 = user pressed Connect Now (or countdown expired) → try reconnect
 *   0 = user pressed Cancel → exit to launcher
 *   2 = user pressed Settings → open settings screen
 *  -1 = g_running became false (signal)
 */
static int reconnect_ui(int attempt, int wait_seconds) {
    LOG_INFO(&g_logger, "Reconnect UI: attempt %d, waiting %ds", attempt, wait_seconds);
    hw_set_led(LED_RED, 50);
    hw_set_led(LED_GREEN, 0);

    struct timeval start;
    gettimeofday(&start, NULL);

    int remaining = wait_seconds;
    int drawn_remaining = -1;       /* != remaining, so the first pass draws */

    /* Pad, keyboard and mouse, through the same VncNavInput the Settings screen
     * uses.  No session holds any USB node here, so this opens its own and
     * seeds whatever is already held (A or B from the session or Settings that
     * led here, a mouse button): none of it acts until released and pressed
     * again. */
    VncNavInput nav;
    vnc_nav_open(&nav);
    nav.kp_mode = -1;
    bool mouse_was_down = nav.ptr_down;   /* held on entry: no press edge */
    bool mouse_armed = false;             /* a press was seen; its release acts */
    bool focus_on = false;                /* ring hidden until the first nav input */
    bool ptr_on = false;
    int  ptr_x = nav.ptr_x, ptr_y = nav.ptr_y;
    int  focus = 0;
    int  decided = -2;                    /* -2: still waiting, else the return value */
    /* Snapshot of what the screen last showed beyond the countdown */
    bool d_focus_on = false, d_ptr_on = false;
    int  d_focus = -1, d_ptr_x = -1, d_ptr_y = -1;

    /* Left to right; the focus order and the return code of each button. */
    static const int btn_code[3] = { 0, 2, 1 };   /* CANCEL, SETTINGS, CONNECT */
    const int btn_x[3] = { RECONNECT_BTN_CANCEL_X, RECONNECT_BTN_SETTINGS_X,
                           RECONNECT_BTN_CONNECT_X };

    while (g_running && remaining >= 0 && decided == -2) {
        /* Poll touch — trigger buttons on finger-up (release),
         * not finger-down, to avoid accidental activations.        */
        int tx = -1, ty = -1;
        if (g_touch_ok) {
            touch_poll(&g_touch);
            TouchState ts = touch_get_state(&g_touch);
            if (ts.released) {
                tx = ts.x;
                ty = ts.y;
                LOG_DEBUG(&g_logger, "Touch released at (%d,%d)", tx, ty);
            }
        }

        /* The button row is anchored to the bottom of the TOUCH-SAFE rectangle,
         * so it stays visible — and inside the digitizer's reach — whatever the
         * bezel margins are and whatever the panel's dead band turns out to be. */
        int btn_y = SCREEN_SAFE_BOTTOM - RECONNECT_BTN_H - RECONNECT_BTN_BOTTOM_MARGIN;

        UiRect rects[3];
        for (int i = 0; i < 3; i++)
            rects[i] = (UiRect){ btn_x[i], btn_y, RECONNECT_BTN_W, RECONNECT_BTN_H };

        /* Pad and keyboard: the first move or activate only shows the ring; a
         * finger hides it.  B / Esc is CANCEL.  A press edge is required
         * (vnc_nav_poll), so a button held at entry never acts. */
        VncNav acts[16];
        char   chs[16];
        int nact = vnc_nav_poll(&nav, acts, chs, 16);
        bool finger = g_touch_ok && (tx >= 0 || touch_get_state(&g_touch).held);
        if (finger) {
            focus_on = false;
            ptr_on = false;
        }

        /* The mouse is a finger at the pointer: a click acts on release over
         * the button under the arrow, as the Settings screen does. */
        if (nav.ptr_moved) {
            ptr_on = true;
            ptr_x = nav.ptr_x;
            ptr_y = nav.ptr_y;
        }
        if (finger) {
            mouse_armed = false;
        } else if (nav.ptr_down && !mouse_was_down) {
            mouse_armed = true;
            focus_on = false;
        } else if (!nav.ptr_down && mouse_was_down && mouse_armed) {
            mouse_armed = false;
            tx = nav.ptr_x;
            ty = nav.ptr_y;
        }
        mouse_was_down = nav.ptr_down;

        for (int i = 0; i < nact && decided == -2 && !finger && !mouse_armed; i++) {
            UiDir d;
            if (vnc_nav_dir(acts[i], &d)) {
                if (focus_on)
                    focus = ui_focus_move(rects, 3, focus, d);
                focus_on = true;        /* the first press only shows the ring */
                ptr_on = false;
            } else if (acts[i] == VNC_NAV_ACTIVATE) {
                if (focus_on && focus >= 0 && focus < 3) {
                    LOG_INFO(&g_logger, "Reconnect button %d activated by pad/keyboard", focus);
                    decided = btn_code[focus];
                }
                focus_on = true;
                ptr_on = false;
            } else if (acts[i] == VNC_NAV_BACK) {
                LOG_INFO(&g_logger, "Reconnect CANCEL by pad/keyboard back");
                decided = 0;
            }
        }

        if (decided == -2 && tx >= 0) {
            for (int i = 0; i < 3; i++) {
                if (in_rect(tx, ty, rects[i].x, rects[i].y, rects[i].w, rects[i].h)) {
                    LOG_INFO(&g_logger, "Reconnect button %d hit at (%d,%d)", i, tx, ty);
                    decided = btn_code[i];
                    break;
                }
            }
        }
        if (decided != -2)
            break;

        /* ── Draw screen, only when it actually changed ───────────
         * Nothing on this screen moves except the one-second countdown and its
         * progress bar, but the loop used to clear and repaint the whole 800x455
         * surface plus three buttons every 33 ms — ~15 % of the single core for a
         * static image.  Touch still polls at 30 Hz above, so latency is
         * unchanged. */
        if (remaining != drawn_remaining || focus_on != d_focus_on ||
            (focus_on && focus != d_focus) || ptr_on != d_ptr_on ||
            (ptr_on && (ptr_x != d_ptr_x || ptr_y != d_ptr_y))) {
            vnc_renderer_clear_screen(&g_fb);

            draw_centered_text(&g_fb, 100, "CONNECTION LOST", RGB565_RED, 3);
            draw_centered_text(&g_fb, 160, "RECONNECTING...", RGB565_WHITE, 2);

            {
                char buf[64];
                snprintf(buf, sizeof(buf), "ATTEMPT: %d", attempt);
                draw_centered_text(&g_fb, 220, buf, RGB565_YELLOW, 2);
            }

            {
                char buf[64];
                snprintf(buf, sizeof(buf), "WAITING %ds", remaining);
                draw_centered_text(&g_fb, 270, buf, RGB565(160, 160, 160), 2);
            }

            /* Progress bar (full width = wait_seconds, shrinks as time passes) */
            {
                int bar_x = 100;
                int bar_w_max = (int)g_fb.width - 200;
                int bar_w = (remaining * bar_w_max) / (wait_seconds > 0 ? wait_seconds : 1);
                if (bar_w < 0) bar_w = 0;
                vnc_renderer_fill_rect(&g_fb, bar_x, 310, bar_w_max, 8,
                                       RGB565(40, 40, 40));
                vnc_renderer_fill_rect(&g_fb, bar_x, 310, bar_w, 8,
                                       RGB565_GREEN);
            }

            draw_button(&g_fb, RECONNECT_BTN_CANCEL_X, btn_y,
                        RECONNECT_BTN_W, RECONNECT_BTN_H,
                        "CANCEL", RGB565(40, 40, 40), RGB565_WHITE);
            draw_button(&g_fb, RECONNECT_BTN_SETTINGS_X, btn_y,
                        RECONNECT_BTN_W, RECONNECT_BTN_H,
                        "SETTINGS", RGB565(0, 0, 60), RGB565(100, 150, 255));
            draw_button(&g_fb, RECONNECT_BTN_CONNECT_X, btn_y,
                        RECONNECT_BTN_W, RECONNECT_BTN_H,
                        "CONNECT NOW", RGB565(0, 60, 0), RGB565_GREEN);

            if (focus_on && focus >= 0 && focus < 3)
                vnc_draw_focus_ring(&g_fb, &rects[focus]);
            if (ptr_on)
                vnc_draw_pointer(&g_fb, ptr_x, ptr_y);

            fb_swap(&g_fb);
            drawn_remaining = remaining;
            d_focus_on = focus_on;
            d_focus = focus;
            d_ptr_on = ptr_on;
            d_ptr_x = ptr_x;
            d_ptr_y = ptr_y;
        }

        /* Update countdown */
        struct timeval now;
        gettimeofday(&now, NULL);
        int elapsed = (int)(now.tv_sec - start.tv_sec);
        remaining = wait_seconds - elapsed;

        /* ~30 Hz input poll (the redraw above is gated on the countdown) */
        usleep(33000);
    }

    vnc_nav_close(&nav);
    if (!g_running) return -1;
    if (decided != -2) return decided;
    return 1;   /* countdown expired → try connecting */
}

/* ── VNC session: connect, run main loop, cleanup connection ───────── */

/* Returns:  0 = connection lost (eligible for reconnect)
 *          -1 = connection failed (never connected)
 *           1 = exit gesture (user wants to quit) */
static int vnc_session(const char *host, int port, int attempt) {
    /* ── Show "Connecting" splash ────────────────────────────── */
    vnc_renderer_clear_screen(&g_fb);
    if (attempt > 0) {
        draw_centered_text(&g_fb, 140, "RECONNECTING...", RGB565_YELLOW, 3);
        {
            char att[64];
            snprintf(att, sizeof(att), "ATTEMPT: %d", attempt);
            draw_centered_text(&g_fb, 190, att, RGB565_WHITE, 2);
        }
        {
            char addr[280];
            snprintf(addr, sizeof(addr), "%s:%d", host, port);
            draw_centered_text(&g_fb, 230, addr, RGB565_GREEN, 2);
        }
        draw_centered_text(&g_fb, 280, "WAITING FOR SERVER...", RGB565_GREY, 2);
    } else {
        draw_centered_text(&g_fb, 180, "CONNECTING TO VNC...", RGB565_WHITE, 3);
        {
            char addr[280];
            snprintf(addr, sizeof(addr), "%s:%d", host, port);
            draw_centered_text(&g_fb, 230, addr, RGB565_GREEN, 2);
        }
    }
    fb_swap(&g_fb);

    hw_set_led(LED_RED, 0);
    hw_set_led(LED_GREEN, 50);      /* dim green = connecting */

    /* ── Connect to VNC server ───────────────────────────────── */
    unsigned int ct = (attempt > 0) ? RECONNECT_CONNECT_TIMEOUT
                                    : RECONNECT_INITIAL_CONNECT_TIMEOUT;
    LOG_INFO(&g_logger, "Connecting to %s:%d (timeout %us) ...", host, port, ct);
    g_vnc_client = vnc_client_init(host, port, ct);
    if (!g_vnc_client) {
        vnc_renderer_clear_screen(&g_fb);
        draw_centered_text(&g_fb, 200, "CONNECTION FAILED!", RGB565_RED, 3);
        fb_swap(&g_fb);
        hw_set_led(LED_RED, 100);
        sleep(attempt > 0 ? 1 : 2);   /* shorter flash during reconnect */
        return -1;      /* never connected */
    }
    LOG_INFO(&g_logger, "Connected to VNC server");

    /* ── Initialize renderer ─────────────────────────────────── */
    if (vnc_renderer_init(&g_renderer, &g_fb) < 0) {
        LOG_ERROR(&g_logger, "Failed to initialize renderer");
        vnc_client_destroy(&g_vnc_client);
        return -1;
    }
    vnc_renderer_set_remote_size(&g_renderer,
                                 g_vnc_client->width, g_vnc_client->height);

    /* ── Initialize input handler ────────────────────────────── */
    if (g_touch_ok) {
        if (vnc_input_init(&g_input, &g_touch, &g_renderer, g_vnc_client) < 0) {
            LOG_WARN(&g_logger, "Input handler init failed");
        }
        /* Pass remote desktop dimensions for USB mouse coordinate space */
        vnc_input_set_remote_size(&g_input, g_vnc_client->width, g_vnc_client->height);
    }

    /* Brief "Connected" splash */
    vnc_renderer_clear_screen(&g_fb);
    draw_centered_text(&g_fb, 200, "CONNECTED!", RGB565_GREEN, 3);
    fb_swap(&g_fb);
    usleep(500000);     /* 500 ms */

    /* Clear buffer for VNC content */
    vnc_renderer_clear_screen(&g_fb);
    fb_swap(&g_fb);

    /* ── Main event loop ─────────────────────────────────────── */
    LOG_DEBUG(&g_logger, "Entering main loop (target %d fps)", TARGET_FPS);
    hw_set_led(LED_RED, 0);
    hw_set_led(LED_GREEN, 100);     /* full green = connected */

    int session_result = 0;         /* default: connection lost */

    while (g_running) {
        int result = WaitForMessage(g_vnc_client, 10000);
        if (result < 0) {
            LOG_ERROR(&g_logger, "VNC connection lost");
            break;
        }

        if (result > 0) {
            if (!HandleRFBServerMessage(g_vnc_client)) {
                LOG_ERROR(&g_logger, "Error handling VNC message");
                break;
            }
        }

        /* Process touch input → VNC pointer events + exit gesture */
        if (g_touch_ok) {
            vnc_input_process(&g_input);

            if (vnc_input_exit_requested(&g_input)) {
                LOG_INFO(&g_logger, "Exit gesture detected — opening settings...");
                session_result = 2;     /* open settings */
                break;
            }

            /* Draw exit progress indicator while holding corner.  Drawn at the
             * safe-rect origin so it sits on the zone vnc_input.c hit-tests. */
            float prog = vnc_input_exit_progress(&g_input);
            if (prog > 0.01f) {
                uint16_t *buf = (uint16_t *)g_fb.back_buffer;
                const int stride = (int)g_fb.width;
                int bar_width = (int)(EXIT_ZONE_SIZE * prog);
                uint16_t color = (prog < 0.7f) ? RGB565_YELLOW : RGB565_RED;
                for (int y = SCREEN_SAFE_TOP; y < SCREEN_SAFE_TOP + 3; y++)
                    for (int x = SCREEN_SAFE_LEFT;
                         x < SCREEN_SAFE_LEFT + bar_width && x < stride; x++)
                        buf[y * stride + x] = color;
                g_renderer.needs_present = true;
            }
        }

        vnc_renderer_present(&g_renderer);
    }

    LOG_INFO(&g_logger, "Session ended (result=%d)", session_result);

    /* Cleanup VNC connection (but NOT framebuffer/touch/watchdog).
     * vnc_client_destroy() frees client->frameBuffer, which rfbClientCleanup()
     * does not — this is the reconnect path, so every session would leak one
     * whole framebuffer. */
    vnc_client_destroy(&g_vnc_client);
    vnc_input_cleanup(&g_input);
    /* Note: renderer is NOT cleaned up here (no 32bpp restore).
     * We stay in 16bpp for the reconnect UI. */

    return session_result;
}

/* ── Main application ──────────────────────────────────────────────── */

static int run_vnc_client(const char *host, int port) {
    int ret = -1;

#if USE_16BPP
    fb_set_bpp(FB_DEVICE, 16);
    DEBUG_PRINT("Switched to 16bpp RGB565");
#endif

    /* Initialize framebuffer */
    if (fb_init(&g_fb, FB_DEVICE) < 0) {
        LOG_ERROR(&g_logger, "Failed to initialize framebuffer");
        return -1;
    }
    /* A remote desktop is watched, not touched: never blank the panel. */
    hw_blank_set_enabled(false);
    LOG_INFO(&g_logger, "Framebuffer: %ux%u, %u bpp, size=%zu",
             g_fb.width, g_fb.height, g_fb.bytes_per_pixel * 8, g_fb.screen_size);

    /* Sanity-check: if the app_launcher (or another 32bpp app) ran before us,
     * fb_set_bpp should have fixed it.  Warn if it didn't. */
    if (g_fb.bytes_per_pixel != 2) {
        LOG_WARN(&g_logger, "Framebuffer is %ubpp, expected 16bpp! "
                 "Display may be corrupted.", g_fb.bytes_per_pixel * 8);
    }

    /* Initialize touch input (non-fatal if absent) */
    if (touch_init(&g_touch, TOUCH_DEVICE) < 0) {
        LOG_WARN(&g_logger, "Touch input not available");
    } else {
        g_touch_ok = true;
        LOG_INFO(&g_logger, "Touch input initialized");
    }

    /* Hardware subsystem */
    hw_init();
    hw_set_backlight(100);

    /* Start watchdog feeder (lives across reconnects) */
    if (pthread_create(&g_watchdog_thread, NULL, watchdog_thread_func, NULL) != 0) {
        LOG_WARN(&g_logger, "Failed to start watchdog thread");
    }

    /* ── Reconnect loop ──────────────────────────────────────────── */
    static const int backoff_delays[] = RECONNECT_DELAYS;
    int attempt = 0;

    while (g_running) {
        /* Always use g_config values (may have been updated by settings GUI) */
        int result = vnc_session(g_config.host, g_config.port, attempt);

        if (result == 2) {
            /* Open settings screen */
            LOG_INFO(&g_logger, "Opening settings screen...");

            /* Drain touch before entering settings */
            if (g_touch_ok)
                touch_drain_events(&g_touch);

            int settings_result = vnc_settings_run(&g_config, &g_fb,
                                                    g_touch_ok ? &g_touch : NULL,
                                                    g_config_path);

            if (settings_result == SETTINGS_EXIT) {
                /* Exit to launcher */
                FILE *f = fopen(RESPAWN_CONFIG_FILE, "w");
                if (f) {
                    fprintf(f, "%s\n", APP_LAUNCHER_PATH);
                    fclose(f);
                    LOG_INFO(&g_logger, "Wrote %s to %s",
                             APP_LAUNCHER_PATH, RESPAWN_CONFIG_FILE);
                }
                ret = 0;
                break;
            }

            if (settings_result == SETTINGS_SAVE) {
                /* Config was modified, reconnect with new settings */
                LOG_INFO(&g_logger, "Settings saved, reconnecting with new config: %s:%d",
                         g_config.host, g_config.port);
                /* Re-publish the content-area choice: the renderer keeps it in a
                 * static that only vnc_renderer_set_remote_size() reads, and that
                 * runs once per session.  Without this the CONTENT row would take
                 * effect only after a restart. */
                vnc_content_set_full(g_config.content_full != 0);
                attempt = 0; /* Reset attempt counter for fresh connect */
                continue;    /* Go back to top of reconnect loop */
            }

            /* SETTINGS_BACK — resume VNC session */
            LOG_INFO(&g_logger, "Settings: back to VNC session");
            attempt = 0;
            continue;
        }

        if (result == 1) {
            /* Direct exit (fallback — shouldn't normally happen with settings) */
            FILE *f = fopen(RESPAWN_CONFIG_FILE, "w");
            if (f) {
                fprintf(f, "%s\n", APP_LAUNCHER_PATH);
                fclose(f);
                LOG_INFO(&g_logger, "Wrote %s to %s",
                         APP_LAUNCHER_PATH, RESPAWN_CONFIG_FILE);
            }
            ret = 0;
            break;
        }

        if (!g_running) {
            ret = 0;    /* clean exit via signal */
            break;
        }

#if RECONNECT_ENABLED
        /* Reset attempt counter after a successful session (connection
         * was established then lost).  Failed connections (-1) keep
         * incrementing so the backoff grows while the server is down. */
        if (result == 0)
            attempt = 0;

        /* Connection lost or failed — enter reconnect loop */
        attempt++;
        LOG_INFO(&g_logger, "Connection lost/failed, reconnect attempt %d", attempt);

        if (RECONNECT_MAX_ATTEMPTS > 0 && attempt > RECONNECT_MAX_ATTEMPTS) {
            LOG_INFO(&g_logger, "Max reconnect attempts (%d) reached, exiting",
                     RECONNECT_MAX_ATTEMPTS);
            ret = -1;
            break;
        }

        /* Pick backoff delay (cap at last entry) */
        int delay_idx = attempt - 1;
        if (delay_idx >= RECONNECT_DELAYS_COUNT)
            delay_idx = RECONNECT_DELAYS_COUNT - 1;
        int wait_sec = backoff_delays[delay_idx];

        /* Drain any lingering touch events before showing UI */
        if (g_touch_ok)
            touch_drain_events(&g_touch);

        int ui_result = reconnect_ui(attempt, wait_sec);
        if (ui_result == 0) {
            /* User cancelled */
            LOG_INFO(&g_logger, "User cancelled reconnect");
            ret = 0;
            break;
        }
        if (ui_result == 2) {
            /* User wants settings */
            LOG_INFO(&g_logger, "Opening settings from reconnect screen...");
            if (g_touch_ok)
                touch_drain_events(&g_touch);

            int settings_result = vnc_settings_run(&g_config, &g_fb,
                                                    g_touch_ok ? &g_touch : NULL,
                                                    g_config_path);

            if (settings_result == SETTINGS_EXIT) {
                FILE *f = fopen(RESPAWN_CONFIG_FILE, "w");
                if (f) {
                    fprintf(f, "%s\n", APP_LAUNCHER_PATH);
                    fclose(f);
                    LOG_INFO(&g_logger, "Wrote %s to %s",
                             APP_LAUNCHER_PATH, RESPAWN_CONFIG_FILE);
                }
                ret = 0;
                break;
            }

            if (settings_result == SETTINGS_SAVE) {
                LOG_INFO(&g_logger, "Settings saved from reconnect, trying new config: %s:%d",
                         g_config.host, g_config.port);
                /* Same reason as the in-session SAVE above. */
                vnc_content_set_full(g_config.content_full != 0);
                attempt = 0;
            }
            /* For BACK or SAVE, continue the reconnect loop (will try connecting) */
            continue;
        }
        if (ui_result < 0) {
            /* Signal received */
            break;
        }
        /* ui_result == 1 → try connecting again (top of loop) */

        /* Drain touch events so a lingering press from "Connect Now"
         * doesn't bleed into the next vnc_session. */
        if (g_touch_ok)
            touch_drain_events(&g_touch);
#else
        /* Reconnect disabled — exit on first disconnect */
        ret = 0;
        break;
#endif
    }

    /* ── Final cleanup ───────────────────────────────────────────── */
    vnc_renderer_cleanup(&g_renderer);

    hw_set_led(LED_GREEN, 0);
    hw_set_led(LED_RED, 0);
    hw_set_backlight(100);

    if (g_touch_ok)
        touch_close(&g_touch);
    fb_close(&g_fb);

    /* Stop watchdog thread */
    g_running = false;
    if (g_watchdog_thread)
        pthread_join(g_watchdog_thread, NULL);

    return ret;
}

/* ── Entry point ───────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    /* --help */
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printf("%s VNC Client v2.2\n\n", product_name());
        printf("Usage: vnc_client [OPTIONS] [host [port]]\n\n");
        printf("Options:\n");
        printf("  --config <path>   Config file (default: %s)\n", VNC_CONFIG_FILE);
        printf("  --host <ip>       VNC server host\n");
        printf("  --port <port>     VNC server port\n");
        printf("  --password <pw>   VNC password\n");
        printf("  --help, -h        Show this help\n\n");
        printf("Config file format: key = value (one per line, # for comments)\n");
        printf("Keys: host, port, password, encodings, compress_level, quality_level,\n");
        printf("      content_area (safe|visible — 'safe' keeps the whole remote\n");
        printf("      desktop within finger reach; 'visible' uses the full screen)\n\n");
        printf("Exit: long-press top-left corner for 3 seconds\n");
        return 0;
    }

    printf("╔═══════════════════════════════════════╗\n");
    {   /* the box's inner width is 39 columns; the name comes from os-release */
        char title[40];
        snprintf(title, sizeof(title), "%s VNC Client v2.2", product_name());
        printf("║  %-37.37s║\n", title);
    }
    printf("║  Auto-Reconnect + Bilinear            ║\n");
    printf("╚═══════════════════════════════════════╝\n\n");

    /* Initialize logger (file + stderr) */
    logger_init(&g_logger, "vnc_client", LOG_LEVEL_INFO, true);

    /* 1. Compile-time defaults */
    strncpy(g_config.host, VNC_DEFAULT_HOST, sizeof(g_config.host) - 1);
    g_config.port = VNC_DEFAULT_PORT;
    strncpy(g_config.password, VNC_DEFAULT_PASSWORD, sizeof(g_config.password) - 1);
    strncpy(g_config.encodings, VNC_ENCODINGS, sizeof(g_config.encodings) - 1);
    g_config.compress_level = VNC_COMPRESS_LEVEL;
    g_config.quality_level  = VNC_QUALITY_LEVEL;
    g_config.content_full   = VNC_DEFAULT_CONTENT_FULL;

    /* 2. Config file overrides */
    const char *config_path = VNC_CONFIG_FILE;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            config_path = argv[i + 1];
            break;
        }
    }
    load_config_file(&g_config, config_path);
    g_config_path = config_path;

    /* Publish the content-area choice to the renderer.  Only the remote
     * desktop's letterbox honours it; this app's own touch UI always stays
     * inside SCREEN_SAFE_*. */
    vnc_content_set_full(g_config.content_full != 0);

    /* 3. Command-line overrides (legacy positional: host [port]) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) { i++; continue; }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(g_config.host, argv[++i], sizeof(g_config.host) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_config.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            strncpy(g_config.password, argv[++i], sizeof(g_config.password) - 1);
        } else if (argv[i][0] != '-') {
            /* Legacy positional: first non-flag arg is host, second is port */
            strncpy(g_config.host, argv[i], sizeof(g_config.host) - 1);
            if (i + 1 < argc && argv[i + 1][0] != '-')
                g_config.port = atoi(argv[++i]);
        }
    }

    LOG_INFO(&g_logger, "Config: %s  Target: %s:%d  Encodings: %s  Compress: %d  Quality: %d",
             config_path, g_config.host, g_config.port,
             g_config.encodings, g_config.compress_level, g_config.quality_level);

    printf("Config:    %s\n", config_path);
    printf("Target:    %s:%d\n", g_config.host, g_config.port);
    printf("Encodings: %s\n", g_config.encodings);
    printf("Compress:  %d  Quality: %d\n", g_config.compress_level, g_config.quality_level);
    /* The logical size is not known until fb_init(); run_vnc_client() logs it. */
    printf("Display:   %s\n", USE_16BPP ? "16bpp RGB565" : "32bpp ARGB");
    printf("Exit:      hold top-left corner %d ms, or a pad's Select %d ms\n\n",
           EXIT_HOLD_MS, UI_HOLD_EXIT_MS);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = run_vnc_client(g_config.host, g_config.port);

    LOG_INFO(&g_logger, "VNC client exited (code %d)", ret);
    logger_close(&g_logger);
    printf("\nVNC client exited (code %d)\n", ret);
    return ret;
}
