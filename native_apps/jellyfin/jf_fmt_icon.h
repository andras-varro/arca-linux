/*
 * jf_fmt_icon.h — the format icon of a track row (FLAC, MP3, WAV, ...): a small PPM
 * per container name, loaded once and handed to the list as an LvIcon.
 *
 *   const LvIcon *ic = jf_fmt_icon_get(it->container);   NULL: keep the page glyph
 *
 * The file is <dir>/ludfin-fmt-<container>.ppm, made by gen_fmt_icons.py; the
 * generator's table is the only list of formats.  A name with no readable file is a
 * remembered miss (tried once), so a row redraw never touches the disk twice.  The name
 * is reduced to [a-z0-9] up to JF_FMT_NAME_MAX before it reaches a path: anything else
 * is NULL without a filesystem call.  The cache holds JF_FMT_CACHE_MAX names; a name
 * past that is NULL, uncached.  The tiles live for the process.
 */
#ifndef ARCA_JF_FMT_ICON_H
#define ARCA_JF_FMT_ICON_H

#include "list_view.h"

#define JF_FMT_ICON_DIR  "/opt/arca/icons"
#define JF_FMT_NAME_MAX  11   /* JfItem.container is 12 bytes */
#define JF_FMT_CACHE_MAX 16

const LvIcon *jf_fmt_icon_get(const char *container);

/* Where the files are read from (a test points it at a temp dir) and the cache is
 * emptied, tiles freed.  NULL restores JF_FMT_ICON_DIR. */
void jf_fmt_icon_set_dir(const char *dir);

#endif
