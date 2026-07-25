/*
 * czo.h -- Canonicalizer Zero (CZO)
 *
 * Phase 0 gate: one identity, one representation, one lineage.
 *
 * CZO turns a byte string into (a) a canonical byte string, (b) a stable
 * 64-bit digest of that canonical form, (c) a 64-bit *skeleton* digest that
 * folds visually confusable characters, and (d) a lineage bitmask recording
 * exactly which transforms fired.
 *
 * CZO decides nothing about relationships between records. It has no corpus,
 * no index, and no memory. Deciding whether two canonical forms constitute a
 * duplicate, a collision, or a coincidence is out of scope by construction.
 *
 * Freestanding C99. No allocation, no I/O, no locale, no dependencies.
 * Deterministic: identical input bytes -> identical output bytes, forever.
 */

#ifndef CZO_H
#define CZO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CZO_VERSION       "0.1.0"
#define CZO_SPEC_VERSION  1u        /* bump invalidates all stored digests */

#define CZO_MAX_INPUT  4096
#define CZO_MAX_CANON  8192         /* folds may expand: ss, ae, th */

/* ------------------------------------------------------------------ */
/* Verdict                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CZO_OK        = 0,  /* canonical form is authoritative; pass downstream */
    CZO_AMBIGUOUS = 1,  /* canonical form emitted, but lossy in a way that
                           may be semantic; route to quarantine, not to the
                           engine */
    CZO_REJECT    = 2   /* no canonical form exists; canon[] is empty */
} czo_verdict;

typedef enum {
    CZO_R_NONE = 0,
    CZO_R_EMPTY,                 /* nothing left after canonicalization   */
    CZO_R_TOO_LONG,              /* exceeds CZO_MAX_INPUT / CZO_MAX_CANON */
    CZO_R_INVALID_UTF8,
    CZO_R_NONPRINTABLE,          /* C0/C1 control that is not whitespace  */
    CZO_R_MIXED_SCRIPT,          /* Latin + Cyrillic in one token, etc.   */
    CZO_R_LEADING_ZERO_NUMERIC   /* "0042" -> "42": length was discarded  */
} czo_reason;

/* ------------------------------------------------------------------ */
/* Lineage: which transforms fired                                     */
/* ------------------------------------------------------------------ */

enum {
    CZO_T_NONE           = 0u,
    CZO_T_TRIM           = 1u << 0,  /* leading/trailing whitespace removed  */
    CZO_T_WS_NORMALIZE   = 1u << 1,  /* exotic space -> U+0020               */
    CZO_T_WS_COLLAPSE    = 1u << 2,  /* runs of space -> single space        */
    CZO_T_ZW_STRIP       = 1u << 3,  /* zero-width, BOM, soft hyphen dropped */
    CZO_T_CTRL_STRIP     = 1u << 4,  /* tab/newline folded to space          */
    CZO_T_COMPAT_FOLD    = 1u << 5,  /* fullwidth, smart quotes, en/em dash  */
    CZO_T_DIACRITIC_FOLD = 1u << 6,  /* e-acute -> e; combining marks dropped */
    CZO_T_CASE_FOLD      = 1u << 7,  /* ASCII A-Z -> a-z                     */
    CZO_T_SEP_STRIP      = 1u << 8,  /* - _ . / : space removed inside an ID */
    CZO_T_ZERO_STRIP     = 1u << 9,  /* leading zeros removed from a number  */
    CZO_T_SKELETON_DIFF  = 1u << 10  /* skeleton != canon: confusables present */
};

/* ------------------------------------------------------------------ */
/* Field class                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    CZO_CLASS_AUTO = 0,  /* infer from shape                              */
    CZO_CLASS_ID,        /* machine identifier: separators are noise       */
    CZO_CLASS_NAME,      /* human/vendor name: word boundaries are signal  */
    CZO_CLASS_NUMERIC    /* pure quantity/identifier: digits only survive  */
} czo_class;

/* ------------------------------------------------------------------ */
/* Result                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    czo_verdict verdict;
    czo_reason  reason;
    czo_class   resolved_class;  /* what AUTO resolved to */
    uint32_t    transforms;      /* CZO_T_* bitmask -- the lineage record */
    uint64_t    digest;          /* FNV-1a-64 over canon                  */
    uint64_t    skeleton;        /* FNV-1a-64 over the confusable-folded form */
    size_t      canon_len;
    char        canon[CZO_MAX_CANON];
} czo_result;

/*
 * Canonicalize `len` bytes at `input` under `cls`, writing to `*out`.
 *
 * Returns 0 if out->verdict is CZO_OK or CZO_AMBIGUOUS, -1 on CZO_REJECT.
 * `out` is always fully initialized, including on rejection.
 * `input` may be NULL only if `len` is 0. `out` must not be NULL.
 *
 * Two records with equal `digest` have the same canonical identity.
 * Two records with unequal `digest` but equal `skeleton` are homograph
 * candidates -- CZO reports the fact and takes no position on it.
 */
int czo_canonicalize(const char *input, size_t len,
                     czo_class cls, czo_result *out);

/* Convenience wrapper for NUL-terminated input. */
int czo_canonicalize_str(const char *input, czo_class cls, czo_result *out);

/* Human-readable renderings. All return static storage or fill `buf`. */
const char *czo_verdict_str(czo_verdict v);
const char *czo_reason_str(czo_reason r);
const char *czo_class_str(czo_class c);

/* Writes a comma-separated transform list into buf; returns bytes needed
 * (excluding NUL), as snprintf does. Never writes past buflen. */
size_t czo_transforms_str(uint32_t transforms, char *buf, size_t buflen);

/* Runs the built-in vector suite. Returns count of failures (0 == pass). */
int czo_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* CZO_H */
