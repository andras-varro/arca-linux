/* audio_page.c — control_panel's Audio page: enable, music/effects, output
 * device, the TEST chime, and the MIX BUS TEST launch.
 *
 * Opened from the home grid's Audio tile, and the one home for the four audio
 * keys control_panel writes.  MIX BUS TEST is audio_mix_test, run as a child
 * process from here and nowhere else — it has no launcher tile.  Exposed only
 * as cp_audio_page (cp_page.h); its state and its audio bus live in this file.
 *
 * ⚠️ There is no SAVE button, on purpose — the LED page's rule.  Every toggle
 * and every OUT press writes its key the moment it changes, so TEST (which plays
 * the device the OUT button SHOWS) and a game (which plays the SAVED one) can
 * never be talking about two different outputs.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/common.h"
#include "../common/audio.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Deployed by build-and-deploy.sh with no manifest: MIX BUS TEST below is the
 * only route to it. */
#define MIX_TEST_PATH "/opt/games/audio_mix_test"

/* ── State and persistence ─────────────────────────────────────────────── */

/* The OUT values, their labels and their cycle order are common/audio_out.c's
 * choice table, and which of them are listed depends on what is attached — so a
 * new output is a row there, not an edit here. */

typedef struct {
    bool enabled;       /* config key audio_enabled */
    bool music;         /* music_enabled — subordinate to enabled, see layout */
    bool effects;       /* effects_enabled — likewise */
    int  dev_idx;       /* audio_device as SAVED, an AudioOutChoice */
    char bt_addr[18];   /* audio_bt_addr: the preferred headset (BLUETOOTH, AUTO), or "" */
} AudioPageState;

static AudioPageState audio_state;      /* the values, as saved */

/* The page's own bus: open while the page is up, so TEST is a queue and a pump
 * rather than an open, a blocking hold and a close per press. */
static Audio page_audio;
static bool  page_audio_open;
static int   page_audio_idx;            /* the dev_idx it was opened on */

/* What the last paint showed for "is a USB DAC there" — input() compares the
 * live probe against it, so a DAC plugged in or pulled repaints the OUT button
 * without waiting for an unrelated touch. */
static bool shown_usb;
static bool shown_bt;

/* Every value read through config.c's helpers, which are what common/audio.c
 * reads, so the switch on screen cannot disagree with what a game will do —
 * and on a cleared Config they return exactly that default. */
static void audio_page_load(const Config *cfg) {
    audio_state.enabled = config_audio_enabled(cfg);
    audio_state.music   = config_music_enabled(cfg);
    audio_state.effects = config_effects_enabled(cfg);
    /* Unrecognised maps to onboard, as audio_out_device_for() resolves it. */
    audio_state.dev_idx = audio_out_choice_of(config_audio_device(cfg));
    const char *a = config_audio_bt_addr(cfg);
    snprintf(audio_state.bt_addr, sizeof(audio_state.bt_addr), "%s",
             audio_out_bt_addr_valid(a) ? a : "");
}

/* Writes the four keys into the FILE by re-reading it, not by saving the
 * caller's Config, and updates the in-memory copy too — led_page.c's
 * led_persist() and its reasons: a later whole-file save elsewhere then writes
 * these values rather than the old ones, and this one cannot persist anything
 * another page holds unsaved. */
static void audio_persist(const AudioPageState *s, Config *mem) {
    Config *both[2];
    Config disk;
    config_init(&disk);
    config_load(&disk);                 /* a missing file just starts empty */
    both[0] = mem;
    both[1] = &disk;
    for (int i = 0; i < 2; i++) {
        config_set_bool(both[i], "audio_enabled",   s->enabled);
        config_set_bool(both[i], "music_enabled",   s->music);
        config_set_bool(both[i], "effects_enabled", s->effects);
        config_set(both[i], "audio_device", audio_out_choice_name(s->dev_idx));
        config_set(both[i], "audio_bt_addr", s->bt_addr);
    }
    if (config_save(&disk) != 0) {
        fprintf(stderr, "control_panel: audio settings save failed\n");
        cp_status("AUDIO SAVE FAILED", false);
    }
}

/* cp_page.h: OUT for another page's shortcut, through the same state and the
 * same persist as the OUT button, so there is one writer of audio_device.  An
 * open bus follows it on the Audio page's next input(). */
int cp_audio_output(void) { return audio_state.dev_idx; }
const char *cp_audio_bt_addr(void) { return audio_state.bt_addr; }

void cp_audio_set_output(Config *cfg, int choice, const char *bt_addr) {
    if (choice < 0 || choice >= AUDIO_OUT_CHOICE_COUNT) return;
    audio_state.dev_idx = choice;
    snprintf(audio_state.bt_addr, sizeof(audio_state.bt_addr), "%s",
             audio_out_bt_addr_valid(bt_addr) ? bt_addr : "");
    audio_persist(&audio_state, cfg);
}

static void page_audio_close(void) {
    if (!page_audio_open) return;
    audio_close(&page_audio);
    page_audio_open = false;
}

/* Unchecked because the ENABLE gate is applied at the widget instead: TEST is
 * disabled while audio is off, so this never opens against a switched-off
 * setting, and the bus stays ready for the moment it is switched back on.  The
 * _pref form opens the SAVED choice, which is what a game resolves — a saved
 * "usb" with no DAC plays onboard, exactly what the AUTO it shows means.
 *
 * Held for the whole page, not per press: open → two blocking holds → close froze
 * the UI ~0.9 s per TEST and paid a stream start and stop each time.  Also why
 * this is not the library's serviced hold: input() pumps instead, every main-loop
 * iteration (and naming that call here would exempt this file from
 * check-audio-pacing.sh). */
static void page_audio_reopen(void) {
    page_audio_close();
    page_audio_idx  = audio_state.dev_idx;
    page_audio_open = (audio_init_unchecked_pref(&page_audio,
                           audio_out_choice_name(audio_state.dev_idx)) == 0);
}

static void audio_page_enter(void) { page_audio_reopen(); }
static void audio_page_leave(void) { page_audio_close(); }

/* The main loop's pacing question (cp_page.h): true exactly while the stream
 * is open, which is audio_pump_active()'s own answer — FRAME_DELAY_IDLE_US
 * would starve it. */
static bool audio_page_busy(void) {
    return page_audio_open && audio_pump_active(&page_audio);
}

/* For the control panel's RESET DEFAULTS, called AFTER config_clear(cfg):
 * reads the state back from the cleared cfg, so the switches show config.c's
 * defaults — the file gets them when cp_reset_all_defaults() saves the cleared
 * Config right after.  An open bus follows the OUT default. */
static void audio_page_reset_defaults(Config *cfg) {
    audio_page_load(cfg);
    if (page_audio_open && page_audio_idx != audio_state.dev_idx)
        page_audio_reopen();
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

static ToggleSwitch audio_toggle;
static ToggleSwitch music_toggle, effects_toggle;
static Button       audio_dev_btn;
static Button       test_audio_btn;
static Button       mix_test_btn;

/* Row 2 carries MUSIC / EFFECTS / OUT under the master switch; row 3 the
 * MIX BUS TEST launch, alone, so a finger aimed at it lands on nothing else. */
#define AUD_SEC_Y      (CONTENT_Y + CONTENT_TOP_GAP)
#define AUD_ROW2_Y     (AUD_SEC_Y + 54)
#define AUD_ROW3_Y     (AUD_ROW2_Y + 54)
#define AUD_TRACK_W    60
#define AUD_TRACK_H    28

/* ⚠️ The OUT button's scale is pinned to 1 deliberately.  scale 2 needs 144 px
 * of glyphs, which fits landscape and NOT the ~150 px row 2 has left in
 * portrait.  Pinning also keeps the button_init macro out of it: that macro
 * picks scale 3 for any box wider than 150 px, keyed off width alone.
 *
 * ⚠️ button_draw() centres the text and neither pads nor clips it, so a label
 * wider than its box paints outside the button in silence.  The box is therefore
 * measured from the WIDEST label in the whole choice table, listed or not, so the
 * row's geometry does not change when a DAC is plugged in. */
#define AUD_OUT_PREFIX "OUT: "

static void out_label(char *buf, size_t n, int choice) {
    snprintf(buf, n, AUD_OUT_PREFIX "%s", audio_out_choice_label(choice));
}

static int out_label_widest(void) {
    int widest = 0;
    char buf[32];
    for (int c = 0; c < AUDIO_OUT_CHOICE_COUNT; c++) {
        out_label(buf, sizeof(buf), c);
        int w = text_measure_width(buf, 1);
        if (w > widest) widest = w;
    }
    return widest;
}

/* A toggle's hit box right edge, as toggle_check_press() computes it. */
static int toggle_right(const ToggleSwitch *t) {
    return t->x - 5 + t->track_w + text_measure_width(t->label, 1) + 20;
}

/* Re-run whenever the logical screen changes (rebuild_ui()). */
static void audio_page_layout(void) {
    const AudioPageState *s = &audio_state;

    toggle_init(&audio_toggle, CONTENT_LEFT + 5, AUD_SEC_Y + 20,
                AUD_TRACK_W, AUD_TRACK_H, "AUDIO ENABLED", s->enabled);

    /* ── MUSIC / EFFECTS ────────────────────────────────────────────────────
     * The two keys every game reads through common/audio.c's audio_init().  They
     * are SUBORDINATE to AUDIO ENABLED: the master off means the process opens
     * no device at all, so these two decide nothing (common/config.h documents
     * the same hierarchy, and audio_sync_disabled() disables them when it is off).
     *
     * A per-game copy would need a live setter for `Audio.music_on`, a mid-run
     * bed stop, and seven writers of one config key.  This is one writer and no
     * new audio API; the price is that changing them means leaving the game.
     *
     * ⚠️ Widths are MEASURED, not guessed: toggle_draw() puts the label at
     * scale 1 eight pixels right of the track, and that whole box is what
     * toggle_check_press() hit-tests.  Both tracks are FLUSH with the master's
     * at CONTENT_LEFT + 5 — an indent read as a stray row rather than as a
     * child, so the subordination is carried by the disabled state instead. */
    int music_w   = AUD_TRACK_W + 8 + text_measure_width("MUSIC", 1);
    int effects_w = AUD_TRACK_W + 8 + text_measure_width("EFFECTS", 1);
    toggle_init(&music_toggle, CONTENT_LEFT + 5, AUD_ROW2_Y,
                AUD_TRACK_W, AUD_TRACK_H, "MUSIC", s->music);
    toggle_init(&effects_toggle, CONTENT_LEFT + 5 + music_w + 40, AUD_ROW2_Y,
                AUD_TRACK_W, AUD_TRACK_H, "EFFECTS", s->effects);

    /* OUT: the listed output choices as ONE button that cycles, because no
     * multi-choice widget exists in this app.  To the RIGHT of EFFECTS, 40 px
     * on — the gap this row already uses between MUSIC and EFFECTS. */
    int out_x = CONTENT_LEFT + 5 + music_w + 40 + effects_w + 40;
    int out_w = out_label_widest() + 16;                            /* 8 px each side */
    char out_txt[32];
    out_label(out_txt, sizeof(out_txt),
              audio_out_choice_shown(s->dev_idx, audio_out_usb_present(),
                                     audio_out_bt_present()));
    button_init_full(&audio_dev_btn, out_x, AUD_ROW2_Y, out_w, AUD_TRACK_H,
                     out_txt, BTN_COLOR_INFO, COLOR_WHITE, BTN_COLOR_HIGHLIGHT, 1);

    button_init_full(&test_audio_btn, CONTENT_RIGHT - 100, AUD_SEC_Y + 18,
                     90, 30, "TEST", BTN_COLOR_INFO, COLOR_WHITE,
                     BTN_COLOR_HIGHLIGHT, 2);

    /* Flush with the toggles, and sized from its own label at scale 2 plus
     * 8 px a side — button_draw() neither pads nor clips (the OUT note above). */
    button_init_full(&mix_test_btn, CONTENT_LEFT + 5, AUD_ROW3_Y,
                     text_measure_width("MIX BUS TEST", 2) + 16, 30,
                     "MIX BUS TEST", BTN_COLOR_INFO, COLOR_WHITE,
                     BTN_COLOR_HIGHLIGHT, 2);

    /* ⚠️ THE RECEIPT.  Everything here hangs off CONTENT_Y, which comes from a
     * per-unit touch inset, so a row pushed past the touchable rect looks
     * perfect in a screenshot and is dead to a finger.  Row 3 is the lowest
     * row, so MIX BUS TEST sets the bottom.  Row 2 is the one exposed
     * sideways — it stacks three text-derived widths left to right, and the
     * portrait content rect is only about 150 px wider than the first two, so
     * the OUT button is the widget whose right edge can leave it.  All read off
     * the widgets as placed. */
    {
        int bottom = (mix_test_btn.y + mix_test_btn.height) - CONTENT_Y;
        int right  = audio_dev_btn.x + audio_dev_btn.width;
        const ToggleSwitch *tg[3] = { &audio_toggle, &music_toggle, &effects_toggle };
        for (int i = 0; i < 3; i++)
            if (toggle_right(tg[i]) > right) right = toggle_right(tg[i]);
        if (test_audio_btn.x + test_audio_btn.width > right)
            right = test_audio_btn.x + test_audio_btn.width;
        if (mix_test_btn.x + mix_test_btn.width > right)
            right = mix_test_btn.x + mix_test_btn.width;
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: audio stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d (safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw and input ─────────────────────────────────────────────────────── */

/* Derived from the state every time, never cached: with AUDIO ENABLED off,
 * everything on the page except that switch is disabled — MUSIC, EFFECTS, OUT,
 * TEST and MIX BUS TEST.  Widget.disabled makes them grey and deaf to every
 * input, so the master is enforced at the control, not only in the paint. */
static void audio_sync_disabled(void) {
    bool off = !audio_state.enabled;
    music_toggle.disabled   = off;
    effects_toggle.disabled = off;
    audio_dev_btn.disabled  = off;
    test_audio_btn.disabled = off;
    mix_test_btn.disabled   = off;
}

static void audio_page_draw(Framebuffer *fb) {
    const AudioPageState *s = &audio_state;

    audio_sync_disabled();
    draw_section_header(fb, AUD_SEC_Y, "AUDIO");
    audio_toggle.state   = s->enabled;  /* derived every frame, never cached */
    music_toggle.state   = s->music;
    effects_toggle.state = s->effects;
    toggle_draw(fb, &audio_toggle);
    button_draw(fb, &test_audio_btn);
    button_draw(fb, &mix_test_btn);
    toggle_draw(fb, &music_toggle);
    toggle_draw(fb, &effects_toggle);

    /* ⚠️ OUT lists only what is attached: with no /dev/dsp1 the USB entry (and
     * with no Bluetooth sink the BLUETOOTH entry) is not
     * in the cycle, and a saved "usb" SHOWS as AUTO — which is what it is doing,
     * since audio_out_device_for() plays it onboard until the DAC returns.  The
     * SAVED value is not touched (audio_out.h says why); the box keeps its
     * table-wide width, so the row's geometry is card-independent. */
    shown_usb = audio_out_usb_present();
    shown_bt  = audio_out_bt_present();
    char out_txt[32];
    out_label(out_txt, sizeof(out_txt),
              audio_out_choice_shown(s->dev_idx, shown_usb, shown_bt));
    button_set_text(&audio_dev_btn, out_txt);
    button_draw(fb, &audio_dev_btn);
}

static CpPageResult audio_page_input(Config *cfg, int tx, int ty,
                                     bool touching, uint32_t now) {
    AudioPageState *s = &audio_state;
    CpPageResult act = CP_PAGE_IDLE;
    bool changed = false;

    /* Flip from the truth, not a stale widget — and that includes which widgets
     * are disabled, re-derived the moment the master changes. */
    audio_sync_disabled();
    audio_toggle.state   = s->enabled;
    music_toggle.state   = s->music;
    effects_toggle.state = s->effects;
    if (toggle_check_press(&audio_toggle, tx, ty, touching, now)) {
        s->enabled = audio_toggle.state;   changed = true;
        audio_sync_disabled();
    }
    if (toggle_check_press(&music_toggle, tx, ty, touching, now)) {
        s->music = music_toggle.state;     changed = true;
    }
    if (toggle_check_press(&effects_toggle, tx, ty, touching, now)) {
        s->effects = effects_toggle.state; changed = true;
    }
    /* Steps from what the button SHOWS to the next LISTED choice, so with no DAC
     * it cycles onboard -> auto and never offers usb.  The label is not written
     * here; draw() derives it from the index every frame, so the two cannot
     * disagree. */
    if (button_update(&audio_dev_btn, tx, ty, touching, now)) {
        bool usb = audio_out_usb_present(), bt = audio_out_bt_present();
        s->dev_idx = audio_out_choice_next(audio_out_choice_shown(s->dev_idx, usb, bt),
                                           usb, bt);
        /* The pin is KEPT: it is a preference inside the Bluetooth tier, in
         * force under BLUETOOTH and AUTO.  Only the Bluetooth page changes it —
         * USE FOR AUDIO on another headset, or REMOVE on the pinned one. */
        changed = true;
    }
    if (changed) {
        audio_persist(s, cfg);
        act = CP_PAGE_REDRAW;
    }

    /* OUT changed: move the open bus onto it, so the next TEST is heard where
     * the button points.  Also the lazy open for a bus whose page-entry open
     * failed. */
    if (button_update(&test_audio_btn, tx, ty, touching, now)) {
        if (!page_audio_open || page_audio_idx != s->dev_idx)
            page_audio_reopen();
        /* Queued, not played: the audio_pump() below delivers it, so the press
         * returns at once. */
        if (page_audio_open)
            audio_test_chime(&page_audio);
    }
    if (page_audio_open && page_audio_idx != s->dev_idx)
        page_audio_reopen();

    /* ⚠️ Every main-loop iteration: the panel calls input() once per iteration
     * whether or not anything was touched, which makes this the page's frame
     * tick.  audio_page_busy() keeps that tick at FRAME_DELAY_ACTIVE_US while
     * the bus is open; a closed bus is a no-op here. */
    audio_pump(&page_audio);

    if (audio_out_usb_present() != shown_usb || audio_out_bt_present() != shown_bt)
        act = CP_PAGE_REDRAW;           /* a DAC or a BT sink came or went: relist OUT */

    /* Last, so neither REDRAW above can overwrite the queued run. */
    if (button_update(&mix_test_btn, tx, ty, touching, now))
        act = CP_PAGE_FULLSCREEN;
    return act;
}

/* After input() returned CP_PAGE_FULLSCREEN: MIX BUS TEST.
 *
 * Launched rather than linked, by the mechanism control_panel.c's
 * run_touch_diagnostic() uses for touch_raw, with the argv app_launcher gives
 * a manifest's fb,touch: argv[0] the exec path, then the framebuffer and touch
 * devices.  The child owns the panel while it runs; this process is blocked in
 * waitpid() and draws nothing.
 *
 * ⚠️ The page's bus is closed FIRST.  audio_mix_test opens its own stream on
 * the same device, and a second concurrent open is refused EBUSY (measured —
 * common/audio_out.h) — the tool would run silent, which is the one failure a
 * mix-bus test cannot report.  enter()'s reopen restores the bus on return, so
 * TEST works again without leaving the page. */
static void audio_page_run_fullscreen(Framebuffer *fb, TouchInput *touch) {
    if (access(MIX_TEST_PATH, X_OK) != 0) {
        cp_status("AUDIO_MIX_TEST NOT INSTALLED", false);
        return;
    }

    page_audio_close();

    fb_clear(fb, COLOR_BLACK);
    text_draw_centered(fb, (int)fb->width / 2, (int)fb->height / 2,
                       "STARTING MIX BUS TEST", COLOR_WHITE, 3);
    fb_swap(fb);

    pid_t pid = fork();
    if (pid < 0) {
        cp_status("FORK FAILED", false);
        page_audio_reopen();
        return;
    }
    if (pid == 0) {
        execl(MIX_TEST_PATH, MIX_TEST_PATH, FB_DEVICE, TOUCH_DEVICE, (char *)NULL);
        perror("execl audio_mix_test");
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;   /* a signal must not orphan the child */

    /* The child pinned its own depth and left the panel black; the geometry
     * files it reads it does not write, so the depth and a drain are all this
     * process has to put back (touch_raw's return also reloads calibration,
     * because its APPLY rewrites it). */
    fb_set_bpp(FB_DEVICE, 32);
    touch_drain_events(touch);
    page_audio_reopen();

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        cp_status("MIX BUS TEST EXITED WITH AN ERROR", false);
}

/* Keyboard focus: what input() hit-tests, minus whatever the master switch
 * has disabled. */
static int audio_page_focusables(UiRect *out, int max) {
    int n = focus_add_toggle(out, 0, max, &audio_toggle);
    n = focus_add_toggle(out, n, max, &music_toggle);
    n = focus_add_toggle(out, n, max, &effects_toggle);
    n = focus_add_button(out, n, max, &audio_dev_btn);
    n = focus_add_button(out, n, max, &test_audio_btn);
    n = focus_add_button(out, n, max, &mix_test_btn);
    return n;
}

const CpPage cp_audio_page = {
    .name           = "Audio",
    .icon           = "cp_audio",
    .load           = audio_page_load,
    .layout         = audio_page_layout,
    .enter          = audio_page_enter,
    .leave          = audio_page_leave,
    .draw           = audio_page_draw,
    .input          = audio_page_input,
    .focusables     = audio_page_focusables,
    .reset_defaults = audio_page_reset_defaults,
    .run_fullscreen = audio_page_run_fullscreen,
    .busy           = audio_page_busy,
};
