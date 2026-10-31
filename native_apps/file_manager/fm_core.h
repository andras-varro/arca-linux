/*
 * fm_core — the file manager's logic with no screen in it.
 *
 * Paths and names, size text, sort order, directory listing, the parser for
 * the USB helper's output, and the copy / move / delete job as a state machine
 * the main loop advances in small steps.  Nothing here draws or reads input,
 * so tests/file_manager_test.c links this file alone and drives all of it
 * against host temp directories.
 *
 * The job never follows a symlink (lstat and O_NOFOLLOW throughout; a link is
 * copied as a link), refuses a copy of a folder into itself, and keeps the
 * stack of open folders in the job so a deep tree costs no C recursion.
 */
#ifndef FM_CORE_H
#define FM_CORE_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#define FM_PATH_MAX    512
#define FM_NAME_MAX    256
#define FM_MAX_ENTRIES 4000     /* a folder shows at most this many; fm_dir_read flags the cut */
#define FM_CHUNK       65536    /* bytes copied per read/write */
#define FM_MAX_DEPTH   20       /* folders open at once in a job */

/* ── Paths and names (pure) ──────────────────────────────────────────────── */

/* Absolute paths only.  Collapses "//", "." and ".."; no trailing slash except
 * "/".  ".." above "/" stays at "/".  False: not absolute, or it does not fit. */
bool fm_path_normalise(const char *in, char *out, size_t n);
/* dir + "/" + name, normalised.  `name` must be a plain component (fm_name_ok). */
bool fm_path_join(const char *dir, const char *name, char *out, size_t n);
/* The parent of a normalised path.  False for "/" or a relative path. */
bool fm_path_parent(const char *path, char *out, size_t n);
/* True when `path` is `root` or lies below it, compared by whole components
 * after normalising both ("/a/bc" is not within "/a/b"). */
bool fm_path_within(const char *root, const char *path);
/* True for /proc, /sys and /dev, or anything below one (whole components). */
bool fm_path_pseudo(const char *path);
/* True for "/", any direct child of "/", anything pseudo, and any path that
 * will not normalise (fail closed). */
bool fm_path_protected(const char *path);
/* A name a user may give a new file or folder: 1..255 bytes, no '/', no control
 * character, not "." or "..". */
bool fm_name_ok(const char *name);
/* The text after the last '/' (the whole string when there is none). */
const char *fm_base(const char *path);
/* Only [A-Za-z0-9_./-]: safe to hand to a shell word without quoting. */
bool fm_path_shell_safe(const char *path);

typedef enum { FM_OK = 0, FM_ERR_SAME, FM_ERR_INTO_SELF, FM_ERR_NAME, FM_ERR_PATH } FmCheck;
/* May `name` in src_dir be copied or moved into dst_dir?  SAME: the destination
 * path is the source path.  INTO_SELF: a folder into itself or below itself. */
FmCheck fm_check_transfer(const char *src_dir, const char *name, const char *dst_dir,
                          bool src_is_dir);
const char *fm_check_text(FmCheck c);

/* ── Text ────────────────────────────────────────────────────────────────── */

/* "812B", "1.5K", "15K", "999K", "1.5M", "2.0G": at most 5 characters. */
void fm_format_size(long long bytes, char *out, size_t n);
/* ── Directory listing and sorting ───────────────────────────────────────── */

typedef enum { FM_T_FILE, FM_T_DIR, FM_T_LINK, FM_T_OTHER } FmType;
typedef struct FmEntry {
    char      *name;    /* malloc'd; case kept */
    FmType     type;    /* from lstat: a link is FM_T_LINK whatever it points at */
    long long  size;
    long long  mtime;
    unsigned   mode;      /* st_mode from the same lstat */
    bool       link_dir;  /* FM_T_LINK whose target (stat) is a directory */
} FmEntry;

typedef enum { FM_SORT_NAME, FM_SORT_SIZE, FM_SORT_DATE, FM_SORT_COUNT } FmSort;
const char *fm_sort_label(FmSort s);   /* "NAME" / "SIZE" / "DATE" */
/* Folders before everything else, always.  Then name (case-insensitive, bytes
 * break a tie), size (largest first) or date (newest first); name breaks ties. */
int fm_entry_cmp(const FmEntry *a, const FmEntry *b, FmSort s);

typedef struct { FmEntry *e; int n; bool truncated; } FmDir;
/* Read `path` (without "." and ".."), sorted.  0, or -errno. */
int  fm_dir_read(const char *path, FmSort sort, FmDir *out);
void fm_dir_sort(FmDir *d, FmSort sort);
int  fm_dir_find(const FmDir *d, const char *name);   /* index or -1 */
void fm_dir_free(FmDir *d);
/* The detail text of an entry: size for a file, "<DIR>" for a folder. */
void fm_entry_detail(const FmEntry *e, char *out, size_t n);
/* "<mode> <size> <YYYY-MM-DD HH:MM>", e.g. "-rw-r--r-- 12.3K 2026-10-18 14:08";
 * the time is localtime of mtime.  Always NUL-terminated, truncated at n. */
void fm_entry_detail_long(const FmEntry *e, char *out, size_t n);
/* ls-style mode text: type character, then rwxrwxrwx with s/S, s/S, t/T for
 * setuid, setgid and sticky.  out holds 10 characters and a NUL. */
void fm_mode_string(unsigned mode, char out[11]);
/* Free and total bytes of the filesystem holding `path`. */
bool fm_space(const char *path, long long *free_bytes, long long *total_bytes);
/* realpath(path): 0 and the canonical target in out when it is a directory;
 * otherwise -errno (-ENOTDIR when it is not a directory, -ENAMETOOLONG when
 * it does not fit in n). */
int fm_link_dir_target(const char *path, char *out, size_t n);

/* ── The USB helper's output (pure) ──────────────────────────────────────── */

typedef enum {
    FM_ST_NONE = 0,     /* a line the contract does not know, or a bad field */
    FM_ST_MOUNTED, FM_ST_ALREADY, FM_ST_UNSUPPORTED, FM_ST_FAILED,
    FM_ST_GONE, FM_ST_EJECTED, FM_ST_BUSY
} FmStKind;
typedef struct {
    FmStKind kind;
    char     mp[128];      /* MOUNTED ALREADY GONE EJECTED BUSY */
    char     dev[64];      /* MOUNTED ALREADY UNSUPPORTED FAILED */
    char     extra[96];    /* UNSUPPORTED: the type; FAILED: the reason (rest of line) */
} FmStLine;
/* One line of `arca-storage`.  A mount point must be absolute, normalised, not
 * "/" and shell-safe; a device must start with "/dev/".  Anything else is
 * FM_ST_NONE, so a root can never be made from a hostile or garbled line. */
FmStLine fm_parse_storage_line(const char *line);

/* ── Single operations ───────────────────────────────────────────────────── */

/* Rename dir/old to dir/new.  0, or an errno: EINVAL for a bad name, EEXIST when
 * the target exists (rename(2) would silently replace a file). */
int fm_rename(const char *dir, const char *old_name, const char *new_name);
int fm_mkdir(const char *dir, const char *name);

/* ── Copy / move / delete as a job ───────────────────────────────────────── */

typedef enum { FM_OP_COPY, FM_OP_MOVE, FM_OP_DELETE } FmOp;
typedef enum {
    FM_J_RUN,         /* fm_job_step has more to do */
    FM_J_CONFLICT,    /* the target exists: answer with fm_job_decide() */
    FM_J_ERROR,       /* answer with fm_job_ack_error() */
    FM_J_DONE,
    FM_J_CANCELLED
} FmJobState;
typedef enum { FM_DECIDE_OVERWRITE, FM_DECIDE_SKIP, FM_DECIDE_OVERWRITE_ALL, FM_DECIDE_SKIP_ALL } FmDecision;

typedef struct { DIR *d; char src[FM_PATH_MAX]; char dst[FM_PATH_MAX]; } FmLevel;

typedef struct {
    FmOp       op;
    FmJobState state;
    char       src_dir[FM_PATH_MAX], dst_dir[FM_PATH_MAX];
    char     **names;
    int        n_names, next_name;
    FmLevel    lv[FM_MAX_DEPTH];
    int        depth;
    /* the file being copied */
    int        in_fd, out_fd;
    char       cur_src[FM_PATH_MAX], cur_dst[FM_PATH_MAX];
    long long  cur_done, cur_total;
    struct stat cur_st;
    unsigned char *buf;
    /* the top-level folder being walked, to catch a destination that is it */
    dev_t      root_dev;
    ino_t      root_ino;
    bool       have_root;
    /* totals */
    long long  bytes_done;
    int        files_done, skipped, errors;
    bool       all_overwrite, all_skip;
    /* a conflict waiting for an answer */
    char       pend_src[FM_PATH_MAX], pend_dst[FM_PATH_MAX];
    bool       pend_top;
    /* the last error */
    int        err_no;
    bool       err_fatal;       /* retrying the next item is pointless (no space, I/O) */
    char       err_msg[FM_PATH_MAX + 64];
} FmJob;

/* Start a job over names[0..n) of src_dir into dst_dir (NULL for FM_OP_DELETE).
 * 0, or -1 with a sentence in err: a bad name, the destination not a folder,
 * a folder into itself, source and destination the same. */
int  fm_job_init(FmJob *j, FmOp op, const char *src_dir, const char *dst_dir,
                 const char *const *names, int n, char *err, size_t errn);
/* Do up to `budget` bytes of work (a file operation costs 2048).  Returns the
 * state; FM_J_RUN means call again. */
FmJobState fm_job_step(FmJob *j, long long budget);
/* The path being worked on, for a progress line. */
const char *fm_job_current(const FmJob *j);
/* Answer FM_J_CONFLICT. */
void fm_job_decide(FmJob *j, FmDecision d);
/* Answer FM_J_ERROR: skip the item and go on, or cancel the job. */
void fm_job_ack_error(FmJob *j, bool cancel);
/* Stop: closes everything and removes the half-written target file.  Items
 * already moved or deleted stay so. */
void fm_job_cancel(FmJob *j);
void fm_job_free(FmJob *j);

/* ── OPEN: what a file is, where it may be edited, the command that shows it ─ */

#define FM_TEXT_MAX    (1024 * 1024)   /* a text file is opened only below this size */
#define FM_SNIFF_BYTES 4096            /* leading bytes read to look for a NUL */

/* A name nano is offered for: a known text extension (.txt .md .conf .cfg .ini .log
 * .sh .c .h .py .json .xml .csv .yml .yaml .toml .lst .service .rules .patch .diff
 * .html .css .js, case-insensitive), or no extension at all ("os-release", "fstab").
 * A dotfile with no further dot counts as no extension.  False for any other
 * extension; the content test (fm_sniff_text) is a second, separate condition. */
bool fm_name_is_text(const char *name);
/* .jpg .jpeg .png, case-insensitive. */
bool fm_name_is_image(const char *name);
/* The EDIT button applies: any regular file (not a link, directory or device); the simple/expert view decides what is reachable.
 * The content sniff stays with the caller, as for OPEN. */
bool fm_edit_ok(FmType type, const char *name);
/* The first `n` bytes of a file of `size` bytes: text when size < FM_TEXT_MAX and
 * none of the bytes is NUL.  An empty file is text. */
bool fm_sniff_text(const unsigned char *buf, size_t n, long long size);
/* May `path` be edited?  Below /home/root/data, or below one of the `n` removable
 * mount points in `media`; whole-component test.  Everything else is view only. */
bool fm_open_editable(const char *path, const char *const *media, int n);

typedef struct {
    char        fb[128], touch[128];
    char        cwd[FM_PATH_MAX], path[FM_PATH_MAX];
    const char *av[16];       /* NULL-terminated; points into this struct, so do not copy it */
    int         ac;
} FmOpenCmd;
/* The argv for opening `path` (absolute, normalised).  image: frame --show;
 * else terminal --exec nano, with -v unless `edit`.  False when the path does not
 * fit or has no directory part. */
bool fm_open_cmd(FmOpenCmd *c, bool image, bool edit, const char *fb, const char *touch, const char *path);

#endif /* FM_CORE_H */
