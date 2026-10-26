/* retro input: USB keyboards and pads through input_scan, read as raw evdev.
 * Pads -> RETRO_DEVICE_JOYPAD/ANALOG ports 0..1 (SNES layout: Xbox A = B,
 * B = A, X = Y, Y = X).  Keyboard -> RETRO_DEVICE_KEYBOARD and the keyboard
 * callback; for a core that never asks for the keyboard it doubles as pad 0.
 * Quit: pad Select+Start, or F12 (Esc too, unless the core is a computer with a keyboard). */
#include "retro.h"
#include "input_scan.h"
#include <errno.h>
#include <stdio.h>
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
static bool quit_req;
static double last_scan;
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
    /* A BT keyboard with a pointer interface (the 5.1 combos) is one node classified MOUSE: allow
     * mice through the scan, then keep only those that also carry the letter keys. */
    int cap[INPUT_KIND_COUNT] = { 0, 4, 2, 2 };
    int old = nn;
    nn = input_scan(nodes, nn, MAXN, cap);
    for (int i = old; i < nn; i++)
        if (nodes[i].kind == INPUT_KIND_MOUSE && !nodes[i].keys) { nn = input_scan_drop(nodes, nn, i); i--; }
    for (int i = old; i < nn; i++) {
        pad_of[i] = -1;
        if (nodes[i].kind == INPUT_KIND_PAD) {
            int used[2] = { 0, 0 };
            for (int j = 0; j < nn; j++) if (j != i && pad_of[j] >= 0) used[pad_of[j]] = 1;
            pad_of[i] = used[0] ? (used[1] ? -2 : 1) : 0;
            for (int a = 0; a < 8; a++) {
                struct input_absinfo ai;
                amin[i][a] = -32768; amax[i][a] = 32767;
                if (ioctl(nodes[i].fd, EVIOCGABS(a), &ai) == 0 && ai.maximum > ai.minimum)
                    { amin[i][a] = ai.minimum; amax[i][a] = ai.maximum; }
            }
        }
        fprintf(stderr, "retro: input %s %s \"%s\"%s\n", nodes[i].path,
                nodes[i].kind == INPUT_KIND_PAD ? "pad" : "keyboard", nodes[i].name,
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
    long r = amax[i][a] - amin[i][a];
    long n = ((long)(v - amin[i][a]) * 65535) / (r ? r : 1) - 32768;   /* soft divide, rare */
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
                    key[rk] = down;
                    if (rk == 293 && down) quit_req = true;       /* F12 */
                    if (rk == 27 && down && !in_computer) quit_req = true;   /* Esc, pad-style cores only: a computer core needs it as a key */
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
    for (int p = 0; p < 2; p++)
        if ((joy[p] & 0xC) == 0xC) quit_req = true;          /* Select+Start */
}

int16_t in_state(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
    if (dev == RDEV_KEYBOARD) { in_computer = true; return port == 0 && id < 512 ? key[id] : 0; }
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

bool in_quit(void) { return quit_req; }

void in_close(void) { while (nn > 0) nn = input_scan_drop(nodes, nn, nn - 1); }
