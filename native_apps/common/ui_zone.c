/* ui_zone — see ui_zone.h.  Pure arithmetic over UiZone arrays; no framebuffer. */
#include "ui_zone.h"
#include <string.h>

static bool zone_present(const UiZone *z) { return z->lv || z->n > 0; }

static int id_index(const UiZone *z, int id) {
    for (int i = 0; i < z->n && i < UZ_BUTTONS; i++)
        if (z->id[i] == id) return i;
    return -1;
}

/* The nearest present zone strictly above (dir < 0) or below (dir > 0) `from`, or -1. */
static int adjacent(const UiZone *z, int nz, int from, int dir) {
    for (int i = from + dir; i >= 0 && i < nz; i += dir)
        if (zone_present(&z[i])) return i;
    return -1;
}

/* Where focus lands when the focused zone is gone: the nearest list zone, else the nearest present zone. */
static int fallback_zone(const UiZone *z, int nz, int from) {
    int best = -1, bd = 0;
    for (int pass = 0; pass < 2 && best < 0; pass++)
        for (int i = 0; i < nz; i++) {
            if (!zone_present(&z[i]) || (pass == 0 && !z[i].lv)) continue;
            int d = i > from ? i - from : from - i;
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
    return best;
}

void uz_begin(UiZoneFocus *f, int zone) {
    memset(f, 0, sizeof(*f));
    f->zone = zone;
    f->id = -1;
    for (int i = 0; i < UZ_ZONES; i++) f->mem[i] = -1;
}

void uz_begin_cycle(UiZoneFocus *f, int zone) {
    uz_begin(f, zone);
    f->cycle = true;
}

/* Focus the first button of zone `to` (or the list), as a fresh entry. */
static void enter(UiZoneFocus *f, const UiZone *z, int to) {
    f->zone = to;
    f->id = -1;
    memset(&f->last, 0, sizeof(f->last));
    if (!z[to].lv && z[to].n > 0) {
        int k = ui_focus_first(z[to].r, z[to].n);
        if (k >= 0) { f->id = z[to].id[k]; f->last = z[to].r[k]; }
    }
}

void uz_focus(UiZoneFocus *f, const UiZone *z, int nz, int zone, int id) {
    if (zone < 0 || zone >= nz) return;
    f->zone = zone;
    f->id = -1;
    memset(&f->last, 0, sizeof(f->last));
    if (z[zone].lv) return;
    int k = id_index(&z[zone], id);
    if (k >= 0) { f->id = id; f->last = z[zone].r[k]; }
}

/* Make (zone, id) valid for this frame's zones.  True when anything changed. */
static bool resolve(UiZoneFocus *f, const UiZone *z, int nz) {
    int zone = f->zone, id = f->id;
    if (zone < 0 || zone >= nz || !zone_present(&z[zone])) {
        int to = fallback_zone(z, nz, zone < 0 ? 0 : zone >= nz ? nz - 1 : zone);
        if (to < 0) return false;
        enter(f, z, to);
        return true;
    }
    if (z[zone].lv) {
        f->id = -1;
        return id != -1;
    }
    int k = id_index(&z[zone], id);
    if (k < 0) {
        k = f->last.w > 0 ? ui_focus_nearest(z[zone].r, z[zone].n, &f->last)
                          : ui_focus_first(z[zone].r, z[zone].n);
        if (k < 0) return false;
        f->id = z[zone].id[k];
    }
    f->last = z[zone].r[k];
    return f->id != id;
}

/* The next (dir > 0) or previous present zone from `from`, wrapping; -1 when no OTHER zone is present. */
static int cycle_zone(const UiZone *z, int nz, int from, int dir) {
    for (int s = 1; s < nz; s++) {
        int i = ((from + dir * s) % nz + nz) % nz;
        if (zone_present(&z[i])) return i;
    }
    return -1;
}

/* Cycle mode: Tab / Shift+Tab / LB / RB change pane, arrows never leave it. */
static void cycle_input(UiZoneFocus *f, const UiZone *z, int nz, const InputState *in, UzOut *out) {
    const ButtonState *b = in->buttons;
    bool next = b[BTN_ID_NEXT].pressed && !b[BTN_ID_RUN].held;
    bool prev = b[BTN_ID_PREV].pressed || (b[BTN_ID_NEXT].pressed && b[BTN_ID_RUN].held);
    bool act = b[BTN_ID_ACTION].pressed || b[BTN_ID_JUMP].pressed;
    bool up = b[BTN_ID_UP].pressed, down = b[BTN_ID_DOWN].pressed;
    bool left = b[BTN_ID_LEFT].pressed, right = b[BTN_ID_RIGHT].pressed;
    if (b[BTN_ID_PAUSE].pressed || b[BTN_ID_BACK].pressed || b[BTN_ID_CANCEL].pressed) out->back = true;
    if (next || prev || up || down || left || right || act) {
        if (!f->shown) { f->shown = true; out->moved = true; }
    }

    if (next || prev) {
        int to = cycle_zone(z, nz, f->zone, next ? +1 : -1);
        if (to >= 0) {
            if (!z[f->zone].lv && f->id >= 0 && f->zone < UZ_ZONES) f->mem[f->zone] = f->id;
            enter(f, z, to);
            if (!z[to].lv && to < UZ_ZONES) {
                int k = id_index(&z[to], f->mem[to]);
                if (k >= 0) { f->id = z[to].id[k]; f->last = z[to].r[k]; }
            }
            out->moved = true;
        }
        return;
    }

    const UiZone *cz = &z[f->zone];
    if (cz->lv) {
        InputState *c = &out->list_buf;
        *c = *in;
        c->buttons[BTN_ID_ACTION].pressed = c->buttons[BTN_ID_ACTION].held = false;
        c->buttons[BTN_ID_JUMP].pressed = c->buttons[BTN_ID_JUMP].held = false;
        out->list_in = c;
        if (act && cz->lv->cursor >= 0 && cz->lv->cursor < cz->lv->count) out->activate_row = cz->lv->cursor;
        return;
    }

    int k = id_index(cz, f->id);
    if (k < 0) return;
    if (act) out->activate = f->id;
    if (cz->flags & UZ_SLIDER) {
        out->left_right = left ? -1 : right ? +1 : 0;
        return;
    }
    int dir = up ? (int)UI_DIR_UP : down ? (int)UI_DIR_DOWN : left ? (int)UI_DIR_LEFT : right ? (int)UI_DIR_RIGHT : -1;
    if (dir < 0) return;
    int m = (dir == UI_DIR_LEFT || dir == UI_DIR_RIGHT) ? ui_focus_move(cz->r, cz->n, k, (UiDir)dir)
                                                         : ui_focus_step(cz->r, cz->n, k, (UiDir)dir);
    if (m >= 0 && m != k) {
        f->id = cz->id[m];
        f->last = cz->r[m];
        out->moved = true;
    }
}

void uz_input(UiZoneFocus *f, const UiZone *z, int nz, const InputState *in, bool ts_pressed, UzOut *out) {
    out->moved = false;
    out->activate = -1;
    out->left = -1;
    out->list_in = NULL;
    out->back = false;
    out->activate_row = -1;
    out->left_right = 0;
    if (nz > UZ_ZONES) nz = UZ_ZONES;
    if (nz <= 0) return;

    if (resolve(f, z, nz)) out->moved = true;

    if (ts_pressed) {
        if (f->shown) { f->shown = false; out->moved = true; }
        return;
    }
    if (f->cycle) {
        if (in) cycle_input(f, z, nz, in, out);
        return;
    }
    bool up = in && in->buttons[BTN_ID_UP].pressed;
    bool down = in && in->buttons[BTN_ID_DOWN].pressed;
    bool left = in && in->buttons[BTN_ID_LEFT].pressed;
    bool right = in && in->buttons[BTN_ID_RIGHT].pressed;
    bool act = in && (in->buttons[BTN_ID_ACTION].pressed || in->buttons[BTN_ID_JUMP].pressed);
    if ((up || down || left || right || act) && !f->shown) { f->shown = true; out->moved = true; }

    const UiZone *cz = &z[f->zone];
    if (cz->lv) {
        /* One direction decides: leaving the list this frame means the list does not also take the key. */
        int to = -1;
        if (up && cz->lv->cursor <= 0) to = adjacent(z, nz, f->zone, -1);
        else if (!up && down && cz->lv->cursor >= cz->lv->count - 1) to = adjacent(z, nz, f->zone, +1);
        if (to >= 0) {
            enter(f, z, to);
            out->moved = true;
            return;
        }
        out->list_in = in;
        return;
    }

    int k = id_index(cz, f->id);
    if (k < 0) return;
    int dir = up ? (int)UI_DIR_UP : down ? (int)UI_DIR_DOWN : left ? (int)UI_DIR_LEFT : right ? (int)UI_DIR_RIGHT : -1;
    if (dir < 0) {
        if (act) out->activate = f->id;
        return;
    }
    if (dir == UI_DIR_LEFT || dir == UI_DIR_RIGHT) {
        int m = ui_focus_move(cz->r, cz->n, k, (UiDir)dir);
        if (m >= 0 && m != k) {
            f->id = cz->id[m];
            f->last = cz->r[m];
            out->moved = true;
        }
        return;
    }
    int m = ui_focus_step(cz->r, cz->n, k, (UiDir)dir);
    if (m >= 0) {
        f->id = cz->id[m];
        f->last = cz->r[m];
        out->moved = true;
        return;
    }
    if (cz->flags & UZ_TRAP) {
        out->left = f->zone;
        return;
    }
    int to = adjacent(z, nz, f->zone, dir == UI_DIR_UP ? -1 : +1);
    if (to >= 0) {
        enter(f, z, to);
        out->moved = true;
    }
}

bool uz_ring(const UiZoneFocus *f, const UiZone *z, int nz, UiRect *out) {
    if (!f->shown || f->zone < 0 || f->zone >= nz || z[f->zone].lv) return false;
    int k = id_index(&z[f->zone], f->id);
    if (k < 0) return false;
    *out = z[f->zone].r[k];
    return true;
}
