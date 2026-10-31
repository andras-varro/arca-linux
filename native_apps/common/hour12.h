/* hour12.h - the one 24 h -> 12 h mapping and AM/PM suffix.
 *
 * Hour 0 is 12 AM, hour 12 is 12 PM.  Every 12 h text in the tree (the top
 * bar, the clock app, the Control Panel's LOCAL row) takes its hour and its
 * suffix from here; header-only so any component can use it without a link
 * change.  Callers range-check the hour (0..23) themselves. */
#ifndef HOUR12_H
#define HOUR12_H

static inline int hour12_of(int hour24) { int x = hour24 % 12; return x ? x : 12; }
static inline const char *hour12_ampm(int hour24) { return hour24 < 12 ? "AM" : "PM"; }

#endif
