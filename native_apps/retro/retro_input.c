/* retro input: USB keyboards and pads through input_scan, read as raw evdev.
 * Pads -> RETRO_DEVICE_JOYPAD/ANALOG ports 0..1 (SNES layout: Xbox A = B,
 * B = A, X = Y, Y = X).  Keyboard -> RETRO_DEVICE_KEYBOARD and the keyboard
 * callback; for a core that never asks for the keyboard it doubles as pad 0.
 * Mice (and a BT keyboard's touchpad) -> RETRO_DEVICE_MOUSE port 0: summed deltas, L/R/M, wheel.
 * Pause menu: pad Select+Start, or F12 (Esc too, unless the core is a computer with a keyboard);
 * while it is up, keys and pad edges become menu events (in_menu_ev) and the core sees nothing. */
#include "retro.h"
#include "input_scan.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAXN 8
static InputNode nodes[MAXN]; static int nn;
static int pad_of[MAXN];                 /* node -> pad port, -1 for keyboards */
static uint16_t joy[2]; static int16_t ana[2][2][2];
static int amin[MAXN][8], amax[MAXN][8];
static uint8_t key[512];                  /* by RETROK */
static int kjoy;                          /* keyboard-as-pad bits */
static bool menu_req, menu_on;
static uint8_t mq[16]; static int mq_n;     /* menu event queue */
static uint16_t pm[2];                      /* pad bits (incl. left stick) last poll, for edges */
static void mq_push(int e) { if (mq_n < 16) mq[mq_n++] = (uint8_t)e; }
static double last_scan;
static int mrx, mry, mx, my;              /* mouse: deltas gathering since the last poll, and the poll's */
static uint8_t mbtn, mwh, mwhp;           /* buttons L|R<<1|M<<2; wheel up|down<<1 gathering, and the poll's */
static int macy;                          /* menu: vertical mouse motion not yet turned into a step */
#define MENU_STEP 24                      /* mouse counts per menu step */
bool in_computer;
void (*in_kbd_cb)(bool, unsigned, uint32_t, uint16_t);

/* evdev KEY_* -> RETROK_* */
static uint16_t kmap[256];
static void kmap_init(void)
{
    static const char row1[] = "1234567890-=", rowq[] = "qwertyuiop[]", rowa[] = "asdfghjkl;'`",
                      rowz[] = "\\zxcvbnm,./";
    for (int i = 0; row1[i]; i++) kmap[2 + i] = (uint16_t)row1[i];
    for (int i = 0; rowq[i]; i++) kmap[16 + i] = (uint16_t)rowq[i];
    for (int i = 0; rowa[i]; i++) kmap[30 + i] = (uint16_t)rowa[i];
    for (int i = 0; rowz[i]; i++) kmap[43 + i] = (uint16_t)rowz[i];
    kmap[1] = 27; kmap[14] = 8; kmap[15] = 9; kmap[28] = 13; kmap[57] = 32; kmap[111] = 127;
    kmap[29] = 306; kmap[97] = 305; kmap[42] = 304; kmap[54] = 303; kmap[56] = 308; kmap[100] = 307;
    kmap[125] = 311; kmap[126] = 312; kmap[127] = 319; kmap[58] = 301; kmap[69] = 300; kmap[70] = 302;
    for (int i = 0; i < 10; i++) kmap[59 + i] = (uint16_t)(282 + i);       /* F1..F10 */
    kmap[87] = 292; kmap[88] = 293;                                         /* F11 F12 */
    kmap[103] = 273; kmap[108] = 274; kmap[106] = 275; kmap[105] = 276;
    kmap[110] = 277; kmap[102] = 278; kmap[107] = 279; kmap[104] = 280; kmap[109] = 281;
    static const uint8_t kp[10] = { 82, 79, 80, 81, 75, 76, 77, 71, 72, 73 };
    for (int i = 0; i < 10; i++) kmap[kp[i]] = (uint16_t)(256 + i);
    kmap[83] = 266; kmap[98] = 267; kmap[55] = 268; kmap[74] = 269; kmap[78] = 270; kmap[96] = 271;
    kmap[99] = 316; kmap[119] = 19;
}

static uint16_t mods(void)
{
    return (uint16_t)((key[303] || key[304] ? 1 : 0) | (key[305] || key[306] ? 2 : 0) |
                      (key[307] || key[308] ? 4 : 0) | (key[311] || key[312] ? 8 : 0));
}

static uint32_t ch_of(unsigned rk)
{
    if (rk >= 32 && rk < 127) {
        if (!(mods() & 1)) return rk;
        if (rk >= 'a' && rk <= 'z') return rk - 32;
        static const char a[] = "1234567890-=[];'`\\,./", b[] = "!@#$%^&*()_+{}:\"~|<>?";
        const char *p = strchr(a, (int)rk); return p ? (uint32_t)b[p - a] : rk;
    }
    return rk == 13 ? 13 : rk == 8 ? 8 : rk == 9 ? 9 : rk == 27 ? 27 : 0;
}

static void scan(void)
{
    /* A BT keyboard with a pointer interface (the 5.1 combos) is one node classified MOUSE and is read
     * as a keyboard; its pointer events reach the mouse like a plain mouse's (pad_of -3). */
    int cap[INPUT_KIND_COUNT] = { 0, 4, 2, 2 };
    int old = nn;
    nn = input_scan(nodes, nn, MAXN, cap);
    for (int i = old; i < nn; i++) {
        pad_of[i] = nodes[i].kind == INPUT_KIND_MOUSE && !nodes[i].keys ? -3 : -1;
        if (nodes[i].kind == INPUT_KIND_PAD) {
            int used[2] = { 0, 0 };
            for (int j = 0; j < i; j++) if (pad_of[j] >= 0) used[pad_of[j]] = 1;   /* only nodes already assigned: later slots hold stale 0 */
            pad_of[i] = used[0] ? (used[1] ? -2 : 1) : 0;
            for (int a = 0; a < 8; a++) {
                struct input_absinfo ai;
                amin[i][a] = -32768; amax[i][a] = 32767;
                if (ioctl(nodes[i].fd, EVIOCGABS(a), &ai) == 0 && ai.maximum > ai.minimum)
                    { amin[i][a] = ai.minimum; amax[i][a] = ai.maximum; }
            }
        }
        fprintf(stderr, "retro: input %s %s \"%s\"%s\n", nodes[i].path,
                nodes[i].kind == INPUT_KIND_PAD ? "pad" : pad_of[i] == -3 ? "mouse" : "keyboard", nodes[i].name,
                pad_of[i] >= 0 ? (pad_of[i] ? " -> port 1" : " -> port 0") : "");
    }
}

void in_open(void) { kmap_init(); scan(); last_scan = now_s(); }

static int jbit_of(int code)
{
    switch (code) {
    case BTN_SOUTH: return 0;  case BTN_EAST: return 8;  case BTN_WEST: return 1;  case BTN_NORTH: return 9;
    case BTN_TL: return 10;    case BTN_TR: return 11;   case BTN_TL2: return 12;  case BTN_TR2: return 13;
    case BTN_SELECT: return 2; case BTN_START: return 3; case BTN_THUMBL: return 14; case BTN_THUMBR: return 15;
    case BTN_DPAD_UP: return 4; case BTN_DPAD_DOWN: return 5; case BTN_DPAD_LEFT: return 6; case BTN_DPAD_RIGHT: return 7;
    }
    return -1;
}

static int16_t norm(int i, int a, int v)
{
    long long r = amax[i][a] - amin[i][a];   /* 64-bit: long is 32 here and (v - min) * 65535 overflows past centre */
    long long n = ((long long)(v - amin[i][a]) * 65535) / (r ? r : 1) - 32768;   /* soft divide, rare */
    return (int16_t)(n < -32768 ? -32768 : n > 32767 ? 32767 : n);
}

static void kbd_joy(unsigned rk, bool down)
{
    int b = -1;
    switch (rk) {
    case 273: b = 4; break; case 274: b = 5; break; case 276: b = 6; break; case 275: b = 7; break;
    case 'z': b = 0; break; case 'x': b = 8; break; case 'a': b = 1; break; case 's': b = 9; break;
    case 'q': b = 10; break; case 'w': b = 11; break; case 13: b = 3; break; case 303: case 8: b = 2; break;
    }
    if (b >= 0) { if (down) kjoy |= 1 << b; else kjoy &= ~(1 << b); }
}

void in_poll(void)
{
    double t = now_s();
    if (t - last_scan > 2.0) { last_scan = t; scan(); }
    for (int i = 0; i < nn; i++) {
        struct input_event ev[32];
        ssize_t r;
        while ((r = read(nodes[i].fd, ev, sizeof ev)) > 0) {
            for (int e = 0; e < (int)(r / (ssize_t)sizeof ev[0]); e++) {
                struct input_event *x = &ev[e];
                int p = pad_of[i];
                if (x->type == EV_REL && p < 0) {       /* any mouse or keyboard-with-pointer node */
                    if (menu_on) {          /* the menu: wheel and motion step the selection */
                        if (x->code == REL_WHEEL) mq_push(x->value > 0 ? MEV_UP : MEV_DOWN);
                        else if (x->code == REL_Y && (macy += x->value) && abs(macy) >= MENU_STEP) { mq_push(macy > 0 ? MEV_DOWN : MEV_UP); macy = 0; }
                        continue;
                    }
                    if (x->code == REL_X) mrx += x->value;
                    else if (x->code == REL_Y) mry += x->value;
                    else if (x->code == REL_WHEEL) mwh |= x->value > 0 ? 1 : 2;
                    continue;
                }
                if (x->type == EV_KEY && p < 0 && x->code >= BTN_LEFT && x->code <= BTN_MIDDLE) {
                    int b = 1 << (x->code - BTN_LEFT);   /* BTN_LEFT, BTN_RIGHT, BTN_MIDDLE are consecutive */
                    if (menu_on) { if (x->value && x->code == BTN_LEFT) mq_push(MEV_OK); else if (x->value && x->code == BTN_RIGHT) mq_push(MEV_BACK); }   /* left = OK, right = Back */
                    else if (x->value) mbtn |= (uint8_t)b;
                    else mbtn &= (uint8_t)~b;
                    continue;
                }
                if (x->type == EV_KEY && p >= 0) {
                    int b = jbit_of(input_pad_key(nodes[i].pad_layout, x->code));
                    if (b >= 0) { if (x->value) joy[p] |= (uint16_t)(1 << b); else joy[p] &= (uint16_t)~(1 << b); }
                } else if (x->type == EV_ABS && p >= 0) {
                    int a = x->code;
                    if (a == ABS_HAT0X || a == ABS_HAT0Y) {
                        int lo = a == ABS_HAT0X ? 6 : 4, hi = lo + 1;
                        joy[p] &= (uint16_t)~((1 << lo) | (1 << hi));
                        if (x->value < 0) joy[p] |= (uint16_t)(1 << lo);
                        if (x->value > 0) joy[p] |= (uint16_t)(1 << hi);
                    } else if (a < 8) {
                        int16_t v = norm(i, a, x->value);
                        if (a == ABS_X) ana[p][0][0] = v; else if (a == ABS_Y) ana[p][0][1] = v;
                        else if (a == ABS_RX) ana[p][1][0] = v; else if (a == ABS_RY) ana[p][1][1] = v;
                        else if (a == ABS_Z || a == ABS_RZ) {
                            int b = a == ABS_Z ? 12 : 13;
                            if (v > 0) joy[p] |= (uint16_t)(1 << b); else joy[p] &= (uint16_t)~(1 << b);
                        }
                    }
                } else if (x->type == EV_KEY && p == -1 && x->code < 256 && x->value != 2) {
                    unsigned rk = kmap[x->code];
                    if (!rk) continue;
                    bool down = x->value != 0;
                    /* F12 and (pad-style cores only: a computer core needs Esc as a key) Esc open the
                     * menu and never reach the core; in the menu every key is a menu event */
                    if (menu_on || rk == 293 || (rk == 27 && !in_computer)) {
                        if (down && menu_on) {
                            int ev = rk == 273 ? MEV_UP : rk == 274 ? MEV_DOWN : rk == 276 ? MEV_LEFT : rk == 275 ? MEV_RIGHT :
                                     rk == 13 || rk == 271 ? MEV_OK : rk == 27 || rk == 293 ? MEV_BACK : 0;
                            if (ev) mq_push(ev);
                        } else if (down) menu_req = true;
                        continue;
                    }
                    key[rk] = down;
                    if (!in_computer) kbd_joy(rk, down);
                    if (in_kbd_cb) in_kbd_cb(down, rk, down ? ch_of(rk) : 0, mods());
                }
            }
        }
        if (r < 0 && errno == ENODEV) {
            fprintf(stderr, "retro: input %s gone\n", nodes[i].path);
            if (pad_of[i] >= 0) joy[pad_of[i]] = 0;
            for (int j = i; j + 1 < nn; j++) { pad_of[j] = pad_of[j + 1];
                memcpy(amin[j], amin[j + 1], sizeof amin[j]); memcpy(amax[j], amax[j + 1], sizeof amax[j]); }
            nn = input_scan_drop(nodes, nn, i); i--;
        }
    }
    mx = mrx; my = mry; mrx = mry = 0; mwhp = mwh; mwh = 0;   /* libretro mouse X/Y are deltas since the last poll */
    for (int p = 0; p < 2; p++) {
        uint16_t m = joy[p];
        if (ana[p][0][0] < -16000) m |= 1 << 6;
        if (ana[p][0][0] > 16000) m |= 1 << 7;
        if (ana[p][0][1] < -16000) m |= 1 << 4;
        if (ana[p][0][1] > 16000) m |= 1 << 5;
        uint16_t e = (uint16_t)(m & ~pm[p]);
        if ((m & 0xC) == 0xC && (pm[p] & 0xC) != 0xC) {        /* Select+Start, on the press edge */
            if (menu_on) mq_push(MEV_BACK); else menu_req = true;
        } else if (menu_on) {
            if (e & 0x10) mq_push(MEV_UP);
            if (e & 0x20) mq_push(MEV_DOWN);
            if (e & 0x40) mq_push(MEV_LEFT);
            if (e & 0x80) mq_push(MEV_RIGHT);
            if (e & 0x01) mq_push(MEV_OK);                      /* Xbox A */
            if (e & 0x100) mq_push(MEV_BACK);                   /* Xbox B */
        }
        pm[p] = m;
    }
}

void in_release(void)
{
    for (unsigned rk = 0; rk < 512; rk++)
        if (key[rk]) { key[rk] = 0; if (in_kbd_cb) in_kbd_cb(false, rk, 0, 0); }
    kjoy = 0; memset(joy, 0, sizeof joy); memset(ana, 0, sizeof ana);
    mrx = mry = mx = my = 0; mbtn = mwh = mwhp = 0;
}

void in_menu_mode(bool on) { menu_on = on; menu_req = false; mq_n = 0; macy = 0; }
int in_menu_ev(void)
{
    if (!mq_n) return 0;
    int e = mq[0]; memmove(mq, mq + 1, (size_t)--mq_n); return e;
}

int16_t in_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (dev == RDEV_KEYBOARD) { in_computer = true; return port == 0 && id < 512 ? key[id] : 0; }
    if (dev == RDEV_MOUSE) {                 /* ids: X, Y, LEFT, RIGHT, WHEELUP, WHEELDOWN, MIDDLE */
        if (port != 0) return 0;
        switch (id) {
        case 0: return (int16_t)mx;  case 1: return (int16_t)my;
        case 2: return mbtn & 1;     case 3: return (mbtn >> 1) & 1;  case 6: return (mbtn >> 2) & 1;
        case 4: return mwhp & 1;     case 5: return (mwhp >> 1) & 1;
        }
        return 0;
    }
    if (port > 1) return 0;
    unsigned bits = joy[port] | (port == 0 && !in_computer ? (unsigned)kjoy : 0);
    /* left stick also drives the d-pad, for cores that only read digital */
    int16_t ax = ana[port][0][0], ay = ana[port][0][1];
    if (ax < -16000) bits |= 1 << 6;
    if (ax > 16000) bits |= 1 << 7;
    if (ay < -16000) bits |= 1 << 4;
    if (ay > 16000) bits |= 1 << 5;
    if (dev == RDEV_JOYPAD) return id == 256 ? (int16_t)bits : (int16_t)(id < 16 ? (bits >> id) & 1 : 0);
    if (dev == RDEV_ANALOG && idx < 2 && id < 2) return ana[port][idx][id];
    return 0;
}

bool in_menu_req(void) { return menu_req; }

void in_close(void) { while (nn > 0) nn = input_scan_drop(nodes, nn, nn - 1); }
