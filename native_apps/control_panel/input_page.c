/* input_page.c — control_panel's Input page: the touch tools, the
 * keyboard, mouse and pad testers, and the player slots.
 *
 * Opened from the home grid's Input tile, and the one home for testing an
 * input device, whatever bus it arrives on: the testers open evdev nodes by
 * path, so a Bluetooth keyboard needs no second copy of them.  The USB bus
 * itself (the device list, RESCAN, port recovery) stays on the USB page.
 * Exposed only as cp_input_page (cp_page.h); its state lives in this file.
 * Its one setting is PLAYERS: which pad or keyboard is P1..P4.  The keys,
 * slot_p1..slot_p4, are written only through gamepad_slot_pin()/unpin() on
 * the panel's GamepadManager (cp_gamepad()), which also keeps the panel's
 * in-memory Config in step; the page never touches them itself.
 *
 * The touch tools (the CALIBRATED row; CALIBRATE, DIAGNOSTIC, MULTI-TOUCH,
 * RESET GEOMETRY) head the page; the testers sit under their own section header.  Each tester's
 * button is disabled while no node of its kind is present.  Calibration, the
 * diagnostic and the geometry reset are control_panel.c's (cp_run_touch_tool(),
 * cp_reset_touch_geometry()); this page only holds their buttons.  A static
 * device set costs nothing: /dev/input is
 * listed once a second while the page is open, the nodes are classified again
 * only when that listing changed, and input() returns CP_PAGE_REDRAW only when
 * the count of some kind did.
 */
#include "cp_page.h"
#include "cp_ui.h"
#include "../common/common.h"
#include "../common/input_scan.h"
#include "../common/ui_flow.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define INPUT_COLOR_PANEL        RGB(30, 30, 45)
#define INPUT_COLOR_PANEL_BD     RGB(50, 50, 70)
#define INPUT_COLOR_DIM          RGB(80, 80, 80)
#define INPUT_COLOR_HDR          COLOR_CYAN
#define INPUT_COLOR_CONN         RGB(0, 200, 80)
#define INPUT_COLOR_KEY_UP       RGB(50, 50, 60)
#define INPUT_COLOR_KEY_DN       RGB(0, 180, 255)
#define INPUT_COLOR_KEY_BD       RGB(80, 80, 100)
#define INPUT_COLOR_KEY_TXT      RGB(200, 200, 200)
#define INPUT_COLOR_STICK_BG     RGB(40, 40, 50)
#define INPUT_COLOR_STICK_DOT    RGB(0, 200, 255)
#define INPUT_COLOR_PAD_ON       RGB(0, 220, 100)
#define INPUT_COLOR_PAD_OFF      RGB(60, 60, 70)
#define INPUT_COLOR_TRIG_BG      RGB(40, 40, 40)
#define INPUT_COLOR_TRIG_FILL    RGB(255, 165, 0)
#define INPUT_COLOR_CURSOR_C     RGB(255, 255, 0)
#define INPUT_COLOR_MBTN_ON      RGB(0, 200, 80)
#define INPUT_COLOR_MBTN_OFF     RGB(60, 60, 70)
#define INPUT_COLOR_SCROLL       RGB(0, 180, 255)
#define INPUT_COLOR_LOG_BG       RGB(15, 15, 25)
#define INPUT_COLOR_LOG_TXT      RGB(150, 200, 150)

/* ── Types and state ────────────────────────────────────────────────────── */

#define MAX_INPUT_DEV     8
#define DEV_NAME_LEN    128
#define LOG_LINES       8
#define LOG_LINE_LEN    64
#define POLL_MS         INPUT_SIG_CHECK_MS   /* how often /dev/input is listed for a hot plug */

#define BITS_PER_LONG   (sizeof(long) * 8)
#define NBITS(x)        ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)          ((x) % BITS_PER_LONG)
#define BIT_LONG(x)     ((x) / BITS_PER_LONG)
#define test_bit(b, a)  ((a[BIT_LONG(b)] >> OFF(b)) & 1)

typedef enum { DEV_UNKNOWN, DEV_KEYBOARD, DEV_MOUSE, DEV_GAMEPAD } DevType;

typedef struct {
    char name[DEV_NAME_LEN]; char path[64];
    DevType type; int ev_num; bool connected;
    bool keys;   /* a MOUSE node that also carries a keyboard (BT keyboard+touchpad) */
    InputPadLayout pad_layout;   /* a pad's raw EV_KEY codes go through input_pad_key() */
} InputDev;

typedef struct {
    bool held[KEY_MAX+1]; int last_code; char last_name[32];
    char log[LOG_LINES][LOG_LINE_LEN]; int log_cnt;
} KbdState;

typedef struct {
    int cx, cy; bool bl, bm, br;
    int scroll; uint32_t scroll_t;
} MouseSt;

typedef struct {
    int lx, ly, rx, ry;
    /* Per open slot (see fds[]): two pads can report different ranges. */
    int amin[MAX_INPUT_DEV][ABS_MAX+1], amax[MAX_INPUT_DEV][ABS_MAX+1];
    int dx, dy; bool btns[16]; int btn_cnt;
    int tl, tr;
} PadSt;

typedef enum { INPUT_SCR_MAIN, INPUT_SCR_KEYBOARD, INPUT_SCR_MOUSE, INPUT_SCR_GAMEPAD,
               INPUT_SCR_MULTITOUCH, INPUT_SCR_CALIBRATE, INPUT_SCR_TOUCH_DIAG } InputScreen;

typedef struct {
    InputScreen  scr;
    InputDev     devs[MAX_INPUT_DEV];   /* evdev nodes the testers can open */
    int          dev_cnt;
    int          kbd_idx, mou_idx, pad_idx;   /* first of each kind, -1 none */
    int          kind_cnt[3];           /* keyboards, mice, pads: what a redraw keys off */
    /* A tester opens EVERY node of its kind at once: a touchpad keyboard exposes
     * its own mouse node, so "the first mouse" is often not the one in the hand. */
    int          fds[MAX_INPUT_DEV];
    int          fd_dev[MAX_INPUT_DEV];  /* devs[] index behind each fd */
    int          fd_cnt;
    int          last_dev;             /* devs[] index of the last event, -1 none */
    KbdState     kbd;
    MouseSt      mou;
    PadSt        pad;
    unsigned long node_sig;            /* /dev/input's listing when last scanned */
    uint32_t     poll_ms;              /* when it was last listed; 0 = list now */
    UiHold       hold;                 /* the tester's exit key: Esc, or a pad's Select/Start */
    UiChord      chord;                /* the mouse tester's LEFT+RIGHT exit */
    /* The mouse tester's exit keys: every keyboard and pad node, read only for
     * Esc and Select/Start (keys are not under test there, so not shown). */
    int          xfds[MAX_INPUT_DEV];
    int          xfd_dev[MAX_INPUT_DEV];
    int          xfd_cnt;
} InputPageState;

static InputPageState input_state;

static Button input_btn_ktest, input_btn_mtest, input_btn_gtest;
static Button input_btn_kback, input_btn_mback, input_btn_gback;
static Button input_btn_multitouch;
static Button input_btn_calibrate, input_btn_touch_diag, input_btn_reset_geom;

/* ── Key table ──────────────────────────────────────────────────────────── */
typedef struct { int code; const char *name, *sname; } KeyInfo;
static const KeyInfo input_ktab[] = {
    {KEY_ESC,"ESC","ESC"},{KEY_1,"1","1"},{KEY_2,"2","2"},{KEY_3,"3","3"},
    {KEY_4,"4","4"},{KEY_5,"5","5"},{KEY_6,"6","6"},{KEY_7,"7","7"},
    {KEY_8,"8","8"},{KEY_9,"9","9"},{KEY_0,"0","0"},{KEY_MINUS,"MINUS","-"},
    {KEY_EQUAL,"EQUAL","="},{KEY_BACKSPACE,"BKSP","BS"},
    {KEY_TAB,"TAB","TAB"},{KEY_Q,"Q","Q"},{KEY_W,"W","W"},{KEY_E,"E","E"},
    {KEY_R,"R","R"},{KEY_T,"T","T"},{KEY_Y,"Y","Y"},{KEY_U,"U","U"},
    {KEY_I,"I","I"},{KEY_O,"O","O"},{KEY_P,"P","P"},
    {KEY_LEFTBRACE,"LBRACE","["},{KEY_RIGHTBRACE,"RBRACE","]"},
    {KEY_ENTER,"ENTER","RET"},
    {KEY_CAPSLOCK,"CAPS","CAP"},{KEY_A,"A","A"},{KEY_S,"S","S"},
    {KEY_D,"D","D"},{KEY_F,"F","F"},{KEY_G,"G","G"},{KEY_H,"H","H"},
    {KEY_J,"J","J"},{KEY_K,"K","K"},{KEY_L,"L","L"},
    {KEY_SEMICOLON,"SEMI",";"},{KEY_APOSTROPHE,"APOS","'"},
    {KEY_BACKSLASH,"BSLASH","\\"},
    {KEY_LEFTSHIFT,"LSHIFT","SHF"},{KEY_Z,"Z","Z"},{KEY_X,"X","X"},
    {KEY_C,"C","C"},{KEY_V,"V","V"},{KEY_B,"B","B"},{KEY_N,"N","N"},
    {KEY_M,"M","M"},{KEY_COMMA,"COMMA",","},{KEY_DOT,"DOT","."},
    {KEY_SLASH,"SLASH","/"},{KEY_RIGHTSHIFT,"RSHIFT","SHF"},
    {KEY_LEFTCTRL,"LCTRL","CTL"},{KEY_LEFTALT,"LALT","ALT"},
    {KEY_SPACE,"SPACE","SPC"},{KEY_RIGHTALT,"RALT","ALT"},
    {KEY_RIGHTCTRL,"RCTRL","CTL"},
    {KEY_UP,"UP","UP"},{KEY_DOWN,"DOWN","DN"},{KEY_LEFT,"LEFT","LT"},
    {KEY_RIGHT,"RIGHT","RT"},
    {KEY_F1,"F1","F1"},{KEY_F2,"F2","F2"},{KEY_F3,"F3","F3"},
    {KEY_F4,"F4","F4"},{KEY_F5,"F5","F5"},{KEY_F6,"F6","F6"},
    {KEY_F7,"F7","F7"},{KEY_F8,"F8","F8"},{KEY_F9,"F9","F9"},
    {KEY_F10,"F10","F10"},{KEY_F11,"F11","F11"},{KEY_F12,"F12","F12"},
    {KEY_GRAVE,"GRAVE","`"},{KEY_DELETE,"DEL","DEL"},{KEY_HOME,"HOME","HOM"},
    {KEY_END,"END","END"},{KEY_PAGEUP,"PGUP","PGU"},
    {KEY_PAGEDOWN,"PGDN","PGD"},{KEY_INSERT,"INS","INS"},
    {0,NULL,NULL}
};

static const char *input_key_name(int c) {
    for (int i=0; input_ktab[i].name; i++) if (input_ktab[i].code==c) return input_ktab[i].name;
    return NULL;
}
static const char *input_key_sname(int c) {
    for (int i=0; input_ktab[i].name; i++) if (input_ktab[i].code==c) return input_ktab[i].sname;
    return NULL;
}

/* ── Keyboard layout ────────────────────────────────────────────────────── */
typedef struct { int col, row, w, code; } LKey;
static const LKey input_kblayout[] = {
    {0,0,2,KEY_ESC},{2,0,2,KEY_1},{4,0,2,KEY_2},{6,0,2,KEY_3},{8,0,2,KEY_4},
    {10,0,2,KEY_5},{12,0,2,KEY_6},{14,0,2,KEY_7},{16,0,2,KEY_8},{18,0,2,KEY_9},
    {20,0,2,KEY_0},{22,0,2,KEY_MINUS},{24,0,2,KEY_EQUAL},{26,0,2,KEY_BACKSPACE},
    {0,1,2,KEY_TAB},{2,1,2,KEY_Q},{4,1,2,KEY_W},{6,1,2,KEY_E},{8,1,2,KEY_R},
    {10,1,2,KEY_T},{12,1,2,KEY_Y},{14,1,2,KEY_U},{16,1,2,KEY_I},{18,1,2,KEY_O},
    {20,1,2,KEY_P},{22,1,2,KEY_LEFTBRACE},{24,1,2,KEY_RIGHTBRACE},{26,1,2,KEY_ENTER},
    {0,2,3,KEY_CAPSLOCK},{3,2,2,KEY_A},{5,2,2,KEY_S},{7,2,2,KEY_D},{9,2,2,KEY_F},
    {11,2,2,KEY_G},{13,2,2,KEY_H},{15,2,2,KEY_J},{17,2,2,KEY_K},{19,2,2,KEY_L},
    {21,2,2,KEY_SEMICOLON},{23,2,2,KEY_APOSTROPHE},{25,2,3,KEY_BACKSLASH},
    {0,3,3,KEY_LEFTSHIFT},{3,3,2,KEY_Z},{5,3,2,KEY_X},{7,3,2,KEY_C},{9,3,2,KEY_V},
    {11,3,2,KEY_B},{13,3,2,KEY_N},{15,3,2,KEY_M},{17,3,2,KEY_COMMA},{19,3,2,KEY_DOT},
    {21,3,2,KEY_SLASH},{23,3,5,KEY_RIGHTSHIFT},
    {0,4,3,KEY_LEFTCTRL},{3,4,3,KEY_LEFTALT},{6,4,16,KEY_SPACE},
    {22,4,3,KEY_RIGHTALT},{25,4,3,KEY_RIGHTCTRL},
    {-1,-1,-1,-1}
};

/* ── Device helpers ─────────────────────────────────────────────────────── */
/* Classification, the touchscreen exclusion and the /dev/input/event* walk are
 * common/input_scan.c's.  This list only says what is there — the testers
 * reopen by path — so every node input_scan() opened is closed again here. */
static void input_scan_devices(InputPageState *s) {
    static const int cap[INPUT_KIND_COUNT] = {
        [INPUT_KIND_KEYBOARD] = MAX_INPUT_DEV,
        [INPUT_KIND_MOUSE]    = MAX_INPUT_DEV,
        [INPUT_KIND_PAD]      = MAX_INPUT_DEV,
    };
    InputNode nodes[MAX_INPUT_DEV];
    int n = input_scan(nodes, 0, MAX_INPUT_DEV, cap);
    s->dev_cnt=0; s->kbd_idx=s->mou_idx=s->pad_idx=-1;
    memset(s->kind_cnt, 0, sizeof(s->kind_cnt));
    for (int i=0; i<n; i++) {
        const InputNode *nd=&nodes[i];
        close(nd->fd);
        DevType t = nd->kind==INPUT_KIND_KEYBOARD ? DEV_KEYBOARD
                  : nd->kind==INPUT_KIND_MOUSE    ? DEV_MOUSE : DEV_GAMEPAD;
        InputDev *d=&s->devs[s->dev_cnt];
        snprintf(d->name,sizeof(d->name),"%s",nd->name[0] ? nd->name : "Unknown");
        snprintf(d->path,sizeof(d->path),"%.*s",(int)sizeof(nd->path),nd->path);
        d->ev_num=-1; sscanf(nd->path,"/dev/input/event%d",&d->ev_num);
        d->type=t; d->connected=true;
        d->keys = (t==DEV_MOUSE && nd->keys);
        d->pad_layout = nd->pad_layout;
        /* Only "is there one" — the testers open every node of the kind. */
        if (t==DEV_KEYBOARD && s->kbd_idx<0) s->kbd_idx=s->dev_cnt;
        else if (t==DEV_MOUSE && s->mou_idx<0) s->mou_idx=s->dev_cnt;
        else if (t==DEV_GAMEPAD && s->pad_idx<0) s->pad_idx=s->dev_cnt;
        if (d->keys && s->kbd_idx<0) s->kbd_idx=s->dev_cnt;
        s->kind_cnt[t-DEV_KEYBOARD]++;
        if (d->keys) s->kind_cnt[DEV_KEYBOARD-DEV_KEYBOARD]++;
        s->dev_cnt++;
    }
}

static void input_close(InputPageState *s);   /* defined below */

/* Open every scanned node of kind t, all non-blocking; the tester drains each
 * one every frame, so any of two mice (or keyboards, or pads) drives it without
 * the operator having to pick one. Returns how many opened. */
static int input_open_kind(InputPageState *s, DevType t) {
    input_close(s);
    for (int i=0; i<s->dev_cnt && s->fd_cnt<MAX_INPUT_DEV; i++) {
        if (s->devs[i].type!=t && !(t==DEV_KEYBOARD && s->devs[i].keys)) continue;
        int fd=open(s->devs[i].path, O_RDONLY|O_NONBLOCK);
        if (fd<0) continue;
        s->fds[s->fd_cnt]=fd;
        s->fd_dev[s->fd_cnt]=i;
        s->fd_cnt++;
    }
    return s->fd_cnt;
}

/* The mouse tester's exit keys: every keyboard and pad node, alongside the
 * mice input_open_kind() opened.  A keyboard+touchpad combo is skipped — it is
 * a mouse node, already open, and input_proc_mouse() reads its Esc. */
static void input_open_exit_keys(InputPageState *s) {
    for (int i=0; i<s->dev_cnt && s->xfd_cnt<MAX_INPUT_DEV; i++) {
        if (s->devs[i].type!=DEV_KEYBOARD && s->devs[i].type!=DEV_GAMEPAD) continue;
        int fd=open(s->devs[i].path, O_RDONLY|O_NONBLOCK);
        if (fd<0) continue;
        s->xfds[s->xfd_cnt]=fd;
        s->xfd_dev[s->xfd_cnt]=i;
        s->xfd_cnt++;
    }
}

static void input_close(InputPageState *s) {
    for (int k=0; k<s->fd_cnt; k++)
        if (s->fds[k]>=0) close(s->fds[k]);
    for (int k=0; k<s->xfd_cnt; k++)
        if (s->xfds[k]>=0) close(s->xfds[k]);
    s->fd_cnt=0;
    s->xfd_cnt=0;
    s->last_dev=-1;
}

static void input_load_axes(InputPageState *s, int slot, int fd) {
    memset(s->pad.amin[slot],0,sizeof(s->pad.amin[slot]));
    memset(s->pad.amax[slot],0,sizeof(s->pad.amax[slot]));
    unsigned long ab[NBITS(ABS_MAX)]={0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(ab)), ab)<0) return;
    for (int a=0; a<=ABS_MAX; a++) {
        if (test_bit(a,ab)) {
            struct input_absinfo inf;
            if (ioctl(fd, EVIOCGABS(a), &inf)==0)
                { s->pad.amin[slot][a]=inf.minimum; s->pad.amax[slot][a]=inf.maximum; }
        }
    }
}

/* One line naming the node behind the last event, node first so a truncated
 * product name still says which /dev/input/event* it was. */
static void input_src_line(const InputPageState *s, char *out, size_t out_size,
                           int max_w, int scale) {
    char raw[DEV_NAME_LEN+32];
    if (s->last_dev>=0 && s->last_dev<s->dev_cnt) {
        const InputDev *d=&s->devs[s->last_dev];
        snprintf(raw,sizeof(raw),"FROM EVENT%d: %s",d->ev_num,d->name);
    } else {
        snprintf(raw,sizeof(raw),"LISTENING ON %d NODE%s - USE ANY",
                 s->fd_cnt, s->fd_cnt==1?"":"S");
    }
    text_truncate(out, out_size, raw, max_w, scale);
}

static int input_norm_axis(int v, int mn, int mx) {
    if (mx==mn) return 0;
    int mid=(mn+mx)/2, hr=(mx-mn)/2;
    if (!hr) return 0;
    int n=((v-mid)*1000)/hr;
    return n<-1000?-1000:n>1000?1000:n;
}

static int input_norm_trig(int v, int mn, int mx) {
    if (mx==mn) return 0;
    int n=((v-mn)*1000)/(mx-mn);
    return n<0?0:n>1000?1000:n;
}

static void input_add_log(KbdState *k, const char *m) {
    if (k->log_cnt>=LOG_LINES) {
        for(int i=0;i<LOG_LINES-1;i++) memcpy(k->log[i],k->log[i+1],LOG_LINE_LEN);
        k->log_cnt=LOG_LINES-1;
    }
    strncpy(k->log[k->log_cnt],m,LOG_LINE_LEN-1);
    k->log[k->log_cnt][LOG_LINE_LEN-1]=0;
    k->log_cnt++;
}

/* ── Event processing ───────────────────────────────────────────────────── */
/* Each returns how many events it applied to the tester's state — the ones
 * that can change what the screen shows (EV_SYN, EV_MSC and the like are read
 * and dropped uncounted).  Zero means the frame would repaint identically, so
 * input_page_run_fullscreen() skips it. */
static int input_proc_kbd(InputPageState *s) {
  int n=0;
  for (int k=0; k<s->fd_cnt; k++) {
    int ev=s->devs[s->fd_dev[k]].ev_num;
    struct input_event e;
    while (read(s->fds[k],&e,sizeof(e))==(ssize_t)sizeof(e)) {
        if (e.type!=EV_KEY || e.code>=KEY_MAX) continue;
        /* Esc is still shown like every key; held, it also fills the exit
         * bar.  Its autorepeat during a hold is logged but not counted, so
         * the frame stays a bar-only repaint — the log shows on release. */
        bool hold_repeat = (e.code==KEY_ESC && e.value==2 && s->hold.down);
        if (e.code==KEY_ESC) ui_hold_key(&s->hold, e.value, get_time_ms());
        if (!hold_repeat) n++;
        s->last_dev=s->fd_dev[k];
        const char *nm=input_key_name(e.code);
        char nb[32]; if(!nm){snprintf(nb,32,"KEY_%d",e.code);nm=nb;}
        char lm[LOG_LINE_LEN];
        if (e.value==1) {
            s->kbd.held[e.code]=true; s->kbd.last_code=e.code;
            strncpy(s->kbd.last_name,nm,31); s->kbd.last_name[31]=0;
            snprintf(lm,sizeof(lm),"PRESS   %s (%d) EV%d",nm,e.code,ev);
            input_add_log(&s->kbd,lm);
        } else if (e.value==0) {
            s->kbd.held[e.code]=false;
            snprintf(lm,sizeof(lm),"RELEASE %s (%d) EV%d",nm,e.code,ev);
            input_add_log(&s->kbd,lm);
        } else if (e.value==2) {
            snprintf(lm,sizeof(lm),"REPEAT  %s (%d) EV%d",nm,e.code,ev);
            input_add_log(&s->kbd,lm);
        }
    }
  }
  return n;
}

static int input_proc_mouse(const Framebuffer *fb, InputPageState *s) {
  int n=0;
  for (int k=0; k<s->fd_cnt; k++) {
    struct input_event e;
    while (read(s->fds[k],&e,sizeof(e))==(ssize_t)sizeof(e)) {
        if (e.type==EV_KEY && e.code==KEY_ESC) {
            /* A keyboard+touchpad combo's Esc: the exit key, not shown. */
            ui_hold_key(&s->hold, e.value, get_time_ms());
            continue;
        }
        if (e.type==EV_REL || e.type==EV_KEY) { s->last_dev=s->fd_dev[k]; n++; }
        if (e.type==EV_REL) {
            if (e.code==REL_X) {
                s->mou.cx+=e.value;
                if(s->mou.cx<0) s->mou.cx=0;
                if(s->mou.cx>=(int)fb->width) s->mou.cx=(int)fb->width-1;
            } else if (e.code==REL_Y) {
                s->mou.cy+=e.value;
                if(s->mou.cy<0) s->mou.cy=0;
                if(s->mou.cy>=(int)fb->height) s->mou.cy=(int)fb->height-1;
            } else if (e.code==REL_WHEEL) {
                s->mou.scroll+=e.value; s->mou.scroll_t=get_time_ms();
            }
        } else if (e.type==EV_KEY) {
            bool p=(e.value!=0);
            if (e.code==BTN_LEFT) s->mou.bl=p;
            else if (e.code==BTN_MIDDLE) s->mou.bm=p;
            else if (e.code==BTN_RIGHT) s->mou.br=p;
            /* Each button is still shown alone; held TOGETHER they exit. */
            if (e.code==BTN_LEFT || e.code==BTN_RIGHT)
                ui_chord_button(&s->chord, e.code==BTN_RIGHT, e.value, get_time_ms());
        }
    }
  }
  return n;
}

/* The mouse tester's exit keys (input_open_exit_keys()): Esc, and a pad's
 * Select/Start in native codes, feed the hold; nothing else is looked at and
 * nothing is shown, so this never asks for a repaint. */
static void input_proc_exit_keys(InputPageState *s) {
  for (int k=0; k<s->xfd_cnt; k++) {
    const InputDev *d=&s->devs[s->xfd_dev[k]];
    struct input_event e;
    while (read(s->xfds[k],&e,sizeof(e))==(ssize_t)sizeof(e)) {
        if (e.type!=EV_KEY) continue;
        int c = d->type==DEV_GAMEPAD ? input_pad_key(d->pad_layout, e.code) : e.code;
        if (d->type==DEV_KEYBOARD ? c==KEY_ESC : (c==BTN_SELECT || c==BTN_START))
            ui_hold_key(&s->hold, e.value, get_time_ms());
    }
  }
}

static int input_proc_pad(InputPageState *s) {
  int n=0;
  for (int k=0; k<s->fd_cnt; k++) {
    const int *mn=s->pad.amin[k], *mx=s->pad.amax[k];
    struct input_event e;
    while (read(s->fds[k],&e,sizeof(e))==(ssize_t)sizeof(e)) {
        if (e.type==EV_ABS || e.type==EV_KEY) { s->last_dev=s->fd_dev[k]; n++; }
        if (e.type==EV_ABS) {
            int c=e.code;
            if (c>ABS_MAX) continue;
            if (c==ABS_X) s->pad.lx=input_norm_axis(e.value,mn[c],mx[c]);
            else if (c==ABS_Y) s->pad.ly=input_norm_axis(e.value,mn[c],mx[c]);
            else if (c==ABS_RX||c==ABS_Z) s->pad.rx=input_norm_axis(e.value,mn[c],mx[c]);
            else if (c==ABS_RY||c==ABS_RZ) s->pad.ry=input_norm_axis(e.value,mn[c],mx[c]);
            else if (c==ABS_HAT0X) s->pad.dx=e.value>0?1:e.value<0?-1:0;
            else if (c==ABS_HAT0Y) s->pad.dy=e.value>0?1:e.value<0?-1:0;
            else if (c==ABS_BRAKE) s->pad.tl=input_norm_trig(e.value,mn[c],mx[c]);
            else if (c==ABS_GAS) s->pad.tr=input_norm_trig(e.value,mn[c],mx[c]);
        } else if (e.type==EV_KEY) {
            /* Native codes, as every app sees them: a hid-generic pad's
             * Select/Start would otherwise show as buttons 6/7 and fill the
             * LB/RB bars. */
            int c=input_pad_key(s->devs[s->fd_dev[k]].pad_layout, e.code);
            int bi=-1;
            if (c>=BTN_GAMEPAD && c<BTN_GAMEPAD+16) bi=c-BTN_GAMEPAD;
            else if (c>=BTN_SOUTH && c<=BTN_THUMBR) bi=c-BTN_SOUTH;
            else if (c>=BTN_TRIGGER && c<BTN_TRIGGER+16) bi=c-BTN_TRIGGER;
            if (bi>=0 && bi<16) {
                s->pad.btns[bi]=(e.value!=0);
                if (bi>=s->pad.btn_cnt) s->pad.btn_cnt=bi+1;
            }
            /* Select and Start, held: cp_key_back()'s BACK and PAUSE under the
             * default input_config.conf map (a remapped pad still exits by
             * these, and by touch). */
            if (c==BTN_SELECT || c==BTN_START)
                ui_hold_key(&s->hold, e.value, get_time_ms());
            if (c==BTN_TL) s->pad.tl=e.value?1000:0;
            if (c==BTN_TR) s->pad.tr=e.value?1000:0;
        }
    }
  }
  return n;
}

/* ── Layout ─────────────────────────────────────────────────────────────── */

#define INPUT_BTN_H       40
#define INPUT_BTN_GAP     10
#define INPUT_BTN_MAX_W   200
/* The top band, for the touch tools: a section header, the CALIBRATED row and
 * one row of buttons (26 + 28 + 44), then the gap a section leaves before the
 * next (20). */
#define INPUT_CALIB_ROW_H 28   /* draw_info_row()'s advance */
#define INPUT_CALIB_NO    "NOT CALIBRATED"   /* the row's longer value */
#define INPUT_TOUCH_H     (90 + INPUT_CALIB_ROW_H)

/* Whether /etc/touch_calibration.conf exists: the row's value, and its colour.
 * Asked on every draw, which is only on a change, so a calibration just saved
 * shows on the repaint that follows the wizard. */
static const char *calib_row_value(uint32_t *color) {
    bool ok = (access(CALIB_FILE, 0) == 0);
    *color = ok ? COLOR_GREEN : COLOR_YELLOW;
    return ok ? "CALIBRATED" : INPUT_CALIB_NO;
}

static Button *const test_btns[3] = { &input_btn_ktest, &input_btn_mtest, &input_btn_gtest };
static const char *const test_labels[3] = { "KBD TEST", "MOUSE TEST", "PAD TEST" };

/* The touch tools' row: four slots, so each button keeps its place as the rest
 * join.  A NULL slot is not placed, drawn or counted. */
enum { TOUCH_SLOT_CALIB, TOUCH_SLOT_DIAG, TOUCH_SLOT_MULTI, TOUCH_SLOT_RESET, TOUCH_SLOTS };
static Button *const touch_btns[TOUCH_SLOTS] = {
    [TOUCH_SLOT_CALIB] = &input_btn_calibrate,
    [TOUCH_SLOT_DIAG]  = &input_btn_touch_diag,
    [TOUCH_SLOT_MULTI] = &input_btn_multitouch,
    [TOUCH_SLOT_RESET] = &input_btn_reset_geom,
};
/* At most 14 characters: a landscape quarter (184 px) holds that at scale 2;
 * portrait wraps them into rows instead of shrinking the font. */
static const char *const touch_labels[TOUCH_SLOTS] = {
    [TOUCH_SLOT_CALIB] = "CALIBRATE",
    [TOUCH_SLOT_DIAG]  = "DIAGNOSTIC",
    [TOUCH_SLOT_MULTI] = "MULTI-TOUCH",
    [TOUCH_SLOT_RESET] = "RESET GEOMETRY",
};
/* RESET is the escape hatch from a bad calibration, so it reads as danger;
 * the diagnostic, the one tool with every layer of interpretation removed,
 * keeps its own colour. */
static const uint32_t touch_colors[TOUCH_SLOTS] = {
    [TOUCH_SLOT_CALIB] = BTN_COLOR_PRIMARY,
    [TOUCH_SLOT_DIAG]  = RGB(100, 60, 120),
    [TOUCH_SLOT_MULTI] = BTN_COLOR_PRIMARY,
    [TOUCH_SLOT_RESET] = BTN_COLOR_DANGER,
};

static int sec_touch_y;       /* the touch tools' section header */
static int touch_scale;       /* their row's text scale, see layout */
static int sec_test_y;        /* the testers' section header */
static int count_y[3];        /* the "N FOUND" line under each tester button */
static int test_scale;        /* the row's text scale, see layout */
static int touch_rows, test_rows;   /* rows the flow used, see layout */
static int touch_h;           /* the touch band's height with those rows */

/* Rows are where a band wraps, never what its font does: the button keeps
 * scale 2 and widens to the longest label (ui_label_fits is the one rule). */
#define INPUT_TEST_ROW_GAP 26   /* under a tester button: its count line, then air */

/* The width every button of a band takes at scale: the share the row would
 * give it, widened to the longest label.  Never narrower than the share, so a
 * band that fits today is unchanged. */
static int band_item_w(const char *const *labels, int n, int share, int scale) {
    int w = share;
    for (int i = 0; i < n; i++)
        if (labels[i])
            while (!ui_label_fits(labels[i], scale, w)) w++;
    return w;
}

/* ── PLAYERS: which pad or keyboard is P1..P4 ───────────────────────────── */
/* One Cycler per player slot.  Its entries are AUTO (unpinned), then every
 * connected pad and keyboard in gamepad_devices()' order, then — only while
 * the slot is pinned to a device that is not in that list — the pin itself,
 * so stepping off it in either direction is possible and it is never offered
 * again once left.  Every entry list is rebuilt from cp_gamepad() on each
 * draw and each input(): the panel's main loop rescans the devices, so an
 * index kept across frames would point at a different controller. */
#define PLAYER_ROW_H     40
#define PLAYER_ROW_GAP   8
#define PLAYER_LABEL_W   36   /* "P1" at scale 2, and a gap before the cycler */
#define PLAYER_COL_GAP   20
#define PLAYER_TEXT_LEN  96
#define PLAYER_MAX_DEVS  (GAMEPAD_MAX_PADS + GAMEPAD_MAX_PER_KIND)
#define PLAYER_COLOR_AUTO    COLOR_LABEL    /* unpinned: the slot follows plug order */
#define PLAYER_COLOR_PINNED  COLOR_WHITE
#define PLAYER_COLOR_LOST    COLOR_YELLOW   /* pinned, not connected */

static Cycler player_cyc[INPUT_SLOTS];
static int    sec_players_y;   /* the PLAYERS section header */
static int    player_cols;     /* 2 in landscape (P1 P2 / P3 P4), 1 in portrait */
/* What the last draw painted per row, so input() repaints only on a change. */
static char   player_shown[INPUT_SLOTS][PLAYER_TEXT_LEN];
static bool   player_shown_dis[INPUT_SLOTS];

typedef struct {
    int      count;   /* entries */
    int      cur;     /* the entry the slot is at now */
    uint32_t color;
    char     text[PLAYER_TEXT_LEN];
} PlayerRow;

typedef struct {
    GamepadDevice dev[PLAYER_MAX_DEVS];
    int           n;
} PlayerDevs;

static void player_devs(PlayerDevs *d) {
    d->n = gamepad_devices(cp_gamepad(), d->dev, PLAYER_MAX_DEVS);
}

/* No name is stored with a pin, so an absent pinned device is shown by its
 * identity: a Bluetooth pad by its MAC (uniq), a wired one by its USB port —
 * the phys stem's last "-" field, "1.2" of "usb-musb-hdrc.1.auto-1.2/input0". */
static void player_ident_short(const InputIdent *id, char *out, size_t n) {
    const char *bus = id->bus == BUS_BLUETOOTH ? "BT"
                    : id->bus == BUS_USB       ? "USB" : "ID";
    if (id->uniq[0]) {
        snprintf(out, n, "%s %s", bus, id->uniq);
        return;
    }
    char stem[sizeof(id->phys)];
    snprintf(stem, sizeof(stem), "%s", id->phys);
    char *sl = strrchr(stem, '/');
    if (sl && strncmp(sl, "/input", 6) == 0) *sl = '\0';
    const char *port = strrchr(stem, '-');
    port = (port && port[1]) ? port + 1 : stem;
    snprintf(out, n, "%s PORT %s", bus, port[0] ? port : "?");
}

static void player_row(const PlayerDevs *d, int slot, PlayerRow *r) {
    InputIdent id;
    bool pinned = false, present = false;
    bool held = gamepad_slot_info(cp_gamepad(), slot, &id, &pinned, &present);
    int at = -1;
    for (int i = 0; held && i < d->n; i++)
        if (input_ident_equal(&d->dev[i].ident, &id)) { at = i; break; }
    bool extra = held && pinned && at < 0;
    r->count = 1 + d->n + (extra ? 1 : 0);
    if (held && pinned && at >= 0) {
        r->cur = 1 + at;
        r->color = PLAYER_COLOR_PINNED;
        if (d->dev[at].name[0]) snprintf(r->text, sizeof(r->text), "%s", d->dev[at].name);
        else player_ident_short(&id, r->text, sizeof(r->text));
    } else if (extra) {
        char who[64];
        player_ident_short(&id, who, sizeof(who));
        r->cur = 1 + d->n;
        r->color = present ? PLAYER_COLOR_PINNED : PLAYER_COLOR_LOST;
        snprintf(r->text, sizeof(r->text), "%s%s", who, present ? "" : " (UNPLUGGED)");
    } else {
        r->cur = 0;
        r->color = PLAYER_COLOR_AUTO;
        if (held && present && at >= 0)
            snprintf(r->text, sizeof(r->text), "AUTO: %s",
                     d->dev[at].name[0] ? d->dev[at].name : "?");
        else
            snprintf(r->text, sizeof(r->text), "AUTO");
    }
}

/* Steps slot by dir through its entries and pins or unpins to match, keeping
 * cfg in step.  A controller pinned elsewhere leaves that slot, which reverts
 * to AUTO — input_slots does that; the next draw reads it fresh. */
static bool player_step(Config *cfg, int slot, int dir) {
    PlayerDevs d;
    PlayerRow r;
    player_devs(&d);
    player_row(&d, slot, &r);
    int next = cycler_step(r.cur, r.count, dir);
    if (next == r.cur || next > d.n) return false;
    int rc = next == 0 ? gamepad_slot_unpin(cp_gamepad(), slot, cfg)
                       : gamepad_slot_pin(cp_gamepad(), &d.dev[next - 1].ident, slot, cfg);
    if (rc == -2) cp_status("PLAYER SET, BUT NOT SAVED", false);
    return true;
}

/* Re-run whenever the logical screen changes (rebuild_ui()).  Each row shares
 * its width out among its slots — a quarter of the content for the touch tools,
 * a third for the testers — up to INPUT_BTN_MAX_W, centred under its header. */
static void input_page_layout(void) {
    sec_touch_y = CONTENT_Y + CONTENT_TOP_GAP;
    int touch_by = sec_touch_y + 26 + INPUT_CALIB_ROW_H;
    int touch_bw = (CONTENT_WIDTH - (TOUCH_SLOTS - 1) * INPUT_BTN_GAP) / TOUCH_SLOTS;
    if (touch_bw > INPUT_BTN_MAX_W) touch_bw = INPUT_BTN_MAX_W;
    int test_bw = (CONTENT_WIDTH - 2 * INPUT_BTN_GAP) / 3;
    if (test_bw > INPUT_BTN_MAX_W) test_bw = INPUT_BTN_MAX_W;
    player_cols = CONTENT_WIDTH >= 600 ? 2 : 1;
    int player_rows = (INPUT_SLOTS + player_cols - 1) / player_cols;

    /* Two passes.  Pass 0 keeps scale 2 and wraps; it is kept unless the stack
     * then runs past CONTENT_H, or a single button per row still cannot hold
     * its label at scale 2.  Pass 1 is the old shape: scale 1, one row each. */
    int touch_w = 0, test_w = 0;
    for (int pass = 0; pass < 2; pass++) {
        int sc = pass == 0 ? 2 : 1;
        touch_scale = test_scale = sc;
        touch_w = band_item_w(touch_labels, TOUCH_SLOTS, touch_bw, sc);
        test_w  = band_item_w(test_labels, 3, test_bw, sc);
        if (pass == 0 && (touch_w > CONTENT_WIDTH || test_w > CONTENT_WIDTH)) continue;
        UiRect probe[TOUCH_SLOTS];
        touch_rows = ui_flow_place(TOUCH_SLOTS, touch_w, INPUT_BTN_H, INPUT_BTN_GAP,
                                   INPUT_BTN_GAP, CONTENT_LEFT, touch_by,
                                   CONTENT_WIDTH, TOUCH_SLOTS, 1, probe);
        test_rows = ui_flow_place(3, test_w, INPUT_BTN_H, INPUT_BTN_GAP,
                                  INPUT_TEST_ROW_GAP, CONTENT_LEFT, 0,
                                  CONTENT_WIDTH, 3, 1, probe);
        touch_h = INPUT_TOUCH_H + (touch_rows - 1) * (INPUT_BTN_H + INPUT_BTN_GAP);
        int players_y = CONTENT_Y + CONTENT_TOP_GAP + touch_h + 26
                      + (test_rows - 1) * (INPUT_BTN_H + INPUT_TEST_ROW_GAP)
                      + INPUT_BTN_H + 8 + 8 + 10;
        int bottom = players_y + 26 + (player_rows - 1) * (PLAYER_ROW_H + PLAYER_ROW_GAP)
                   + PLAYER_ROW_H - CONTENT_Y;
        if (bottom <= CONTENT_H) break;
    }
    {
        UiRect r[TOUCH_SLOTS];
        ui_flow_place(TOUCH_SLOTS, touch_w, INPUT_BTN_H, INPUT_BTN_GAP, INPUT_BTN_GAP,
                      CONTENT_LEFT, touch_by, CONTENT_WIDTH, TOUCH_SLOTS, 1, r);
        for (int i = 0; i < TOUCH_SLOTS; i++)
            if (touch_btns[i])
                button_init_full(touch_btns[i], r[i].x, r[i].y, r[i].w, r[i].h,
                                 touch_labels[i], touch_colors[i], COLOR_WHITE,
                                 RGB(0,200,80), touch_scale);
    }
    sec_test_y = CONTENT_Y + CONTENT_TOP_GAP + touch_h;
    int by = sec_test_y + 26;
    {
        UiRect r[3];
        ui_flow_place(3, test_w, INPUT_BTN_H, INPUT_BTN_GAP, INPUT_TEST_ROW_GAP,
                      CONTENT_LEFT, by, CONTENT_WIDTH, 3, 1, r);
        for (int i = 0; i < 3; i++) {
            button_init_full(test_btns[i], r[i].x, r[i].y, r[i].w, r[i].h,
                             test_labels[i], BTN_COLOR_PRIMARY, COLOR_WHITE,
                             RGB(0,200,80), test_scale);
            count_y[i] = r[i].y + INPUT_BTN_H + 8;
        }
    }
    button_init_full(&input_btn_kback, SCREEN_SAFE_LEFT+10, SCREEN_SAFE_TOP+8,
                     90, 40, "< BACK", BTN_COLOR_WARNING, COLOR_WHITE, RGB(255,200,0), 2);
    button_init_full(&input_btn_mback, SCREEN_SAFE_LEFT+10, SCREEN_SAFE_TOP+8,
                     90, 40, "< BACK", BTN_COLOR_WARNING, COLOR_WHITE, RGB(255,200,0), 2);
    button_init_full(&input_btn_gback, SCREEN_SAFE_LEFT+10, SCREEN_SAFE_TOP+8,
                     90, 40, "< BACK", BTN_COLOR_WARNING, COLOR_WHITE, RGB(255,200,0), 2);

    /* PLAYERS under the testers: two columns where the content is landscape
     * wide (four full-width rows would end ~20 px past a 375 px CONTENT_H),
     * one column of four in portrait.  Row-major, so P1 P2 / P3 P4. */
    sec_players_y = count_y[2] + 8 + 10;
    {
        int col_w = (CONTENT_WIDTH - (player_cols - 1) * PLAYER_COL_GAP) / player_cols;
        for (int s = 0; s < INPUT_SLOTS; s++) {
            int col = s % player_cols, row = s / player_cols;
            cycler_init(&player_cyc[s],
                        CONTENT_LEFT + col * (col_w + PLAYER_COL_GAP) + PLAYER_LABEL_W,
                        sec_players_y + 26 + row * (PLAYER_ROW_H + PLAYER_ROW_GAP),
                        col_w - PLAYER_LABEL_W, PLAYER_ROW_H);
        }
    }

    /* ⚠️ THE RECEIPT, in the settings stack's shape.  Everything hangs off
     * CONTENT_Y, which comes from a per-unit touch inset, so a button pushed
     * past the touchable rect looks perfect in a screenshot and is dead to a
     * finger.  The bottom is the last PLAYERS row; the right edge the last
     * button or cycler as placed in any row.  A label wider than its button
     * is cut, and both rows' labels are counted, and so is the CALIBRATED row's
     * longer value if it runs past the content edge.  A cycler cuts its own
     * entry text, so the PLAYERS rows add nothing to that count. */
    {
        const Cycler *last = &player_cyc[INPUT_SLOTS - 1];
        int bottom = last->y + last->height - CONTENT_Y;
        int right  = 0;
        for (int i = 0; i < 3; i++)
            if (test_btns[i]->x + test_btns[i]->width > right)
                right = test_btns[i]->x + test_btns[i]->width;
        if (last->x + last->width > right) right = last->x + last->width;
        int clipped = 0;
        char cut[24];
        if (fit_value(INPUT_CALIB_NO,
                      CONTENT_LEFT + (CONTENT_WIDTH < 600 ? 150 : 270),   /* draw_info_row()'s */
                      2, cut, sizeof(cut)))
            clipped++;
        for (int i = 0; i < 3; i++)
            if (!ui_label_fits(test_labels[i], test_scale, test_btns[i]->width))
                clipped++;
        for (int i = 0; i < TOUCH_SLOTS; i++) {
            if (!touch_btns[i]) continue;
            if (touch_btns[i]->x + touch_btns[i]->width > right)
                right = touch_btns[i]->x + touch_btns[i]->width;
            if (!ui_label_fits(touch_labels[i], touch_scale, touch_btns[i]->width))
                clipped++;
        }
        const char *verdict = bottom > CONTENT_H     ? "⚠ PAST CONTENT BOTTOM"
                            : right  > CONTENT_RIGHT ? "⚠ PAST CONTENT RIGHT"
                            : "fits";
        printf("control_panel: input stack %s — bottom +%d of CONTENT_H %d, "
               "right %d of CONTENT_RIGHT %d, %d label(s) cut, touch band %d px in %d row(s) "
               "at scale %d, testers in %d row(s) at scale %d, players in %d column(s) of %d px "
               "(safe %dx%d, %s)\n",
               verdict, bottom, CONTENT_H, right, CONTENT_RIGHT, clipped,
               touch_h, touch_rows, touch_scale, test_rows, test_scale,
               player_cols, player_cyc[0].width + PLAYER_LABEL_W,
               SCREEN_SAFE_WIDTH, SCREEN_SAFE_HEIGHT,
               CONTENT_WIDTH < 600 ? "portrait" : "landscape");
    }
}

/* ── Draw: the page ─────────────────────────────────────────────────────── */

/* Derived from the scan every time, never cached: a tester's button is
 * disabled exactly while no node of its kind is present.  Button.disabled
 * makes it grey and button_update() ignores it, so input() needs no guard. */
static void input_sync_disabled(void) {
    const InputPageState *s = &input_state;
    input_btn_ktest.disabled = s->kbd_idx < 0;
    input_btn_mtest.disabled = s->mou_idx < 0;
    input_btn_gtest.disabled = s->pad_idx < 0;
}

static void input_page_draw(Framebuffer *fb) {
    const InputPageState *s = &input_state;
    input_sync_disabled();
    draw_section_header(fb, sec_touch_y, "TOUCH");
    uint32_t calib_color;
    const char *calib = calib_row_value(&calib_color);
    draw_info_row(fb, sec_touch_y + 26, "TOUCH:", calib, calib_color);
    for (int i = 0; i < TOUCH_SLOTS; i++)
        if (touch_btns[i]) button_draw(fb, touch_btns[i]);
    draw_section_header(fb, sec_test_y, "KEYBOARD / MOUSE / PAD");
    for (int i = 0; i < 3; i++) {
        const Button *b = test_btns[i];
        button_draw(fb, test_btns[i]);
        char cnt[24];
        if (s->kind_cnt[i] > 0) snprintf(cnt, sizeof(cnt), "%d FOUND", s->kind_cnt[i]);
        else                    snprintf(cnt, sizeof(cnt), "NONE FOUND");
        fb_draw_text(fb, b->x + (b->width - text_measure_width(cnt, 1)) / 2, count_y[i],
                     cnt, s->kind_cnt[i] > 0 ? COLOR_LABEL : COLOR_DISABLED, 1);
    }

    draw_section_header(fb, sec_players_y, "PLAYERS");
    PlayerDevs d;
    player_devs(&d);
    for (int sl = 0; sl < INPUT_SLOTS; sl++) {
        Cycler *c = &player_cyc[sl];
        PlayerRow r;
        player_row(&d, sl, &r);
        c->disabled = r.count <= 1;      /* nothing to choose between */
        c->text_color = r.color;
        /* Scale 2 when the entry fits between the arrows (cycler_draw()'s
         * zone arithmetic), else 1 rather than a cut name. */
        int zone = c->height < (c->width >> 2) ? c->height : (c->width >> 2);
        c->text_scale = text_measure_width(r.text, 2) <= c->width - 2 * zone - 8 ? 2 : 1;
        char label[4];
        snprintf(label, sizeof(label), "P%d", sl + 1);
        fb_draw_text(fb, c->x - PLAYER_LABEL_W, c->y + (c->height - text_measure_height(2)) / 2,
                     label, COLOR_LABEL, 2);
        cycler_draw(fb, c, r.text);
        snprintf(player_shown[sl], sizeof(player_shown[sl]), "%s", r.text);
        player_shown_dis[sl] = c->disabled;
    }
}


/* ── Draw: hold to exit ─────────────────────────────────────────────── */
/* The keyboard and pad testers show every key, so a key leaves them only when
 * HELD (ui_hold_*, UI_HOLD_EXIT_MS); the mouse tester, which shows every
 * button, also takes LEFT+RIGHT held together (ui_chord_button()) on the same
 * timer and bar.  The static screen carries a one-line
 * hint under the title; while the key is held, this band — right of the BACK
 * button, down to just above the tester's info line, inside SCREEN_SAFE_* —
 * shows the hint and a bar filling over the hold, and is the ONLY part of the
 * screen repainted while it fills (fb_swap_rect()). */
#define HOLD_BAR_H 10
static UiRect hold_band(const Button *back) {
    UiRect r;
    r.x = back->x + back->width + 10;
    r.y = SCREEN_SAFE_TOP + 8;
    r.w = SCREEN_SAFE_RIGHT - 10 - r.x;
    r.h = 36;            /* ends at SAFE_TOP+43; the info line starts at +44 */
    return r;
}

static void draw_hold_hint(Framebuffer *fb, const char *hint) {
    text_draw_centered(fb, screen_base_width/2, SCREEN_SAFE_TOP+40, hint,
                       INPUT_COLOR_DIM, 1);
}

static void draw_hold_band(Framebuffer *fb, const Button *back, const char *hint,
                           int permille) {
    UiRect r = hold_band(back);
    if (r.w < 40) return;
    fb_fill_rect(fb, r.x, r.y, r.w, r.h, COLOR_BG);
    text_draw_centered(fb, r.x + r.w/2, r.y + 10, hint, COLOR_WHITE, 1);
    int bw = r.w - 40, bx = r.x + 20, by = r.y + r.h - HOLD_BAR_H - 6;
    fb_fill_rect(fb, bx, by, bw, HOLD_BAR_H, INPUT_COLOR_TRIG_BG);
    int fw = permille * bw / 1000;     /* library divide, not idiv */
    if (fw > 0) fb_fill_rect(fb, bx, by, fw, HOLD_BAR_H, INPUT_COLOR_TRIG_FILL);
    fb_draw_rect(fb, bx, by, bw, HOLD_BAR_H, INPUT_COLOR_PANEL_BD);
}

/* The filled width draw_hold_band() would paint: what decides a repaint. */
static int hold_fill_px(const Button *back, int permille) {
    UiRect r = hold_band(back);
    return r.w < 40 ? 0 : permille * (r.w - 40) / 1000;
}

#define KBD_HOLD_HINT "HOLD ESC TO EXIT"
#define PAD_HOLD_HINT "HOLD SELECT OR START TO EXIT"
#define MOU_HOLD_HINT "HOLD ESC OR LEFT+RIGHT TO EXIT"
#define MOU_HOLD_HINT_SHORT "HOLD ESC OR L+R TO EXIT"

/* The mouse hint, the long form wherever it fits both the band right of BACK
 * and the line under the title: 180 px at scale 1, against a band of
 * SCREEN_SAFE_WIDTH - 120 (under 360 only in portrait), so the short form is
 * for a unit with an unusually wide inset. */
static const char *mou_hold_hint(void) {
    UiRect r = hold_band(&input_btn_mback);
    int w = text_measure_width(MOU_HOLD_HINT, 1);
    return (w <= r.w - 8 && w <= SCREEN_SAFE_WIDTH - 8) ? MOU_HOLD_HINT
                                                        : MOU_HOLD_HINT_SHORT;
}

/* ── Draw: Keyboard fullscreen ──────────────────────────────────────── */
static void draw_kbd_test(Framebuffer *fb, InputPageState *s) {
    fb_clear(fb, COLOR_BG);
    button_draw(fb, &input_btn_kback);
    text_draw_centered(fb, screen_base_width/2, SCREEN_SAFE_TOP+22,
                       "KEYBOARD TEST", COLOR_WHITE, 3);
    draw_hold_hint(fb, KBD_HOLD_HINT);
    char inf[96];
    if (s->kbd.last_code>0 && s->last_dev>=0)
        snprintf(inf,sizeof(inf),"LAST KEY: %s (CODE %d) - EVENT%d", s->kbd.last_name,
                 s->kbd.last_code, s->devs[s->last_dev].ev_num);
    else if (s->kbd.last_code>0)
        snprintf(inf,sizeof(inf),"LAST KEY: %s (CODE %d)", s->kbd.last_name, s->kbd.last_code);
    else snprintf(inf,sizeof(inf),"PRESS ANY KEY ON ANY KEYBOARD");
    text_draw_centered(fb, screen_base_width/2, SCREEN_SAFE_TOP+52, inf, COLOR_CYAN, 2);

    int kx=SCREEN_SAFE_LEFT+20, ky=SCREEN_SAFE_TOP+72, hu=26, kh=32, g=2;
    for (int i=0; input_kblayout[i].code>=0; i++) {
        const LKey *k=&input_kblayout[i];
        int x=kx+k->col*hu, y=ky+k->row*(kh+g), w=k->w*hu-g;
        bool h=(k->code<KEY_MAX)?s->kbd.held[k->code]:false;
        fb_fill_rounded_rect(fb,x,y,w,kh,3,h?INPUT_COLOR_KEY_DN:INPUT_COLOR_KEY_UP);
        fb_draw_rounded_rect(fb,x,y,w,kh,3,INPUT_COLOR_KEY_BD);
        const char *l=input_key_sname(k->code);
        if (l) {
            int tw=text_measure_width(l,1);
            fb_draw_text(fb,x+(w-tw)/2,y+(kh-8)/2,l,h?COLOR_WHITE:INPUT_COLOR_KEY_TXT,1);
        }
    }
    int lx2=SCREEN_SAFE_LEFT+20, ly2=ky+5*(kh+g)+8;
    int lw2=SCREEN_SAFE_WIDTH-40, lh2=SCREEN_SAFE_BOTTOM-ly2-10;
    if (lh2<30) lh2=30;
    fb_fill_rounded_rect(fb,lx2,ly2,lw2,lh2,4,INPUT_COLOR_LOG_BG);
    fb_draw_rounded_rect(fb,lx2,ly2,lw2,lh2,4,INPUT_COLOR_PANEL_BD);
    fb_draw_text(fb,lx2+8,ly2+4,"EVENT LOG:",INPUT_COLOR_DIM,1);
    {
        int sx=lx2+8+text_measure_width("EVENT LOG:",1)+16;
        char src[256]; input_src_line(s, src, sizeof(src), lx2+lw2-8-sx, 1);
        fb_draw_text(fb,sx,ly2+4,src,COLOR_CYAN,1);
    }
    int lny=ly2+16, llh=13, ml=(lh2-20)/llh;
    if(ml>LOG_LINES) ml=LOG_LINES;
    if(ml<0) ml=0;
    int st=s->kbd.log_cnt-ml; if(st<0) st=0;
    for (int i=st; i<s->kbd.log_cnt; i++)
        fb_draw_text(fb,lx2+8,lny+(i-st)*llh,s->kbd.log[i],INPUT_COLOR_LOG_TXT,1);
}

/* ── Draw: Mouse fullscreen ─────────────────────────────────────────── */
static void draw_mou_test(Framebuffer *fb, InputPageState *s) {
    fb_clear(fb, COLOR_BG);
    button_draw(fb, &input_btn_mback);
    text_draw_centered(fb, screen_base_width/2, SCREEN_SAFE_TOP+22,
                       "MOUSE TEST", COLOR_WHITE, 3);
    draw_hold_hint(fb, mou_hold_hint());
    int sw=screen_base_width, sh=screen_base_height;
    int ax=SCREEN_SAFE_LEFT+20, ay=SCREEN_SAFE_TOP+55, aw=sw-240, ah=sh-ay-20;
    fb_fill_rounded_rect(fb,ax,ay,aw,ah,6,RGB(15,15,25));
    fb_draw_rounded_rect(fb,ax,ay,aw,ah,6,INPUT_COLOR_PANEL_BD);
    for(int gx=ax+50;gx<ax+aw;gx+=50) fb_draw_line(fb,gx,ay+1,gx,ay+ah-2,RGB(25,25,35));
    for(int gy=ay+50;gy<ay+ah;gy+=50) fb_draw_line(fb,ax+1,gy,ax+aw-2,gy,RGB(25,25,35));

    int cx=s->mou.cx, cy=s->mou.cy;
    int dx=cx<ax+2?ax+2:cx>=ax+aw-2?ax+aw-3:cx;
    int dy=cy<ay+2?ay+2:cy>=ay+ah-2?ay+ah-3:cy;
    fb_draw_line(fb,dx-15,dy,dx+15,dy,INPUT_COLOR_CURSOR_C);
    fb_draw_line(fb,dx,dy-15,dx,dy+15,INPUT_COLOR_CURSOR_C);
    fb_fill_circle(fb,dx,dy,3,INPUT_COLOR_CURSOR_C);

    int px=ax+aw+15, pw=sw-px-15, py=ay;
    fb_draw_text(fb,px,py,"POSITION",INPUT_COLOR_HDR,2);
    char ps[32];
    snprintf(ps,32,"X: %d",cx); fb_draw_text(fb,px,py+22,ps,COLOR_WHITE,2);
    snprintf(ps,32,"Y: %d",cy); fb_draw_text(fb,px,py+42,ps,COLOR_WHITE,2);

    int bsy=py+80;
    fb_draw_text(fb,px,bsy,"BUTTONS",INPUT_COLOR_HDR,2);
    struct { bool h; const char *l; int o; } mb[3]={{s->mou.bl,"L",0},{s->mou.bm,"M",45},{s->mou.br,"R",90}};
    for(int b=0;b<3;b++) {
        uint32_t c=mb[b].h?INPUT_COLOR_MBTN_ON:INPUT_COLOR_MBTN_OFF;
        int bx=px+20+mb[b].o, by2=bsy+40;
        fb_fill_circle(fb,bx,by2,14,c); fb_draw_circle(fb,bx,by2,14,INPUT_COLOR_PANEL_BD);
        fb_draw_text(fb,bx-4,by2+18,mb[b].l,COLOR_LABEL,2);
    }

    int ssy=bsy+95;
    fb_draw_text(fb,px,ssy,"SCROLL",INPUT_COLOR_HDR,2);
    snprintf(ps,32,"WHEEL: %d",s->mou.scroll);
    fb_draw_text(fb,px,ssy+22,ps,COLOR_WHITE,2);
    uint32_t now=get_time_ms();
    if (now-s->mou.scroll_t<300) fb_fill_circle(fb,px+pw-20,ssy+10,8,INPUT_COLOR_SCROLL);
    int brx=px, bry=ssy+45, brw=pw-10<20?20:pw-10, brh=20;
    fb_fill_rect(fb,brx,bry,brw,brh,RGB(40,40,40));
    int sv=s->mou.scroll; if(sv>50) sv=50; if(sv<-50) sv=-50;
    int ix=brx+brw/2+(sv*brw/100);
    fb_fill_rect(fb,ix-3,bry,6,brh,INPUT_COLOR_SCROLL);
    fb_draw_line(fb,brx+brw/2,bry,brx+brw/2,bry+brh,INPUT_COLOR_DIM);

    /* Which node moved the crosshair: every mouse node is open at once. */
    int sry=bry+brh+20;
    fb_draw_text(fb,px,sry,"SOURCE",INPUT_COLOR_HDR,2);
    if (s->last_dev>=0 && s->last_dev<s->dev_cnt) {
        const InputDev *d=&s->devs[s->last_dev];
        snprintf(ps,32,"EVENT%d",d->ev_num);
        fb_draw_text(fb,px,sry+22,ps,COLOR_WHITE,2);
        char nm[256]; text_truncate(nm, sizeof(nm), d->name, pw-5, 1);
        fb_draw_text(fb,px,sry+44,nm,COLOR_LABEL,1);
    } else {
        fb_draw_text(fb,px,sry+22,"NONE YET",INPUT_COLOR_DIM,2);
    }
    snprintf(ps,32,"%d MOUSE NODE%s OPEN",s->fd_cnt,s->fd_cnt==1?"":"S");
    fb_draw_text(fb,px,sry+58,ps,INPUT_COLOR_DIM,1);
    text_draw_centered(fb,sw/2,sh-15,"MOVE ANY MOUSE TO CONTROL CROSSHAIR",INPUT_COLOR_DIM,1);
}

/* ── Draw: Gamepad helpers ──────────────────────────────────────────── */
static void input_draw_stick(Framebuffer *fb, int cx, int cy, int r,
                           int vx, int vy, const char *label) {
    fb_fill_circle(fb,cx,cy,r,INPUT_COLOR_STICK_BG);
    fb_draw_circle(fb,cx,cy,r,INPUT_COLOR_PANEL_BD);
    fb_draw_line(fb,cx-r,cy,cx+r,cy,RGB(40,40,55));
    fb_draw_line(fb,cx,cy-r,cx,cy+r,RGB(40,40,55));
    int dpx=cx+(vx*(r-6))/1000, dpy=cy+(vy*(r-6))/1000;
    fb_fill_circle(fb,dpx,dpy,6,INPUT_COLOR_STICK_DOT);
    fb_draw_circle(fb,dpx,dpy,6,COLOR_WHITE);
    text_draw_centered(fb,cx,cy+r+14,label,COLOR_LABEL,2);
}

static void input_draw_dpad(Framebuffer *fb, int cx, int cy, int sz, int dx, int dy) {
    int arm=sz/3, hf=arm/2;
    fb_fill_rect(fb,cx-sz/2,cy-hf,sz,arm,RGB(50,50,60));
    fb_fill_rect(fb,cx-hf,cy-sz/2,arm,sz,RGB(50,50,60));
    if(dx<0) fb_fill_rect(fb,cx-sz/2,cy-hf,arm,arm,INPUT_COLOR_PAD_ON);
    if(dx>0) fb_fill_rect(fb,cx+sz/2-arm,cy-hf,arm,arm,INPUT_COLOR_PAD_ON);
    if(dy<0) fb_fill_rect(fb,cx-hf,cy-sz/2,arm,arm,INPUT_COLOR_PAD_ON);
    if(dy>0) fb_fill_rect(fb,cx-hf,cy+sz/2-arm,arm,arm,INPUT_COLOR_PAD_ON);
    fb_draw_rect(fb,cx-sz/2,cy-hf,sz,arm,INPUT_COLOR_PANEL_BD);
    fb_draw_rect(fb,cx-hf,cy-sz/2,arm,sz,INPUT_COLOR_PANEL_BD);
    text_draw_centered(fb,cx,cy+sz/2+14,"D-PAD",COLOR_LABEL,2);
}

/* ── Draw: Gamepad fullscreen ───────────────────────────────────────── */
static void draw_pad_test(Framebuffer *fb, InputPageState *s) {
    fb_clear(fb, COLOR_BG);
    button_draw(fb, &input_btn_gback);
    text_draw_centered(fb, screen_base_width/2, SCREEN_SAFE_TOP+22,
                       "GAMEPAD TEST", COLOR_WHITE, 3);
    draw_hold_hint(fb, PAD_HOLD_HINT);
    int sw=screen_base_width, sr=55;
    int lcx=SCREEN_SAFE_LEFT+30+sr, lcy=SCREEN_SAFE_TOP+80+sr;
    input_draw_stick(fb,lcx,lcy,sr,s->pad.lx,s->pad.ly,"LEFT STICK");
    input_draw_dpad(fb,lcx,lcy+sr+70,70,s->pad.dx,s->pad.dy);
    int rcx=sw-SCREEN_SAFE_LEFT-30-sr;
    input_draw_stick(fb,rcx,lcy,sr,s->pad.rx,s->pad.ry,"RIGHT STICK");

    int bcnt=s->pad.btn_cnt<1?8:s->pad.btn_cnt;
    if(bcnt>16) bcnt=16;
    int cols=4, bsz=28, bgap=6;
    int bax=lcx+sr+40, bay=SCREEN_SAFE_TOP+75;
    for(int i=0;i<bcnt;i++) {
        int col=i%cols, row=i/cols;
        int bx=bax+col*(bsz+bgap)+bsz/2;
        int by=bay+row*(bsz+bgap)+bsz/2;
        uint32_t c=s->pad.btns[i]?INPUT_COLOR_PAD_ON:INPUT_COLOR_PAD_OFF;
        fb_fill_circle(fb,bx,by,bsz/2,c);
        fb_draw_circle(fb,bx,by,bsz/2,INPUT_COLOR_PANEL_BD);
        char bn[4]; snprintf(bn,4,"%d",i);
        int tw=text_measure_width(bn,1);
        fb_draw_text(fb,bx-tw/2,by-4,bn,COLOR_WHITE,1);
    }
    fb_draw_text(fb,bax,bay-16,"BUTTONS",INPUT_COLOR_HDR,2);

    int trw=150, trh=18, try2=SCREEN_SAFE_BOTTOM-80;
    int trlx=(sw/2)-trw-20, trrx=(sw/2)+20;
    fb_draw_text(fb,trlx,try2-16,"LT",COLOR_LABEL,1);
    fb_fill_rect(fb,trlx,try2,trw,trh,INPUT_COLOR_TRIG_BG);
    if(s->pad.tl>0) fb_fill_rect(fb,trlx,try2,(s->pad.tl*trw)/1000,trh,INPUT_COLOR_TRIG_FILL);
    fb_draw_rect(fb,trlx,try2,trw,trh,INPUT_COLOR_PANEL_BD);
    char tv[16]; snprintf(tv,16,"%d%%",s->pad.tl/10);
    fb_draw_text(fb,trlx+trw+8,try2+4,tv,COLOR_WHITE,1);

    fb_draw_text(fb,trrx,try2-16,"RT",COLOR_LABEL,1);
    fb_fill_rect(fb,trrx,try2,trw,trh,INPUT_COLOR_TRIG_BG);
    if(s->pad.tr>0) fb_fill_rect(fb,trrx,try2,(s->pad.tr*trw)/1000,trh,INPUT_COLOR_TRIG_FILL);
    fb_draw_rect(fb,trrx,try2,trw,trh,INPUT_COLOR_PANEL_BD);
    snprintf(tv,16,"%d%%",s->pad.tr/10);
    fb_draw_text(fb,trrx+trw+8,try2+4,tv,COLOR_WHITE,1);

    int rvx=bax, rvy=bay+(bcnt/cols+1)*(bsz+bgap)+20;
    fb_draw_text(fb,rvx,rvy,"RAW AXES",INPUT_COLOR_HDR,2);
    char rv[48];
    snprintf(rv,48,"LX:%+5d LY:%+5d",s->pad.lx,s->pad.ly);
    fb_draw_text(fb,rvx,rvy+20,rv,COLOR_LABEL,1);
    snprintf(rv,48,"RX:%+5d RY:%+5d",s->pad.rx,s->pad.ry);
    fb_draw_text(fb,rvx,rvy+34,rv,COLOR_LABEL,1);
    snprintf(rv,48,"DX:%+2d DY:%+2d",s->pad.dx,s->pad.dy);
    fb_draw_text(fb,rvx,rvy+48,rv,COLOR_LABEL,1);

    /* Every pad node is open at once; name the one that sent the last event. */
    char src[256]; input_src_line(s, src, sizeof(src), SCREEN_SAFE_WIDTH-20, 2);
    text_draw_centered(fb,sw/2,SCREEN_SAFE_BOTTOM-30,src,COLOR_CYAN,2);
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static void input_page_load(const Config *cfg) {
    (void)cfg;
    InputPageState *s = &input_state;
    s->scr = INPUT_SCR_MAIN;
    s->fd_cnt = 0;
    s->last_dev = -1;
    s->node_sig = input_node_sig();
    input_scan_devices(s);
}

/* A device plugged in or pulled while the page was closed shows on opening:
 * the next input() lists /dev/input at once and classifies afresh. */
static void input_page_enter(void) {
    input_state.poll_ms = 0;
    input_state.node_sig = ~0UL;
}

/* RESET GEOMETRY's OK: the reset, its status line and the re-layout are all
 * control_panel.c's. */
static void input_reset_geometry_confirmed(Config *cfg) {
    (void)cfg;
    cp_reset_touch_geometry();
}

/* Main screen only: a tester is queued here and run by
 * input_page_run_fullscreen().  The hot-plug poll repaints only when the count
 * of some kind changed — a node the testers would not open changes nothing. */
static CpPageResult input_page_input(Config *cfg, int tx, int ty,
                                     bool touching, uint32_t now) {
    InputPageState *state = &input_state;
    CpPageResult act = CP_PAGE_IDLE;

    if (state->poll_ms == 0 || now - state->poll_ms >= POLL_MS) {
        state->poll_ms = now;
        unsigned long sig = input_node_sig();
        if (sig != state->node_sig) {
            int prev[3];
            memcpy(prev, state->kind_cnt, sizeof(prev));
            state->node_sig = sig;
            input_scan_devices(state);
            if (memcmp(prev, state->kind_cnt, sizeof(prev)) != 0)
                act = CP_PAGE_REDRAW;
        }
    }

    input_sync_disabled();
    if (button_update(&input_btn_calibrate, tx, ty, touching, now)) {
        state->scr = INPUT_SCR_CALIBRATE;
        act = CP_PAGE_FULLSCREEN;
    }
    if (button_update(&input_btn_touch_diag, tx, ty, touching, now)) {
        state->scr = INPUT_SCR_TOUCH_DIAG;
        act = CP_PAGE_FULLSCREEN;
    }
    if (button_update(&input_btn_reset_geom, tx, ty, touching, now))
        cp_confirm("RESET SCREEN GEOMETRY?",
                   "TOUCH RANGE AND EDGES\nGO BACK TO DEFAULTS.", "RESET",
                   input_reset_geometry_confirmed);
    if (button_update(&input_btn_multitouch, tx, ty, touching, now)) {
        state->scr = INPUT_SCR_MULTITOUCH;
        act = CP_PAGE_FULLSCREEN;
    }
    if (button_update(&input_btn_ktest, tx, ty, touching, now)) {
        memset(&state->kbd, 0, sizeof(state->kbd));
        if (input_open_kind(state, DEV_KEYBOARD) > 0) {
            state->scr = INPUT_SCR_KEYBOARD;
            act = CP_PAGE_FULLSCREEN;
        }
    }
    if (button_update(&input_btn_mtest, tx, ty, touching, now)) {
        memset(&state->mou, 0, sizeof(state->mou));
        if (input_open_kind(state, DEV_MOUSE) > 0) {
            input_open_exit_keys(state);
            state->scr = INPUT_SCR_MOUSE;
            act = CP_PAGE_FULLSCREEN;
        }
    }
    if (button_update(&input_btn_gtest, tx, ty, touching, now)) {
        memset(&state->pad, 0, sizeof(state->pad));
        state->pad.btn_cnt = 8;
        if (input_open_kind(state, DEV_GAMEPAD) > 0) {
            for (int k = 0; k < state->fd_cnt; k++)
                input_load_axes(state, k, state->fds[k]);
            state->scr = INPUT_SCR_GAMEPAD;
            act = CP_PAGE_FULLSCREEN;
        }
    }

    /* PLAYERS: fresh from the manager every frame — a pad that came or went
     * in the main loop's rescan repaints the rows that show it. */
    {
        PlayerDevs d;
        player_devs(&d);
        for (int sl = 0; sl < INPUT_SLOTS; sl++) {
            PlayerRow r;
            player_row(&d, sl, &r);
            player_cyc[sl].disabled = r.count <= 1;
            if (player_cyc[sl].disabled != player_shown_dis[sl] ||
                strcmp(r.text, player_shown[sl]) != 0)
                if (act == CP_PAGE_IDLE) act = CP_PAGE_REDRAW;
        }
        for (int sl = 0; sl < INPUT_SLOTS; sl++) {
            int dir = cycler_check_tap(&player_cyc[sl], tx, ty, touching);
            if (dir && player_step(cfg, sl, dir) && act == CP_PAGE_IDLE)
                act = CP_PAGE_REDRAW;
        }
    }
    return act;
}

/* ── Full-screen: the multi-touch test ──────────────────────────────────── */

/* The band along the top edge whose right-hand 100 px is the exit tap. */
#define MT_EXIT_H 36

/* Multi-touch: one dot per MT slot, read straight off the evdev fd because
 * TouchInput tracks a single pointer. Slots arrive only from a driver that
 * reports ABS_MT_SLOT; the legacy BTN_TOUCH still drives the exit tap.
 *
 * The primary contact (slot 0, or the single-touch ABS pair when the driver
 * sends no slots) also gets the calibration trace: a yellow trail of
 * calibrated points on a dim 80 px grid, RAW / CAL / LIN readouts, and a red
 * crosshair at LIN, the pure per-axis linear estimate from the raw range that
 * ignores calibration, so what the curve changed is visible.  Every new
 * sample goes to MT_LOG_PATH as "raw_x raw_y cal_x cal_y est_x est_y". */
#define MT_SLOTS 2
#define MT_LOG_PATH "/tmp/touch_trace.log"
#define MT_GRID_STEP 80
#define MT_TRAIL_MAX 8000
typedef struct { short x, y; } MtPt;
static MtPt mt_trail[MT_TRAIL_MAX];

static void test_multitouch(Framebuffer *fb, TouchInput *touch) {
    static const uint32_t slot_col[MT_SLOTS] = { RGB(255,200,0), RGB(0,200,255) };
    int calib_ok = (touch_load_calibration(touch, CALIB_FILE) == 0);
    if (calib_ok) touch_enable_calibration(touch, true);
    int rx[MT_SLOTS] = {0}, ry[MT_SLOTS] = {0};
    bool on[MT_SLOTS] = {false};
    int slot = 0, lx = 0, ly = 0, max_fingers = 0;
    bool seen_mt = false, running = true, held = false;
    InputSynDrop sd = {0};

    const int W = fb->width, H = fb->height;
    int rgx = touch->raw_max_x - touch->raw_min_x; if (rgx <= 0) rgx = 4095;
    int rgy = touch->raw_max_y - touch->raw_min_y; if (rgy <= 0) rgy = 4095;
    int trail_n = 0, trail_head = 0;
    int last_raw_x = -1, last_raw_y = -1, last_cal_x = -1, last_cal_y = -1;
    int last_est_x = -1, last_est_y = -1;
    bool have_sample = false;

    FILE *log = fopen(MT_LOG_PATH, "w");
    if (log) {
        fprintf(log, "# touch_trace  screen=%dx%d  portrait=%d\n",
                W, H, touch->portrait_mode);
        fprintf(log, "# raw_range X[%d..%d] Y[%d..%d]\n",
                touch->raw_min_x, touch->raw_max_x,
                touch->raw_min_y, touch->raw_max_y);
        fprintf(log, "# calib_enabled=%d raw_range X[%d..%d] Y[%d..%d]\n",
                touch->calib.enabled,
                touch->raw_min_x, touch->raw_max_x,
                touch->raw_min_y, touch->raw_max_y);
        fprintf(log, "# bezel(UI) T=%d B=%d L=%d R=%d\n",
                touch->calib.bezel_top, touch->calib.bezel_bottom,
                touch->calib.bezel_left, touch->calib.bezel_right);
        fprintf(log, "# columns: raw_x raw_y cal_x cal_y est_x est_y\n");
        fflush(log);
    }

    /* Paints only after the touch fd delivered something: every line of this
     * screen is a function of what was read, so a quiet panel repaints
     * nothing (it used to clear + swap every 16 ms regardless).  The poll()
     * below is the loop's wait, so an idle screen sleeps in the kernel. */
    bool dirty = true;
    touch_drain_events(touch);
    while (running) {
        if (dirty) {
            dirty = false;
            /* Counted before the header, which prints the maximum — counted in
             * the dot loop it lagged a frame, and with no next frame it stayed
             * stale. */
            int fingers = 0;
            for (int i = 0; i < MT_SLOTS; i++) if (on[i]) fingers++;
            if (fingers > max_fingers) max_fingers = fingers;

            /* The primary contact: record a sample only when it changed. */
            bool prim = seen_mt ? on[0] : held;
            if (prim) {
                int raw_x = seen_mt ? rx[0] : lx, raw_y = seen_mt ? ry[0] : ly;
                int cal_x = raw_x, cal_y = raw_y;
                touch_map_raw(touch, &cal_x, &cal_y);
                int est_x = (raw_x - touch->raw_min_x) * W / rgx;
                int est_y = (raw_y - touch->raw_min_y) * H / rgy;
                if (raw_x != last_raw_x || raw_y != last_raw_y ||
                    cal_x != last_cal_x || cal_y != last_cal_y) {
                    mt_trail[trail_head].x = (short)cal_x;
                    mt_trail[trail_head].y = (short)cal_y;
                    trail_head = (trail_head + 1) % MT_TRAIL_MAX;
                    if (trail_n < MT_TRAIL_MAX) trail_n++;
                    if (log) {
                        fprintf(log, "%d %d %d %d %d %d\n",
                                raw_x, raw_y, cal_x, cal_y, est_x, est_y);
                        fflush(log);
                    }
                    last_raw_x = raw_x; last_raw_y = raw_y;
                    last_cal_x = cal_x; last_cal_y = cal_y;
                }
                last_est_x = est_x; last_est_y = est_y;
                have_sample = true;
            }

            fb_clear(fb, RGB(20,20,30));
            /* Grid and its labels (dim): seen, not pressed, so the whole
             * visible screen.  The x labels sit on the bottom edge and the
             * 0 row is unlabelled, clear of the header text. */
            const uint32_t grid = RGB(45,45,60), glab = RGB(90,90,110);
            for (int x = 0; x <= W; x += MT_GRID_STEP)
                fb_draw_line(fb, x, 0, x, H - 1, grid);
            for (int y = 0; y <= H; y += MT_GRID_STEP)
                fb_draw_line(fb, 0, y, W - 1, y, grid);
            for (int x = 0; x <= W; x += MT_GRID_STEP) {
                char b[12]; snprintf(b, sizeof(b), "%d", x);
                fb_draw_text(fb, x + 2, H - 10, b, glab, 1);
            }
            for (int y = MT_GRID_STEP; y <= H; y += MT_GRID_STEP) {
                char b[12]; snprintf(b, sizeof(b), "%d", y);
                fb_draw_text(fb, 2, y + 2, b, glab, 1);
            }
            for (int i = 0; i < trail_n; i++)
                fb_fill_circle(fb, mt_trail[i].x, mt_trail[i].y, 2, COLOR_YELLOW);

            char hdr[96]; snprintf(hdr, sizeof(hdr),
                "Multi-touch  |  MT slots: %s  |  max fingers: %d  |  Calib: %s",
                seen_mt ? "yes" : "none yet", max_fingers, calib_ok ? "ON" : "OFF");
            fb_draw_text(fb, 4, 2, hdr, COLOR_WHITE, 1);
            fb_draw_text(fb, fb->width - 160, 2, "[EXIT: top-right]", RGB(180,80,80), 1);
            for (int i = 0; i < MT_SLOTS; i++) {
                if (!on[i]) continue;
                int x = rx[i], y = ry[i];
                touch_map_raw(touch, &x, &y);
                fb_fill_circle(fb, x, y, 28, slot_col[i]);
                char lbl[48]; snprintf(lbl, sizeof(lbl), "slot %d raw(%d,%d) scr(%d,%d)",
                                       i, rx[i], ry[i], x, y);
                fb_draw_text(fb, 4, 16 + 12 * i, lbl, slot_col[i], 1);
            }
            if (prim) {
                /* the red LIN crosshair beside the calibrated dot */
                for (int i = -10; i <= 10; i++) {
                    fb_draw_pixel(fb, last_est_x + i, last_est_y, COLOR_RED);
                    fb_draw_pixel(fb, last_est_x, last_est_y + i, COLOR_RED);
                }
            }
            if (have_sample) {
                char rb[64];
                snprintf(rb, sizeof(rb), "RAW %4d,%4d", last_raw_x, last_raw_y);
                fb_draw_text(fb, W / 2 - 150, 48, rb, COLOR_WHITE, 2);
                snprintf(rb, sizeof(rb), "CAL %4d,%4d", last_cal_x, last_cal_y);
                fb_draw_text(fb, W / 2 - 150, 72, rb, COLOR_CYAN, 2);
                snprintf(rb, sizeof(rb), "LIN %4d,%4d", last_est_x, last_est_y);
                fb_draw_text(fb, W / 2 + 20, 72, rb, COLOR_RED, 2);
                snprintf(rb, sizeof(rb), "samples:%d", trail_n);
                fb_draw_text(fb, W / 2 - 150, 96, rb, COLOR_GRAY, 1);
            }
            fb_swap(fb);
        }

        struct pollfd pfd = { .fd = touch->fd, .events = POLLIN };
        if (poll(&pfd, 1, 16) <= 0) continue;
        struct input_event ev;
        while (read(touch->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            dirty = true;
            if (input_syn_drop_skip(&sd, &ev)) continue;
            if (ev.type == EV_ABS) {
                switch (ev.code) {
                case ABS_MT_SLOT: slot = ev.value; seen_mt = true; break;
                case ABS_MT_TRACKING_ID:
                    if (slot >= 0 && slot < MT_SLOTS) on[slot] = ev.value >= 0;
                    break;
                case ABS_MT_POSITION_X: if (slot >= 0 && slot < MT_SLOTS) rx[slot] = ev.value; break;
                case ABS_MT_POSITION_Y: if (slot >= 0 && slot < MT_SLOTS) ry[slot] = ev.value; break;
                case ABS_X: lx = ev.value; break;
                case ABS_Y: ly = ev.value; break;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                held = ev.value != 0;
                if (ev.value == 1) {
                    int x = lx, y = ly;
                    touch_map_raw(touch, &x, &y);
                    if (x > (int)fb->width - 100 && y < MT_EXIT_H) running = false;
                }
            }
            if (poll(&pfd, 1, 0) <= 0) break;
        }

        /* After a SYN_DROPPED the kernel discarded unread events, so a lift
         * among them is gone and the slot would stay drawn as held.  Its own
         * per-slot state survives: re-read every slot (tracking id < 0 is up),
         * the current slot and the single-touch levels.  Waits for the torn
         * packet's SYN_REPORT, which may arrive in a later read. */
        if (sd.resync && !sd.dropping) {
            sd.resync = false;
            dirty = true;
            struct { __u32 code; __s32 v[MT_SLOTS]; } rq;
            bool ok = true;
            rq.code = ABS_MT_TRACKING_ID;
            if (ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(rq)), &rq) == 0) {
                for (int i = 0; i < MT_SLOTS; i++) on[i] = rq.v[i] >= 0;
            } else {
                ok = false;
                for (int i = 0; i < MT_SLOTS; i++) on[i] = false;   /* unknown: up */
            }
            if (ok) {
                rq.code = ABS_MT_POSITION_X;
                if (ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(rq)), &rq) == 0)
                    for (int i = 0; i < MT_SLOTS; i++) rx[i] = rq.v[i];
                rq.code = ABS_MT_POSITION_Y;
                if (ioctl(touch->fd, EVIOCGMTSLOTS(sizeof(rq)), &rq) == 0)
                    for (int i = 0; i < MT_SLOTS; i++) ry[i] = rq.v[i];
            }
            struct input_absinfo ai;
            if (ioctl(touch->fd, EVIOCGABS(ABS_MT_SLOT), &ai) == 0) slot = ai.value;
            if (ioctl(touch->fd, EVIOCGABS(ABS_X), &ai) == 0) lx = ai.value;
            if (ioctl(touch->fd, EVIOCGABS(ABS_Y), &ai) == 0) ly = ai.value;
            unsigned char keys[(BTN_TOUCH / 8) + 1];
            memset(keys, 0, sizeof(keys));
            if (ioctl(touch->fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
                held = (keys[BTN_TOUCH / 8] >> (BTN_TOUCH % 8)) & 1;
        }
    }
    if (log) fclose(log);
    touch_drain_events(touch);
    touch_enable_calibration(touch, false);
}

/* ── Full-screen: the testers ───────────────────────────────────────────── */

/* After input() returned CP_PAGE_FULLSCREEN.  Runs until the tester's BACK, a
 * held exit key (Esc; a pad's Select or Start) or, in the mouse tester, LEFT
 * and RIGHT held together, for UI_HOLD_EXIT_MS (or
 * a signal asking the panel to quit), then closes every node it opened: its
 * hardware is its own to clean up. */
static void input_page_run_fullscreen(Framebuffer *fb, TouchInput *touch) {
    InputPageState *state = &input_state;
    if (state->scr == INPUT_SCR_MULTITOUCH) {   /* its own loop and exit tap */
        test_multitouch(fb, touch);
        state->scr = INPUT_SCR_MAIN;
        return;
    }
    if (state->scr == INPUT_SCR_CALIBRATE || state->scr == INPUT_SCR_TOUCH_DIAG) {
        int mode = state->scr == INPUT_SCR_CALIBRATE ? CP_TOUCH_CALIBRATE
                                                     : CP_TOUCH_DIAGNOSTIC;
        state->scr = INPUT_SCR_MAIN;
        cp_run_touch_tool(fb, touch, mode);   /* re-lays this page out too */
        return;
    }
    if (state->scr == INPUT_SCR_MOUSE) {
        state->mou.cx = (int)fb->width / 2;
        state->mou.cy = (int)fb->height / 2;
    }
    /* ⚠️ Paint only on change — the dirty-flag rule (../CLAUDE.md →
     * Rendering).  A full clear + fb_swap every frame, with nothing new to
     * show, held this tester at ~45 % CPU sitting idle.  The terms: the first
     * frame; any event the tester applied (input_proc_*'s count); a touch
     * edge; a button look change (button_take_dirty(), as control_panel.c's
     * main loop); and the timed element, the mouse tester's scroll dot,
     * which goes out SCROLL_LIT_MS after the last wheel event with no event to
     * say so.  The hold-to-exit bar also fills on the clock, but repaints only
     * its own band, never this frame.  Nothing else moves on a clock: an unplugged
     * device just stops sending, and the screen it leaves is already right. */
    enum { SCROLL_LIT_MS = 300 };          /* draw_mou_test()'s window */
    bool dirty = true;
    bool scroll_lit = false;               /* as last painted */
    int hold_px = -1;                      /* exit bar's fill as last painted, -1 none */
    memset(&state->hold, 0, sizeof(state->hold));
    memset(&state->chord, 0, sizeof(state->chord));
    while (cp_running() && state->scr != INPUT_SCR_MAIN) {
        uint32_t now = get_time_ms();

        int applied = 0;
        switch (state->scr) {
            case INPUT_SCR_KEYBOARD: applied = input_proc_kbd(state); break;
            case INPUT_SCR_MOUSE:    applied = input_proc_mouse(fb, state); break;
            case INPUT_SCR_GAMEPAD:  applied = input_proc_pad(state); break;
            default: break;
        }
        if (applied > 0) dirty = true;
        if (scroll_lit && now - state->mou.scroll_t >= SCROLL_LIT_MS)
            dirty = true;                  /* the dot has to go out */

        /* Hold to exit: every tester.  The held key's press edge is never
         * seen as BACK after this — run_current_fullscreen_mode() drains the
         * pad layer, which then holds it as already down.  The mouse tester
         * has two triggers on one timer: an exit key, or LEFT+RIGHT. */
        if (state->scr == INPUT_SCR_MOUSE) input_proc_exit_keys(state);
        const Button *hold_back = state->scr == INPUT_SCR_KEYBOARD ? &input_btn_kback
                                : state->scr == INPUT_SCR_GAMEPAD  ? &input_btn_gback
                                : state->scr == INPUT_SCR_MOUSE    ? &input_btn_mback
                                : NULL;
        UiHold hold = state->scr == INPUT_SCR_MOUSE
                    ? ui_hold_either(&state->hold, &state->chord.hold)
                    : state->hold;
        int hold_pm = 0;
        if (hold_back) {
            bool hold_exit = false;
            hold_pm = ui_hold_progress(hold.down, get_time_ms(),
                                       hold.start_ms, UI_HOLD_EXIT_MS,
                                       &hold_exit);
            if (hold_exit) {
                input_close(state); state->scr = INPUT_SCR_MAIN;
                break;
            }
        }
        const char *hold_hint = state->scr == INPUT_SCR_KEYBOARD ? KBD_HOLD_HINT
                              : state->scr == INPUT_SCR_MOUSE    ? mou_hold_hint()
                                                                 : PAD_HOLD_HINT;

        /* A hold that ended with no event this tester shows — the mouse
         * tester's exit keys are read unshown — still has to take its bar
         * down. */
        if (hold_back && !hold.down && hold_px >= 0) dirty = true;

        if (!dirty && hold_back && hold.down &&
            hold_fill_px(hold_back, hold_pm) != hold_px) {
            /* The bar alone moved: repaint and present only its band. */
            draw_hold_band(fb, hold_back, hold_hint, hold_pm);
            UiRect b = hold_band(hold_back);
            fb_swap_rect(fb, b.x, b.y, b.w, b.h);
            hold_px = hold_fill_px(hold_back, hold_pm);
        }

        if (dirty) {
            /* Sampled before the draw, which reads the clock again later: a
             * dot the draw lit is then always one this records as lit. */
            uint32_t t_draw = get_time_ms();
            switch (state->scr) {
                case INPUT_SCR_KEYBOARD: draw_kbd_test(fb, state); break;
                case INPUT_SCR_MOUSE:    draw_mou_test(fb, state); break;
                case INPUT_SCR_GAMEPAD:  draw_pad_test(fb, state); break;
                default: break;
            }
            hold_px = -1;
            if (hold_back && hold.down) {   /* a full frame mid-hold keeps the bar */
                draw_hold_band(fb, hold_back, hold_hint, hold_pm);
                hold_px = hold_fill_px(hold_back, hold_pm);
            }
            fb_swap(fb);
            dirty = false;
            scroll_lit = state->scr == INPUT_SCR_MOUSE &&
                         t_draw - state->mou.scroll_t < SCROLL_LIT_MS;
        }

        touch_poll(touch);
        TouchState ts = touch_get_state(touch);
        int tx = ts.x, ty = ts.y;
        bool touching = ts.pressed || ts.held;

        switch (state->scr) {
        case INPUT_SCR_KEYBOARD:
            if (button_update(&input_btn_kback, tx, ty, touching, now)) {
                input_close(state); state->scr = INPUT_SCR_MAIN;
            }
            break;
        case INPUT_SCR_MOUSE:
            if (button_update(&input_btn_mback, tx, ty, touching, now)) {
                input_close(state); state->scr = INPUT_SCR_MAIN;
            }
            break;
        case INPUT_SCR_GAMEPAD:
            if (button_update(&input_btn_gback, tx, ty, touching, now)) {
                input_close(state); state->scr = INPUT_SCR_MAIN;
            }
            break;
        default: break;
        }

        /* Read into a local so the || cannot short-circuit past the clear. */
        bool btn_look = button_take_dirty();
        if (ts.pressed || ts.released || btn_look) dirty = true;

        usleep(FRAME_DELAY_ACTIVE_US);
    }
    input_close(state);   /* a signal can end the loop inside a tester */
    state->scr = INPUT_SCR_MAIN;
}

/* Keyboard focus: the touch-tool row, the tester row and the PLAYERS rows, as
 * input() hit-tests them; a tester with no device of its kind, and a player
 * row with nothing to choose, is disabled and skipped. */
static int input_page_focusables(UiRect *out, int max) {
    int n = 0;
    for (int i = 0; i < TOUCH_SLOTS; i++)
        if (touch_btns[i]) n = focus_add_button(out, n, max, touch_btns[i]);
    for (int i = 0; i < 3; i++)
        n = focus_add_button(out, n, max, test_btns[i]);
    for (int sl = 0; sl < INPUT_SLOTS; sl++)
        n = focus_add_cycler(out, n, max, &player_cyc[sl]);
    return n;
}

/* LEFT/RIGHT on a PLAYERS row steps it.  idx is resolved against the list
 * focusables() writes now, so a disabled widget skipped earlier in it cannot
 * shift which row it names. */
/* RESET DEFAULTS has already cleared slot_p1..slot_p4 from cfg; the manager's
 * live pins follow, or the page would keep showing pins the file lost. */
static void input_page_reset_defaults(Config *cfg) {
    for (int sl = 0; sl < INPUT_SLOTS; sl++)
        gamepad_slot_unpin(cp_gamepad(), sl, cfg);
}

#define INPUT_FOCUS_MAX (TOUCH_SLOTS + 3 + INPUT_SLOTS)
static bool input_page_focus_nudge(Config *cfg, int idx, int dir) {
    UiRect r[INPUT_FOCUS_MAX];
    int n = input_page_focusables(r, INPUT_FOCUS_MAX);
    if (idx < 0 || idx >= n) return false;
    for (int sl = 0; sl < INPUT_SLOTS; sl++) {
        if (player_cyc[sl].disabled) continue;
        UiRect c = cycler_rect(&player_cyc[sl]);
        if (c.x == r[idx].x && c.y == r[idx].y && c.w == r[idx].w && c.h == r[idx].h) {
            player_step(cfg, sl, dir);
            return true;
        }
    }
    return false;
}

const CpPage cp_input_page = {
    .name           = "Input",
    .icon           = "cp_input",
    .load           = input_page_load,
    .layout         = input_page_layout,
    .enter          = input_page_enter,
    .draw           = input_page_draw,
    .input          = input_page_input,
    .focusables     = input_page_focusables,
    .focus_nudge    = input_page_focus_nudge,
    .reset_defaults = input_page_reset_defaults,
    .run_fullscreen = input_page_run_fullscreen,
};
