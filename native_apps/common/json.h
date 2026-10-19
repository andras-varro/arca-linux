/*
 * json.h — a small JSON tokenizer: strict, allocation-free, bounded by length.
 *
 * `json_parse()` validates one complete document and fills a caller-supplied
 * token array.  Nothing is copied: a token is a byte span into the caller's text,
 * so the text must outlive the tokens.  No heap, no static state, no thread, no
 * `setlocale` dependency (the one `strtod` call in `json_num()` is fed text that
 * is already validated), and never a byte read at or past `len` — the input need
 * not be NUL-terminated, which is what an HTTP body in a fixed buffer looks like.
 *
 * Why strict: the documents come from a media server over the LAN and are
 * parsed on a 600 MHz core with 234 MB of RAM and no way to log into the box that
 * sent them.  A parser that quietly accepts `[1,]`, a leading zero or a raw
 * newline inside a string turns a server bug into a wrong track list rather than
 * an error.  The full JSON grammar is enforced: whitespace is ' ' \t \n \r only,
 * numbers are `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`, strings take no
 * raw byte below 0x20 and only the escapes \" \\ \/ \b \f \n \r \t \uXXXX, no
 * trailing comma, object keys are strings, and exactly one top-level value may
 * be followed by nothing but whitespace.
 *
 * ⚠️ **Bytes >= 0x80 inside a string are NOT validated as UTF-8.**  They pass
 * through `json_parse()` untouched and `json_str()` copies them as they are
 * (grouping a lead byte with its continuation bytes only so a truncation never
 * splits a character).  A lone \uD800 escape is likewise accepted by the parser;
 * `json_str()` turns it into U+FFFD.
 *
 * ⚠️ **A string token's span is RAW: the quotes are excluded, the escapes are
 * still escaped.**  `json_str()` is the only unescape.  Container tokens span
 * from the opening to one past the closing bracket.  `end` is always exclusive.
 *
 * Token layout.  Tokens are stored in document order, depth first, so a subtree is
 * contiguous and `next` (the index of the first token AFTER the subtree) skips it
 * in O(1) — no walk, no recursion:
 *   - an array is its own token, then its elements' subtrees one after another;
 *   - an object is its own token, then for each pair the KEY (a plain STRING
 *     token, no children), then the value's subtree.
 * Object iteration is therefore done by hand: `i = obj + 1`, the key is `t[i]`,
 * the value is `t[i + 1]`, the next pair starts at `t[i + 1].next`, and there are
 * `t[obj].size` pairs.  Array iteration has helpers below.
 *
 * Error priority: the first problem met while reading left to right wins.  A
 * document that ends inside a value is JSON_ERR_PART, never JSON_ERR_INVAL, so a
 * caller can tell "the body was cut short" from "the server sent garbage".  The
 * tests parse every prefix of a document and require PART from each.
 *
 * Nesting is capped at JSON_MAX_DEPTH containers (recursive descent: the stack
 * cost is bounded and small, which an input-controlled depth would not be).
 *
 * The helpers take the token array and its count and treat any index outside
 * [0, n) or of the wrong type as "absent" (-1 / false): a lookup on a missing
 * field is a normal case for optional Jellyfin fields, not a fault.
 */

#ifndef ARCA_JSON_H
#define ARCA_JSON_H

#include <stdbool.h>
#include <stddef.h>

#define JSON_MAX_DEPTH 32   /**< 32 nested containers parse; the 33rd is JSON_ERR_DEPTH */

typedef enum {
    JSON_UNDEF = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_BOOL,
    JSON_NULL
} JsonType;

typedef struct {
    JsonType type;
    int start;   /**< first byte of the span (a string: first byte after the quote) */
    int end;     /**< one past the last byte (a string: the closing quote's offset)  */
    int size;    /**< array: element count; object: key/value pair count; else 0     */
    int next;    /**< index of the first token after this token's subtree            */
} JsonTok;

enum {
    JSON_ERR_INVAL = -1,   /**< malformed, or trailing garbage, or a bad argument       */
    JSON_ERR_NOMEM = -2,   /**< the document needs more tokens than `max_toks`          */
    JSON_ERR_DEPTH = -3,   /**< more than JSON_MAX_DEPTH nested containers              */
    JSON_ERR_PART  = -4    /**< the input ended early (truncated), or is empty          */
};

/**
 * Parse one JSON document.  Returns the token count (>= 1; token 0 is the root)
 * or a negative JSON_ERR_*.  Writes only toks[0 .. max_toks-1].  `s == NULL`, a
 * `len` above INT_MAX (offsets are int), a negative `max_toks` or a NULL `toks`
 * with `max_toks > 0` give JSON_ERR_INVAL.  `toks` may be NULL with `max_toks == 0`
 * to learn whether the document is valid (a valid one then reports NOMEM).
 * On any error the contents of `toks` are unspecified.
 */
int json_parse(const char *s, size_t len, JsonTok *toks, int max_toks);

/**
 * The token count `json_parse()` would return for this text, or the same
 * negative JSON_ERR_* code.  Stores nothing, so after JSON_ERR_NOMEM a caller
 * sizes its array with `json_count()` and parses again: the count is the budget
 * json_parse() needs, and one less is NOMEM.
 */
int json_count(const char *s, size_t len);

/* ── lookup and iteration ───────────────────────────────────────────────── */

/**
 * Index of the VALUE token for `key` in object token `obj`, or -1 (not an object,
 * bad index, key absent).  The first of duplicate keys wins.  ⚠️ The compare is
 * EXACT RAW BYTES against the key token's span — no unescape, so it is O(1) per key
 * with no buffer; a key the server wrote WITH escapes (N followed by
 * the six characters backslash-u-0-0-6-1, then "me") will not match "Name".  Real API field names are plain ASCII.  `s` is the document text.
 */
int json_obj_get(const char *s, const JsonTok *t, int n, int obj, const char *key);

/** First element of array token `arr`, or -1 (empty, not an array, bad index). */
int json_arr_first(const JsonTok *t, int n, int arr);

/** The sibling after element `elem` of array `arr`, or -1 at the end or on bad input. */
int json_arr_next(const JsonTok *t, int n, int arr, int elem);

/**
 * The `i`-th element (0-based) of array `arr`, or -1 when out of range.  O(i): it
 * walks from the first element, so a loop over an array uses json_arr_first() and
 * json_arr_next() instead.
 */
int json_arr_get(const JsonTok *t, int n, int arr, int i);

/* ── values ─────────────────────────────────────────────────────────────── */

/**
 * Unescape a STRING token into `out` as UTF-8.  Snprintf-like: the return value is
 * the FULL length the string needs (without the NUL), so truncation happened iff
 * the return is >= `out_size`.  When `out_size > 0` the result is ALWAYS
 * NUL-terminated.  `out` may be NULL with `out_size == 0` to measure.  Output is
 * cut only between whole characters: once one does not fit, nothing after it is
 * written either (a short later character never leapfrogs a longer earlier one),
 * and the count carries on.
 *
 * \uXXXX becomes 1-3 UTF-8 bytes; a valid high+low pair (D83D then DE00) becomes
 * one 4-byte sequence; a lone or misordered surrogate becomes U+FFFD; so does
 * \u0000, which keeps the result a C string.  Returns -1 (and an empty `out`) when
 * the token is not a STRING or holds an invalid escape.  The token must come from
 * json_parse() of the same text: the span is trusted, it cannot be re-checked
 * against a length that this call is not given.
 */
int json_str(const char *s, const JsonTok *t, char *out, size_t out_size);

/**
 * A NUMBER token as a double, via `strtod` on a bounded stack copy (a token longer
 * than 63 bytes is refused).  ⚠️ `strtod` follows the C locale only because no
 * app calls `setlocale`; a caller that does would break the decimal point.
 */
bool json_num(const char *s, const JsonTok *t, double *out);

/**
 * A NUMBER token as an exact 64-bit integer.  False for a fraction or exponent
 * (`12.0` and `1e3` are refused even though they are whole) and for a value outside
 * long long.  ⚠️ This is the call for tick counts: RunTimeTicks is ~1e11 and a
 * double would also hold it, but a 64-bit id would not survive one.
 */
bool json_i64(const char *s, const JsonTok *t, long long *out);

/** A BOOL token as a bool; false for any other token type. */
bool json_bool(const char *s, const JsonTok *t, bool *out);

/** True iff `t` is a NULL token (a NULL pointer is not null, it is absent: false). */
bool json_is_null(const JsonTok *t);

#endif /* ARCA_JSON_H */
