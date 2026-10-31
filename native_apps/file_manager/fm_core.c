/* fm_core.c — see fm_core.h. */
#define _GNU_SOURCE
#include "fm_core.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* ── Paths and names ─────────────────────────────────────────────────────── */

bool fm_path_normalise(const char *in, char *out, size_t n) {
    if (!in || !out || n < 2 || in[0] != '/') return false;
    size_t len = 0;               /* out holds "" or "/a/b", never a trailing '/' */
    out[0] = '\0';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *c = p;
        while (*p && *p != '/') p++;
        size_t cl = (size_t)(p - c);
        if (cl == 1 && c[0] == '.') continue;
        if (cl == 2 && c[0] == '.' && c[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;                  /* drop the '/' too */
            out[len] = '\0';
            continue;
        }
        if (len + 1 + cl + 1 > n) return false;
        out[len++] = '/';
        memcpy(out + len, c, cl);
        len += cl;
        out[len] = '\0';
    }
    if (len == 0) { out[0] = '/'; out[1] = '\0'; }
    return true;
}

bool fm_name_ok(const char *name) {
    if (!name || !*name) return false;
    size_t l = strlen(name);
    if (l > FM_NAME_MAX - 1) return false;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char *c = name; *c; c++)
        if (*c == '/' || (unsigned char)*c < 0x20 || *c == 0x7f) return false;
    return true;
}

bool fm_path_join(const char *dir, const char *name, char *out, size_t n) {
    if (!dir || !fm_name_ok(name)) return false;
    char tmp[FM_PATH_MAX * 2];
    if (snprintf(tmp, sizeof tmp, "%s/%s", dir, name) >= (int)sizeof tmp) return false;
    return fm_path_normalise(tmp, out, n);
}

bool fm_path_parent(const char *path, char *out, size_t n) {
    char t[FM_PATH_MAX];
    if (!fm_path_normalise(path, t, sizeof t) || !strcmp(t, "/")) return false;
    char *s = strrchr(t, '/');
    if (s == t) t[1] = '\0'; else *s = '\0';
    if (strlen(t) + 1 > n) return false;
    strcpy(out, t);
    return true;
}

bool fm_path_within(const char *root, const char *path) {
    char r[FM_PATH_MAX], p[FM_PATH_MAX];
    if (!fm_path_normalise(root, r, sizeof r) || !fm_path_normalise(path, p, sizeof p))
        return false;
    size_t rl = strlen(r);
    if (!strcmp(r, "/")) return true;
    if (strncmp(p, r, rl) != 0) return false;
    return p[rl] == '\0' || p[rl] == '/';
}

bool fm_path_pseudo(const char *path) {
    return fm_path_within("/proc", path) || fm_path_within("/sys", path) ||
           fm_path_within("/dev", path);
}

bool fm_path_protected(const char *path) {
    char p[FM_PATH_MAX];
    if (!fm_path_normalise(path, p, sizeof p)) return true;    /* fail closed */
    if (!strcmp(p, "/") || fm_path_pseudo(p)) return true;
    return !strchr(p + 1, '/');        /* "/x": a direct child of the root */
}

const char *fm_base(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

bool fm_path_shell_safe(const char *path) {
    if (!path || !*path) return false;
    for (const char *c = path; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '.' && *c != '/' && *c != '-')
            return false;
    return true;
}

FmCheck fm_check_transfer(const char *src_dir, const char *name, const char *dst_dir,
                          bool src_is_dir) {
    if (!fm_name_ok(name)) return FM_ERR_NAME;
    char s[FM_PATH_MAX], d[FM_PATH_MAX], dd[FM_PATH_MAX];
    if (!fm_path_join(src_dir, name, s, sizeof s) ||
        !fm_path_normalise(dst_dir, dd, sizeof dd) ||
        !fm_path_join(dst_dir, name, d, sizeof d))
        return FM_ERR_PATH;
    if (!strcmp(s, d)) return FM_ERR_SAME;
    if (src_is_dir && fm_path_within(s, dd)) return FM_ERR_INTO_SELF;
    return FM_OK;
}

const char *fm_check_text(FmCheck c) {
    switch (c) {
    case FM_OK:            return "OK";
    case FM_ERR_SAME:      return "SOURCE AND TARGET ARE THE SAME";
    case FM_ERR_INTO_SELF: return "CANNOT COPY A FOLDER INTO ITSELF";
    case FM_ERR_NAME:      return "BAD NAME";
    default:               return "BAD PATH";
    }
}

/* ── Text ────────────────────────────────────────────────────────────────── */

void fm_format_size(long long bytes, char *out, size_t n) {
    static const char unit[] = "KMGT";
    if (bytes < 0) bytes = 0;
    if (bytes < 1024) { snprintf(out, n, "%dB", (int)bytes); return; }
    int u = 0, sh = 10;
    while (u < 3 && (bytes >> sh) >= 1024) { u++; sh += 10; }
    long long tenths = (bytes * 10) >> sh;     /* bytes < 2^62 at TB: no overflow */
    if (tenths < 100) snprintf(out, n, "%d.%d%c", (int)(tenths / 10), (int)(tenths % 10), unit[u]);
    else              snprintf(out, n, "%d%c", (int)(tenths / 10), unit[u]);
}

/* ── Directory listing and sorting ───────────────────────────────────────── */

const char *fm_sort_label(FmSort s) {
    switch (s) {
    case FM_SORT_SIZE: return "SIZE";
    case FM_SORT_DATE: return "DATE";
    default:           return "NAME";
    }
}

static int name_cmp(const char *a, const char *b) {
    int c = strcasecmp(a, b);
    return c ? c : strcmp(a, b);
}

int fm_entry_cmp(const FmEntry *a, const FmEntry *b, FmSort s) {
    bool ad = a->type == FM_T_DIR, bd = b->type == FM_T_DIR;
    if (ad != bd) return ad ? -1 : 1;
    if (!ad) {
        if (s == FM_SORT_SIZE && a->size != b->size) return a->size > b->size ? -1 : 1;
        if (s == FM_SORT_DATE && a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
    }
    return name_cmp(a->name, b->name);
}

static FmSort g_sort;
static int qcmp(const void *a, const void *b) { return fm_entry_cmp(a, b, g_sort); }

void fm_dir_sort(FmDir *d, FmSort sort) {
    g_sort = sort;
    if (d->n > 1) qsort(d->e, (size_t)d->n, sizeof *d->e, qcmp);
}

int fm_dir_read(const char *path, FmSort sort, FmDir *out) {
    memset(out, 0, sizeof *out);
    DIR *d = opendir(path);
    if (!d) return -errno;
    int cap = 64;
    out->e = malloc((size_t)cap * sizeof *out->e);
    if (!out->e) { closedir(d); return -ENOMEM; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (out->n >= FM_MAX_ENTRIES) { out->truncated = true; break; }
        char full[FM_PATH_MAX];
        struct stat st;
        if (!fm_path_join(path, de->d_name, full, sizeof full) || lstat(full, &st) < 0)
            continue;               /* vanished, or a name too long to use */
        if (out->n == cap) {
            cap *= 2;
            FmEntry *ne = realloc(out->e, (size_t)cap * sizeof *ne);
            if (!ne) break;
            out->e = ne;
        }
        FmEntry *e = &out->e[out->n];
        e->name = strdup(de->d_name);
        if (!e->name) break;
        e->type = S_ISDIR(st.st_mode) ? FM_T_DIR : S_ISREG(st.st_mode) ? FM_T_FILE
                : S_ISLNK(st.st_mode) ? FM_T_LINK : FM_T_OTHER;
        e->size = (long long)st.st_size;
        e->mtime = (long long)st.st_mtime;
        e->mode = (unsigned)st.st_mode;
        e->link_dir = false;
        if (e->type == FM_T_LINK) {
            struct stat ts;
            e->link_dir = stat(full, &ts) == 0 && S_ISDIR(ts.st_mode);
        }
        out->n++;
    }
    closedir(d);
    fm_dir_sort(out, sort);
    return 0;
}

int fm_dir_find(const FmDir *d, const char *name) {
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->e[i].name, name)) return i;
    return -1;
}

void fm_dir_free(FmDir *d) {
    for (int i = 0; i < d->n; i++) free(d->e[i].name);
    free(d->e);
    memset(d, 0, sizeof *d);
}

void fm_entry_detail(const FmEntry *e, char *out, size_t n) {
    switch (e->type) {
    case FM_T_DIR:   snprintf(out, n, "<DIR>"); break;
    case FM_T_LINK:  snprintf(out, n, "<LNK>"); break;
    case FM_T_OTHER: snprintf(out, n, "<DEV>"); break;
    default:         fm_format_size(e->size, out, n); break;
    }
}

void fm_mode_string(unsigned mode, char out[11]) {
    static const char rwx[] = "rwxrwxrwx";
    unsigned t = mode & S_IFMT;
    out[0] = t == S_IFDIR ? 'd' : t == S_IFLNK ? 'l' : t == S_IFREG ? '-'
           : t == S_IFCHR ? 'c' : t == S_IFBLK ? 'b' : t == S_IFIFO ? 'p'
           : t == S_IFSOCK ? 's' : '?';
    for (int i = 0; i < 9; i++)
        out[1 + i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    if (mode & S_ISUID) out[3] = out[3] == 'x' ? 's' : 'S';
    if (mode & S_ISGID) out[6] = out[6] == 'x' ? 's' : 'S';
    if (mode & S_ISVTX) out[9] = out[9] == 'x' ? 't' : 'T';
    out[10] = '\0';
}

void fm_entry_detail_long(const FmEntry *e, char *out, size_t n) {
    if (!out || n == 0) return;
    char mode[11], size[16], when[24] = "----/--/-- --:--";
    fm_mode_string(e->mode, mode);
    fm_entry_detail(e, size, sizeof size);
    time_t t = (time_t)e->mtime;
    struct tm tm;
    tzset();                           /* a long-running process caches the zone */
    if (localtime_r(&t, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    snprintf(out, n, "%s %s %s", mode, size, when);
}

bool fm_space(const char *path, long long *free_bytes, long long *total_bytes) {
    struct statvfs v;
    if (statvfs(path, &v) < 0) return false;
    if (free_bytes)  *free_bytes  = (long long)v.f_bavail * (long long)v.f_frsize;
    if (total_bytes) *total_bytes = (long long)v.f_blocks * (long long)v.f_frsize;
    return true;
}

int fm_link_dir_target(const char *path, char *out, size_t n) {
    if (!path || !out || n == 0) return -EINVAL;
    char *r = realpath(path, NULL);
    if (!r) return -errno;
    struct stat st;
    int rc;
    if (stat(r, &st) < 0) rc = -errno;
    else if (!S_ISDIR(st.st_mode)) rc = -ENOTDIR;
    else if (strlen(r) + 1 > n) rc = -ENAMETOOLONG;
    else { strcpy(out, r); rc = 0; }
    free(r);
    return rc;
}

/* ── The USB helper's output ─────────────────────────────────────────────── */

/* Next space-separated word of *p into out; false when there is none or it
 * does not fit.  *p is left at the rest of the line. */
static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static bool word(const char **p, char *out, size_t n) {
    const char *s = *p;
    while (blank(*s)) s++;
    const char *e = s;
    while (*e && !blank(*e)) e++;
    if (e == s || (size_t)(e - s) >= n) return false;
    memcpy(out, s, (size_t)(e - s));
    out[e - s] = '\0';
    while (blank(*e)) e++;
    *p = e;
    return true;
}

static bool mp_ok(const char *mp) {
    char t[FM_PATH_MAX];
    return fm_path_normalise(mp, t, sizeof t) && !strcmp(t, mp) && strcmp(mp, "/") &&
           fm_path_shell_safe(mp);
}

static bool dev_ok(const char *dev) {
    return !strncmp(dev, "/dev/", 5) && dev[5] && fm_path_shell_safe(dev);
}

FmStLine fm_parse_storage_line(const char *line) {
    FmStLine r;
    memset(&r, 0, sizeof r);
    if (!line) return r;
    const char *p = line;
    char kw[24];
    if (!word(&p, kw, sizeof kw)) return r;
    if (!strcmp(kw, "MOUNTED") || !strcmp(kw, "ALREADY")) {
        if (word(&p, r.mp, sizeof r.mp) && mp_ok(r.mp) &&
            word(&p, r.dev, sizeof r.dev) && dev_ok(r.dev))
            r.kind = kw[0] == 'M' ? FM_ST_MOUNTED : FM_ST_ALREADY;
    } else if (!strcmp(kw, "UNSUPPORTED")) {
        if (word(&p, r.dev, sizeof r.dev) && dev_ok(r.dev) &&
            word(&p, r.extra, sizeof r.extra))
            r.kind = FM_ST_UNSUPPORTED;
    } else if (!strcmp(kw, "FAILED")) {
        if (word(&p, r.dev, sizeof r.dev) && dev_ok(r.dev)) {
            snprintf(r.extra, sizeof r.extra, "%s", p);
            size_t l = strlen(r.extra);
            while (l && (r.extra[l - 1] == '\n' || r.extra[l - 1] == '\r' || r.extra[l - 1] == ' '))
                r.extra[--l] = '\0';
            r.kind = FM_ST_FAILED;
        }
    } else if (!strcmp(kw, "GONE") || !strcmp(kw, "EJECTED") || !strcmp(kw, "BUSY")) {
        if (word(&p, r.mp, sizeof r.mp) && mp_ok(r.mp))
            r.kind = kw[0] == 'G' ? FM_ST_GONE : kw[0] == 'E' ? FM_ST_EJECTED : FM_ST_BUSY;
    }
    /* Every verb but FAILED takes exact fields: a trailing word is a garbled line. */
    if (r.kind != FM_ST_NONE && r.kind != FM_ST_FAILED && *p) r.kind = FM_ST_NONE;
    if (r.kind == FM_ST_NONE) memset(&r, 0, sizeof r);
    return r;
}

/* ── Single operations ───────────────────────────────────────────────────── */

int fm_rename(const char *dir, const char *old_name, const char *new_name) {
    char a[FM_PATH_MAX], b[FM_PATH_MAX];
    struct stat st;
    if (!fm_name_ok(old_name) || !fm_name_ok(new_name)) return EINVAL;
    if (!fm_path_join(dir, old_name, a, sizeof a) || !fm_path_join(dir, new_name, b, sizeof b))
        return ENAMETOOLONG;
    if (!strcmp(a, b)) return 0;
    if (lstat(b, &st) == 0) return EEXIST;
    return rename(a, b) == 0 ? 0 : errno;
}

int fm_mkdir(const char *dir, const char *name) {
    char p[FM_PATH_MAX];
    if (!fm_name_ok(name)) return EINVAL;
    if (!fm_path_join(dir, name, p, sizeof p)) return ENAMETOOLONG;
    return mkdir(p, 0755) == 0 ? 0 : errno;
}

/* ── The job ─────────────────────────────────────────────────────────────── */

#define META_COST 2048     /* what one file-system call is charged against the budget */

static void job_fail(FmJob *j, int e, const char *what, const char *path) {
    j->err_no = e;
    j->err_fatal = e == ENOSPC || e == EIO || e == EROFS || e == EDQUOT || e == EFBIG;
    snprintf(j->err_msg, sizeof j->err_msg, "%s\n%s", what, path);
    if (e) {
        size_t l = strlen(j->err_msg);
        snprintf(j->err_msg + l, sizeof j->err_msg - l, "\n%s", strerror(e));
    }
    j->errors++;
    j->state = FM_J_ERROR;
}

static void file_abort(FmJob *j, bool remove_target) {
    if (j->in_fd >= 0)  { close(j->in_fd);  j->in_fd = -1; }
    if (j->out_fd >= 0) { close(j->out_fd); j->out_fd = -1; if (remove_target) unlink(j->cur_dst); }
}

int fm_job_init(FmJob *j, FmOp op, const char *src_dir, const char *dst_dir,
                const char *const *names, int n, char *err, size_t errn) {
    memset(j, 0, sizeof *j);
    j->in_fd = j->out_fd = -1;
    j->op = op;
    if (err && errn) err[0] = '\0';
#define BAD(...) do { if (err && errn) snprintf(err, errn, __VA_ARGS__); fm_job_free(j); return -1; } while (0)
    if (n <= 0) BAD("NOTHING TO DO");
    if (!fm_path_normalise(src_dir, j->src_dir, sizeof j->src_dir)) BAD("BAD PATH");
    if (op != FM_OP_DELETE) {
        struct stat dt;
        if (!dst_dir || !fm_path_normalise(dst_dir, j->dst_dir, sizeof j->dst_dir)) BAD("BAD PATH");
        if (stat(j->dst_dir, &dt) < 0 || !S_ISDIR(dt.st_mode)) BAD("TARGET IS NOT A FOLDER");
    }
    j->names = calloc((size_t)n, sizeof *j->names);
    if (!j->names) BAD("OUT OF MEMORY");
    for (int i = 0; i < n; i++) {
        char p[FM_PATH_MAX];
        struct stat st;
        if (!fm_name_ok(names[i]) || !fm_path_join(j->src_dir, names[i], p, sizeof p))
            BAD("BAD NAME");
        if (lstat(p, &st) < 0) BAD("%s: %s", names[i], strerror(errno));
        if (op != FM_OP_DELETE) {
            FmCheck c = fm_check_transfer(j->src_dir, names[i], j->dst_dir, S_ISDIR(st.st_mode));
            if (c != FM_OK) BAD("%s", fm_check_text(c));
        }
        j->names[i] = strdup(names[i]);
        if (!j->names[i]) BAD("OUT OF MEMORY");
        j->n_names = i + 1;
    }
    j->buf = op == FM_OP_DELETE ? NULL : malloc(FM_CHUNK);
    if (op != FM_OP_DELETE && !j->buf) BAD("OUT OF MEMORY");
#undef BAD
    j->state = FM_J_RUN;
    return 0;
}

static bool push_level(FmJob *j, const char *src, const char *dst) {
    if (j->depth >= FM_MAX_DEPTH) { job_fail(j, ENAMETOOLONG, "FOLDERS NESTED TOO DEEPLY", src); return false; }
    DIR *d = opendir(src);
    if (!d) { job_fail(j, errno, "CANNOT OPEN", src); return false; }
    FmLevel *L = &j->lv[j->depth++];
    L->d = d;
    snprintf(L->src, sizeof L->src, "%s", src);
    snprintf(L->dst, sizeof L->dst, "%s", dst ? dst : "");
    return true;
}

static void finish_level(FmJob *j) {
    FmLevel *L = &j->lv[--j->depth];
    closedir(L->d);
    L->d = NULL;
    if (j->op == FM_OP_DELETE) {
        if (rmdir(L->src) < 0) job_fail(j, errno, "CANNOT REMOVE FOLDER", L->src);
    } else if (j->op == FM_OP_MOVE) {
        /* Files left behind (skipped, failed) keep their folder: not an error. */
        if (rmdir(L->src) < 0 && errno != ENOTEMPTY && errno != EEXIST)
            job_fail(j, errno, "CANNOT REMOVE FOLDER", L->src);
    }
}

static void finish_file(FmJob *j) {
    struct timespec ts[2] = { j->cur_st.st_atim, j->cur_st.st_mtim };
    (void)fchmod(j->out_fd, j->cur_st.st_mode & 07777);      /* vfat refuses: fine */
    (void)futimens(j->out_fd, ts);
    int c = close(j->out_fd);
    int ce = errno;
    j->out_fd = -1;
    close(j->in_fd);
    j->in_fd = -1;
    if (c < 0) { unlink(j->cur_dst); job_fail(j, ce, "WRITE FAILED", j->cur_dst); return; }
    j->files_done++;
    if (j->op == FM_OP_MOVE && unlink(j->cur_src) < 0)
        job_fail(j, errno, "COPIED, BUT CANNOT REMOVE SOURCE", j->cur_src);
}

/* Copy up to `budget` bytes; returns what it charged. */
static long long copy_chunk(FmJob *j, long long budget) {
    size_t want = budget < FM_CHUNK ? (size_t)budget : FM_CHUNK;
    ssize_t r;
    do r = read(j->in_fd, j->buf, want); while (r < 0 && errno == EINTR);
    if (r < 0) { int e = errno; file_abort(j, true); job_fail(j, e, "READ FAILED", j->cur_src); return META_COST; }
    if (r == 0) { finish_file(j); return META_COST; }
    ssize_t off = 0;
    while (off < r) {
        ssize_t w = write(j->out_fd, j->buf + off, (size_t)(r - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            file_abort(j, true);
            job_fail(j, e, "WRITE FAILED", j->cur_dst);
            return META_COST;
        }
        off += w;
    }
    j->cur_done += r;
    j->bytes_done += r;
    return r;
}

/* Begin copying one non-folder.  dexists: the target is there and may be replaced. */
static void start_file(FmJob *j, const char *src, const char *dst, const struct stat *st,
                       bool dexists, bool top) {
    if (j->op == FM_OP_MOVE && top) {
        if (rename(src, dst) == 0) { j->files_done++; return; }
        if (errno != EXDEV) { job_fail(j, errno, "CANNOT MOVE", src); return; }
    }
    if (dexists && unlink(dst) < 0 && errno != ENOENT) { job_fail(j, errno, "CANNOT REPLACE", dst); return; }
    if (S_ISLNK(st->st_mode)) {
        char target[FM_PATH_MAX];
        ssize_t r = readlink(src, target, sizeof target - 1);
        if (r < 0) { job_fail(j, errno, "CANNOT READ LINK", src); return; }
        target[r] = '\0';
        if (symlink(target, dst) < 0) { job_fail(j, errno, "CANNOT CREATE LINK", dst); return; }
        j->files_done++;
        if (j->op == FM_OP_MOVE && unlink(src) < 0) job_fail(j, errno, "CANNOT REMOVE SOURCE", src);
        return;
    }
    j->in_fd = open(src, O_RDONLY | O_NOFOLLOW);
    if (j->in_fd < 0) { job_fail(j, errno, "CANNOT OPEN", src); return; }
    j->out_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, (st->st_mode & 0777) | 0200);
    if (j->out_fd < 0) { int e = errno; close(j->in_fd); j->in_fd = -1; job_fail(j, e, "CANNOT CREATE", dst); return; }
    snprintf(j->cur_src, sizeof j->cur_src, "%s", src);
    snprintf(j->cur_dst, sizeof j->cur_dst, "%s", dst);
    j->cur_st = *st;
    j->cur_done = 0;
    j->cur_total = (long long)st->st_size;
}

static void process(FmJob *j, const char *src, const char *dst, bool top) {
    struct stat st, dt;
    if (lstat(src, &st) < 0) { job_fail(j, errno, "CANNOT READ", src); return; }
    snprintf(j->cur_src, sizeof j->cur_src, "%s", src);
    if (j->op == FM_OP_DELETE) {
        if (S_ISDIR(st.st_mode)) push_level(j, src, NULL);
        else if (unlink(src) < 0) job_fail(j, errno, "CANNOT DELETE", src);
        else j->files_done++;
        return;
    }
    bool dexists = lstat(dst, &dt) == 0;
    if (S_ISDIR(st.st_mode)) {
        if (top) { j->root_dev = st.st_dev; j->root_ino = st.st_ino; j->have_root = true; }
        if (top && j->op == FM_OP_MOVE && !dexists) {
            if (rename(src, dst) == 0) { j->files_done++; return; }
            if (errno != EXDEV) { job_fail(j, errno, "CANNOT MOVE", src); return; }
        }
        if (dexists && !S_ISDIR(dt.st_mode)) { job_fail(j, EEXIST, "EXISTS AND IS NOT A FOLDER", dst); return; }
        if (dexists && j->have_root && dt.st_dev == j->root_dev && dt.st_ino == j->root_ino) {
            job_fail(j, 0, fm_check_text(FM_ERR_INTO_SELF), dst);
            return;
        }
        if (!dexists && mkdir(dst, (st.st_mode & 07777) | 0700) < 0) { job_fail(j, errno, "CANNOT CREATE FOLDER", dst); return; }
        push_level(j, src, dst);
        return;
    }
    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) { j->skipped++; return; }   /* device, fifo, socket */
    if (dexists) {
        if (dt.st_dev == st.st_dev && dt.st_ino == st.st_ino) { job_fail(j, 0, fm_check_text(FM_ERR_SAME), dst); return; }
        if (S_ISDIR(dt.st_mode)) { job_fail(j, EISDIR, "A FOLDER WITH THAT NAME EXISTS", dst); return; }
        if (j->all_skip) { j->skipped++; return; }
        if (!j->all_overwrite) {
            snprintf(j->pend_src, sizeof j->pend_src, "%s", src);
            snprintf(j->pend_dst, sizeof j->pend_dst, "%s", dst);
            j->pend_top = top;
            j->state = FM_J_CONFLICT;
            return;
        }
    }
    start_file(j, src, dst, &st, dexists, top);
}

static void next_entry(FmJob *j) {
    char src[FM_PATH_MAX], dst[FM_PATH_MAX];
    dst[0] = '\0';
    if (j->depth > 0) {
        FmLevel *L = &j->lv[j->depth - 1];
        struct dirent *de = readdir(L->d);
        if (!de) { finish_level(j); return; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) return;
        if (!fm_path_join(L->src, de->d_name, src, sizeof src) ||
            (j->op != FM_OP_DELETE && !fm_path_join(L->dst, de->d_name, dst, sizeof dst))) {
            job_fail(j, ENAMETOOLONG, "PATH TOO LONG", L->src);
            return;
        }
        process(j, src, dst, false);
    } else {
        if (j->next_name >= j->n_names) { sync(); j->state = FM_J_DONE; return; }
        const char *name = j->names[j->next_name++];
        if (!fm_path_join(j->src_dir, name, src, sizeof src) ||
            (j->op != FM_OP_DELETE && !fm_path_join(j->dst_dir, name, dst, sizeof dst))) {
            job_fail(j, ENAMETOOLONG, "PATH TOO LONG", name);
            return;
        }
        j->have_root = false;
        process(j, src, dst, true);
    }
}

FmJobState fm_job_step(FmJob *j, long long budget) {
    while (j->state == FM_J_RUN && budget > 0) {
        if (j->in_fd >= 0) { budget -= copy_chunk(j, budget); continue; }
        budget -= META_COST;
        next_entry(j);
    }
    return j->state;
}

const char *fm_job_current(const FmJob *j) {
    return j->in_fd >= 0 ? j->cur_dst : j->cur_src;
}

void fm_job_decide(FmJob *j, FmDecision d) {
    if (j->state != FM_J_CONFLICT) return;
    j->state = FM_J_RUN;
    if (d == FM_DECIDE_SKIP_ALL) j->all_skip = true;
    if (d == FM_DECIDE_OVERWRITE_ALL) j->all_overwrite = true;
    if (d == FM_DECIDE_SKIP || d == FM_DECIDE_SKIP_ALL) { j->skipped++; return; }
    struct stat st;
    if (lstat(j->pend_src, &st) < 0) { job_fail(j, errno, "CANNOT READ", j->pend_src); return; }
    start_file(j, j->pend_src, j->pend_dst, &st, true, j->pend_top);
}

void fm_job_ack_error(FmJob *j, bool cancel) {
    if (j->state != FM_J_ERROR) return;
    if (cancel) fm_job_cancel(j);
    else j->state = FM_J_RUN;
}

void fm_job_cancel(FmJob *j) {
    file_abort(j, true);
    while (j->depth > 0) { closedir(j->lv[--j->depth].d); j->lv[j->depth].d = NULL; }
    j->state = FM_J_CANCELLED;
}

void fm_job_free(FmJob *j) {
    file_abort(j, false);
    while (j->depth > 0) { closedir(j->lv[--j->depth].d); j->lv[j->depth].d = NULL; }
    if (j->names) {
        for (int i = 0; i < j->n_names; i++) free(j->names[i]);
        free(j->names);
        j->names = NULL;
    }
    free(j->buf);
    j->buf = NULL;
    j->n_names = 0;
}

/* ── OPEN ────────────────────────────────────────────────────────────────── */

static const char *const TEXT_EXT[] = {
    "txt", "md", "conf", "cfg", "ini", "log", "sh", "c", "h", "py", "json", "xml", "csv",
    "yml", "yaml", "toml", "lst", "service", "rules", "patch", "diff", "html", "css", "js",
};

/* The extension after the last dot, or NULL when there is none (a leading dot alone is not one). */
static const char *name_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) return NULL;
    return dot + 1;
}

bool fm_name_is_text(const char *name) {
    const char *x = name_ext(name);
    if (!x) return true;
    for (size_t i = 0; i < sizeof TEXT_EXT / sizeof TEXT_EXT[0]; i++)
        if (!strcasecmp(x, TEXT_EXT[i])) return true;
    return false;
}

bool fm_name_is_image(const char *name) {
    const char *x = name_ext(name);
    return x && (!strcasecmp(x, "jpg") || !strcasecmp(x, "jpeg") || !strcasecmp(x, "png"));
}

bool fm_sniff_text(const unsigned char *buf, size_t n, long long size) {
    if (size < 0 || size >= FM_TEXT_MAX) return false;
    for (size_t i = 0; i < n && i < FM_SNIFF_BYTES; i++)
        if (buf[i] == 0) return false;
    return true;
}

bool fm_open_editable(const char *path, const char *const *media, int n) {
    if (fm_path_within("/home/root/data", path)) return true;
    for (int i = 0; i < n; i++)
        if (media[i] && fm_path_within(media[i], path)) return true;
    return false;
}

bool fm_open_cmd(FmOpenCmd *c, bool image, bool edit, const char *fb, const char *touch, const char *path) {
    memset(c, 0, sizeof *c);
    if (snprintf(c->fb, sizeof c->fb, "%s", fb) >= (int)sizeof c->fb) return false;
    if (snprintf(c->touch, sizeof c->touch, "%s", touch) >= (int)sizeof c->touch) return false;
    if (!fm_path_normalise(path, c->path, sizeof c->path)) return false;
    if (!fm_path_parent(c->path, c->cwd, sizeof c->cwd)) return false;
    int n = 0;
    if (image) {
        c->av[n++] = "/opt/games/frame";
        c->av[n++] = "--fb";    c->av[n++] = c->fb;
        c->av[n++] = "--touch"; c->av[n++] = c->touch;
        c->av[n++] = "--show";  c->av[n++] = c->path;
    } else {
        c->av[n++] = "/opt/games/terminal";
        c->av[n++] = c->fb;
        c->av[n++] = c->touch;
        c->av[n++] = "--cwd";   c->av[n++] = c->cwd;
        c->av[n++] = "--exec";  c->av[n++] = "nano";
        if (!edit) c->av[n++] = "-v";
        c->av[n++] = "--";      c->av[n++] = c->path;
    }
    c->av[n] = NULL;
    c->ac = n;
    return true;
}
