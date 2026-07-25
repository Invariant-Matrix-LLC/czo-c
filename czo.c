/*
 * czo.c -- Canonicalizer Zero implementation.
 *
 * Pipeline, in fixed order. The order is part of the contract; changing it
 * changes digests and requires a CZO_SPEC_VERSION bump.
 *
 *   1. length gate
 *   2. UTF-8 decode + validate + script census (on ORIGINAL codepoints)
 *   3. per-codepoint fold: drop invisibles, normalize spaces, compat-fold,
 *      diacritic-fold to ASCII
 *   4. mixed-script adjudication
 *   5. trim + collapse whitespace
 *   6. ASCII case fold
 *   7. class resolution and class-specific reduction
 *   8. emptiness gate
 *   9. encode, digest
 *  10. skeleton fold, skeleton digest
 */

#include "czo.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* FNV-1a 64                                                           */
/* ------------------------------------------------------------------ */

#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME  1099511628211ULL

static uint64_t fnv1a64(const unsigned char *p, size_t n)
{
    uint64_t h = FNV64_OFFSET;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

static int u8_decode(const unsigned char *s, size_t n, size_t i,
                     uint32_t *cp, size_t *adv)
{
    unsigned char c = s[i];

    if (c < 0x80) { *cp = c; *adv = 1; return 0; }

    if ((c & 0xE0) == 0xC0) {
        if (i + 1 >= n || (s[i+1] & 0xC0) != 0x80) return -1;
        *cp = ((uint32_t)(c & 0x1Fu) << 6) | (uint32_t)(s[i+1] & 0x3Fu);
        if (*cp < 0x80u) return -1;                 /* overlong */
        *adv = 2; return 0;
    }

    if ((c & 0xF0) == 0xE0) {
        if (i + 2 >= n) return -1;
        if ((s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80) return -1;
        *cp = ((uint32_t)(c & 0x0Fu) << 12)
            | ((uint32_t)(s[i+1] & 0x3Fu) << 6)
            |  (uint32_t)(s[i+2] & 0x3Fu);
        if (*cp < 0x800u) return -1;                /* overlong */
        if (*cp >= 0xD800u && *cp <= 0xDFFFu) return -1;  /* surrogate */
        *adv = 3; return 0;
    }

    if ((c & 0xF8) == 0xF0) {
        if (i + 3 >= n) return -1;
        if ((s[i+1] & 0xC0) != 0x80 || (s[i+2] & 0xC0) != 0x80
                                    || (s[i+3] & 0xC0) != 0x80) return -1;
        *cp = ((uint32_t)(c & 0x07u) << 18)
            | ((uint32_t)(s[i+1] & 0x3Fu) << 12)
            | ((uint32_t)(s[i+2] & 0x3Fu) << 6)
            |  (uint32_t)(s[i+3] & 0x3Fu);
        if (*cp < 0x10000u || *cp > 0x10FFFFu) return -1;
        *adv = 4; return 0;
    }

    return -1;
}

static size_t u8_encode(uint32_t cp, unsigned char *out)
{
    if (cp < 0x80u) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (unsigned char)(0xC0u | (cp >> 6));
        out[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (unsigned char)(0xE0u | (cp >> 12));
        out[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (unsigned char)(0xF0u | (cp >> 18));
    out[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ------------------------------------------------------------------ */
/* Character classes                                                   */
/* ------------------------------------------------------------------ */

static int cp_is_space(uint32_t c)
{
    return c == 0x09u || c == 0x0Au || c == 0x0Bu || c == 0x0Cu
        || c == 0x0Du || c == 0x20u || c == 0x85u || c == 0xA0u
        || c == 0x1680u || (c >= 0x2000u && c <= 0x200Au)
        || c == 0x2028u || c == 0x2029u || c == 0x202Fu
        || c == 0x205Fu || c == 0x3000u;
}

/* Invisible formatting: carries no identity, always dropped. */
static int cp_is_invisible(uint32_t c)
{
    return c == 0x00ADu || c == 0x180Eu
        || (c >= 0x200Bu && c <= 0x200Fu)
        || (c >= 0x202Au && c <= 0x202Eu)
        || (c >= 0x2060u && c <= 0x2064u)
        || (c >= 0x2066u && c <= 0x206Fu)
        || c == 0xFEFFu
        || (c >= 0xFFF9u && c <= 0xFFFBu);
}

static int cp_is_combining(uint32_t c)
{
    return (c >= 0x0300u && c <= 0x036Fu)
        || (c >= 0x1AB0u && c <= 0x1AFFu)
        || (c >= 0x1DC0u && c <= 0x1DFFu)
        || (c >= 0x20D0u && c <= 0x20F0u)
        || (c >= 0xFE00u && c <= 0xFE0Fu)
        || (c >= 0xFE20u && c <= 0xFE2Fu);
}

/* Non-whitespace control: no canonical form exists. */
static int cp_is_bad_control(uint32_t c)
{
    if (cp_is_space(c)) return 0;
    if (c < 0x20u) return 1;
    if (c == 0x7Fu) return 1;
    if (c >= 0x80u && c <= 0x9Fu) return 1;
    return 0;
}

#define SCR_NEUTRAL  0
#define SCR_LATIN    1
#define SCR_GREEK    2
#define SCR_CYRILLIC 3
#define SCR_OTHER    4

static int script_of(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return SCR_LATIN;
    if (c < 0x80u) return SCR_NEUTRAL;
    if (cp_is_space(c) || cp_is_invisible(c) || cp_is_combining(c))
        return SCR_NEUTRAL;
    if (c >= 0x2000u && c <= 0x2BFFu) return SCR_NEUTRAL; /* punct/symbols */
    if (c == 0xD7u || c == 0xF7u)     return SCR_NEUTRAL;
    if ((c >= 0xC0u && c <= 0x24Fu) || (c >= 0x1E00u && c <= 0x1EFFu))
        return SCR_LATIN;
    if ((c >= 0x0370u && c <= 0x03FFu) || (c >= 0x1F00u && c <= 0x1FFFu))
        return SCR_GREEK;
    if (c >= 0x0400u && c <= 0x052Fu) return SCR_CYRILLIC;
    return SCR_OTHER;
}

/* ------------------------------------------------------------------ */
/* Fold tables                                                         */
/* ------------------------------------------------------------------ */

/* U+00C0 .. U+00FF -> ASCII. NULL means "leave alone". */
static const char *const L1FOLD[64] = {
    "A","A","A","A","A","A","AE","C",
    "E","E","E","E","I","I","I","I",
    "D","N","O","O","O","O","O",NULL,
    "O","U","U","U","U","Y","TH","ss",
    "a","a","a","a","a","a","ae","c",
    "e","e","e","e","i","i","i","i",
    "d","n","o","o","o","o","o",NULL,
    "o","u","u","u","u","y","th","y"
};

/* U+0100 .. U+017F -> single ASCII letter (approximate; ligatures collapse
 * to their first letter, which is documented and deliberate). */
static const char LAFOLD[129] =
    "AaAaAa"           /* 0100-0105 */
    "CcCcCcCc"         /* 0106-010D */
    "DdDd"             /* 010E-0111 */
    "EeEeEeEeEe"       /* 0112-011B */
    "GgGgGgGg"         /* 011C-0123 */
    "HhHh"             /* 0124-0127 */
    "IiIiIiIiIi"       /* 0128-0131 */
    "Ii"               /* 0132-0133 */
    "Jj"               /* 0134-0135 */
    "Kkk"              /* 0136-0138 */
    "LlLlLlLlLl"       /* 0139-0142 */
    "NnNnNnn"          /* 0143-0149 */
    "Nn"               /* 014A-014B */
    "OoOoOoOo"         /* 014C-0153 */
    "RrRrRr"           /* 0154-0159 */
    "SsSsSsSs"         /* 015A-0161 */
    "TtTtTt"           /* 0162-0167 */
    "UuUuUuUuUuUu"     /* 0168-0173 */
    "Ww"               /* 0174-0175 */
    "YyY"              /* 0176-0178 */
    "ZzZzZz"           /* 0179-017E */
    "s";               /* 017F      */

/* Confusable skeleton map: visually Latin-looking non-Latin letters. */
typedef struct { uint32_t cp; char to; } confusable;

static const confusable CONFUSABLES[] = {
    /* Cyrillic lowercase */
    {0x0430,'a'},{0x0432,'b'},{0x0435,'e'},{0x0437,'3'},{0x043A,'k'},
    {0x043C,'m'},{0x043D,'h'},{0x043E,'o'},{0x0440,'p'},{0x0441,'c'},
    {0x0442,'t'},{0x0443,'y'},{0x0445,'x'},{0x0450,'e'},{0x0451,'e'},
    {0x0455,'s'},{0x0456,'i'},{0x0457,'i'},{0x0458,'j'},{0x04BB,'h'},
    {0x04CF,'l'},{0x0501,'d'},{0x051B,'q'},{0x051D,'w'},
    /* Cyrillic uppercase */
    {0x0410,'a'},{0x0412,'b'},{0x0415,'e'},{0x0417,'3'},{0x041A,'k'},
    {0x041C,'m'},{0x041D,'h'},{0x041E,'o'},{0x0420,'p'},{0x0421,'c'},
    {0x0422,'t'},{0x0423,'y'},{0x0425,'x'},{0x0405,'s'},{0x0406,'i'},
    {0x0408,'j'},
    /* Greek lowercase */
    {0x03B1,'a'},{0x03B2,'b'},{0x03B5,'e'},{0x03B7,'n'},{0x03B9,'i'},
    {0x03BA,'k'},{0x03BD,'v'},{0x03BF,'o'},{0x03C1,'p'},{0x03C4,'t'},
    {0x03C5,'u'},{0x03C7,'x'},
    /* Greek uppercase */
    {0x0391,'a'},{0x0392,'b'},{0x0395,'e'},{0x0396,'z'},{0x0397,'h'},
    {0x0399,'i'},{0x039A,'k'},{0x039C,'m'},{0x039D,'n'},{0x039F,'o'},
    {0x03A1,'p'},{0x03A4,'t'},{0x03A5,'y'},{0x03A7,'x'}
};

#define N_CONFUSABLES (sizeof(CONFUSABLES) / sizeof(CONFUSABLES[0]))

static char confusable_of(uint32_t cp)
{
    size_t i;
    for (i = 0; i < N_CONFUSABLES; i++)
        if (CONFUSABLES[i].cp == cp) return CONFUSABLES[i].to;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

/*
 * Simple (1:1, locale-independent) case folding for the three scripts CZO
 * can be confident about. Everything else is left as-is rather than folded
 * incorrectly -- see README, "What CZO does not do".
 */
static uint32_t cp_case_fold(uint32_t c)
{
    if (c >= 'A' && c <= 'Z')             return c + 32u;   /* ASCII       */
    if (c >= 0x0391u && c <= 0x03A1u)     return c + 32u;   /* Greek A-P   */
    if (c >= 0x03A3u && c <= 0x03ABu)     return c + 32u;   /* Greek S-Y   */
    if (c == 0x03C2u)                     return 0x03C3u;   /* final sigma */
    if (c == 0x0386u)                     return 0x03ACu;   /* accented A  */
    if (c >= 0x0388u && c <= 0x038Au)     return c + 37u;   /* accented EHI*/
    if (c == 0x038Cu)                     return 0x03CCu;
    if (c >= 0x038Eu && c <= 0x038Fu)     return c + 63u;
    if (c >= 0x0400u && c <= 0x040Fu)     return c + 80u;   /* Cyrillic ext*/
    if (c >= 0x0410u && c <= 0x042Fu)     return c + 32u;   /* Cyrillic    */
    if (c >= 0x0460u && c <= 0x0481u && (c & 1u) == 0u) return c + 1u;
    if (c >= 0x048Au && c <= 0x052Fu && (c & 1u) == 0u) return c + 1u;
    return c;
}

static int is_id_separator(uint32_t c)
{
    return c == '-' || c == '_' || c == '.' || c == '/'
        || c == ':' || c == ' ' || c == '\\' || c == '#';
}

/* Punctuation dropped outright from names. */
static int is_name_noise(uint32_t c)
{
    return c == '.' || c == ',' || c == '\'' || c == '"' || c == '('
        || c == ')' || c == '[' || c == ']' || c == '&' || c == '#'
        || c == '*' || c == '!' || c == '?' || c == ';' || c == ':';
}

/* Punctuation that acts as a word boundary in names. */
static int is_name_break(uint32_t c)
{
    return c == '-' || c == '_' || c == '/' || c == '\\' || c == '|'
        || c == '+' || c == '@';
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

static void result_init(czo_result *out)
{
    memset(out, 0, sizeof(*out));
    out->verdict        = CZO_OK;
    out->reason         = CZO_R_NONE;
    out->resolved_class = CZO_CLASS_AUTO;
    out->transforms     = CZO_T_NONE;
    out->digest         = 0;
    out->skeleton       = 0;
    out->canon_len      = 0;
    out->canon[0]       = '\0';
}

static int reject(czo_result *out, czo_reason r)
{
    /* rejection overrides any pending ambiguity, but lineage is preserved */
    uint32_t keep_t = out->transforms;
    result_init(out);
    out->transforms = keep_t;
    out->verdict    = CZO_REJECT;
    out->reason     = r;
    return -1;
}

int czo_canonicalize(const char *input, size_t len,
                     czo_class cls, czo_result *out)
{
    static const uint32_t EMPTY[1] = {0};
    const unsigned char *in = (const unsigned char *)input;

    uint32_t buf[CZO_MAX_CANON];
    size_t   m = 0;
    size_t   i, j, w;
    uint32_t scripts = 0;
    uint32_t tf = CZO_T_NONE;
    czo_verdict verdict = CZO_OK;
    czo_reason  reason  = CZO_R_NONE;
    unsigned char enc[CZO_MAX_CANON];
    size_t enc_len = 0;
    unsigned char skel[CZO_MAX_CANON];
    size_t skel_len = 0;
    int confusable_hits = 0;

    if (out == NULL) return -1;
    result_init(out);
    if (in == NULL) { in = (const unsigned char *)EMPTY; len = 0; }

    if (len == 0)             return reject(out, CZO_R_EMPTY);
    if (len > CZO_MAX_INPUT)  return reject(out, CZO_R_TOO_LONG);

    /* --- 2/3. decode, census, fold ---------------------------------- */
    i = 0;
    while (i < len) {
        uint32_t cp;
        size_t adv;
        int scr;

        if (u8_decode(in, len, i, &cp, &adv) != 0)
            return reject(out, CZO_R_INVALID_UTF8);
        i += adv;

        scr = script_of(cp);
        if (scr != SCR_NEUTRAL) scripts |= (1u << scr);

        if (cp_is_invisible(cp))  { tf |= CZO_T_ZW_STRIP;       continue; }
        if (cp_is_combining(cp))  { tf |= CZO_T_DIACRITIC_FOLD; continue; }
        if (cp_is_bad_control(cp)) return reject(out, CZO_R_NONPRINTABLE);

        if (cp_is_space(cp)) {
            if (cp != 0x20u) tf |= (cp < 0x20u) ? CZO_T_CTRL_STRIP
                                                : CZO_T_WS_NORMALIZE;
            cp = 0x20u;
        } else if (cp >= 0xFF01u && cp <= 0xFF5Eu) {
            cp -= 0xFEE0u; tf |= CZO_T_COMPAT_FOLD;
        } else if ((cp >= 0x2010u && cp <= 0x2015u) || cp == 0x2212u
                || cp == 0x2043u || cp == 0xFE58u || cp == 0xFE63u) {
            cp = '-';  tf |= CZO_T_COMPAT_FOLD;
        } else if (cp == 0x2018u || cp == 0x2019u || cp == 0x201Au
                || cp == 0x201Bu || cp == 0x2032u || cp == 0x02BCu) {
            cp = '\''; tf |= CZO_T_COMPAT_FOLD;
        } else if (cp == 0x201Cu || cp == 0x201Du || cp == 0x201Eu
                || cp == 0x201Fu || cp == 0x2033u) {
            cp = '"';  tf |= CZO_T_COMPAT_FOLD;
        } else if (cp == 0x2044u || cp == 0x2215u) {
            cp = '/';  tf |= CZO_T_COMPAT_FOLD;
        }

        /* diacritic folds may expand to two ASCII characters */
        if (cp >= 0xC0u && cp <= 0xFFu && L1FOLD[cp - 0xC0u] != NULL) {
            const char *rep = L1FOLD[cp - 0xC0u];
            tf |= CZO_T_DIACRITIC_FOLD;
            while (*rep) {
                if (m >= CZO_MAX_CANON) return reject(out, CZO_R_TOO_LONG);
                buf[m++] = (uint32_t)(unsigned char)*rep++;
            }
            continue;
        }
        if (cp >= 0x100u && cp <= 0x17Fu) {
            tf |= CZO_T_DIACRITIC_FOLD;
            cp = (uint32_t)(unsigned char)LAFOLD[cp - 0x100u];
        }

        if (m >= CZO_MAX_CANON) return reject(out, CZO_R_TOO_LONG);
        buf[m++] = cp;
    }

    /* --- 4. mixed-script adjudication -------------------------------
     * Only confusable script pairs matter. Latin + CJK is not a homograph
     * risk and is not flagged. */
    {
        int confusable_scripts = 0;
        if (scripts & (1u << SCR_LATIN))    confusable_scripts++;
        if (scripts & (1u << SCR_GREEK))    confusable_scripts++;
        if (scripts & (1u << SCR_CYRILLIC)) confusable_scripts++;
        if (confusable_scripts >= 2) {
            verdict = CZO_AMBIGUOUS;
            reason  = CZO_R_MIXED_SCRIPT;
        }
    }

    /* --- 5. trim + collapse ------------------------------------------ */
    {
        size_t a = 0, b = m;
        while (a < b && buf[a] == 0x20u) a++;
        while (b > a && buf[b-1] == 0x20u) b--;
        if (a != 0 || b != m) tf |= CZO_T_TRIM;

        w = 0;
        for (j = a; j < b; j++) {
            if (buf[j] == 0x20u && w > 0 && buf[w-1] == 0x20u) {
                tf |= CZO_T_WS_COLLAPSE;
                continue;
            }
            buf[w++] = buf[j];
        }
        m = w;
    }

    /* --- 6. case fold ------------------------------------------------ */
    for (j = 0; j < m; j++) {
        uint32_t c = buf[j], f = cp_case_fold(buf[j]);
        if (f != c) { buf[j] = f; tf |= CZO_T_CASE_FOLD; }
    }

    /* --- 7. class resolution + reduction ----------------------------- */
    if (cls == CZO_CLASS_AUTO) {
        int has_space = 0, has_alpha = 0, has_digit = 0, has_other = 0;
        for (j = 0; j < m; j++) {
            uint32_t c = buf[j];
            if (c == 0x20u)                        has_space = 1;
            else if (is_digit(c))                  has_digit = 1;
            else if (c >= 'a' && c <= 'z')         has_alpha = 1;
            else if (!is_id_separator(c))          has_other = 1;
        }
        if (has_space || has_other)            cls = CZO_CLASS_NAME;
        else if (has_digit && !has_alpha)      cls = CZO_CLASS_NUMERIC;
        else                                   cls = CZO_CLASS_ID;
    }
    out->resolved_class = cls;

    if (cls == CZO_CLASS_ID || cls == CZO_CLASS_NUMERIC) {
        w = 0;
        for (j = 0; j < m; j++) {
            uint32_t c = buf[j];
            if (is_id_separator(c)) { tf |= CZO_T_SEP_STRIP; continue; }
            if (cls == CZO_CLASS_NUMERIC && !is_digit(c)) {
                tf |= CZO_T_SEP_STRIP;
                continue;
            }
            buf[w++] = c;
        }
        m = w;

        /* leading zeros: "0042" and "42" may or may not be one identity.
         * CZO picks the shorter form and refuses to pretend it knows. */
        if (m > 1 && buf[0] == '0') {
            int all_digits = 1;
            for (j = 0; j < m; j++)
                if (!is_digit(buf[j])) { all_digits = 0; break; }
            if (all_digits) {
                size_t z = 0;
                while (z + 1 < m && buf[z] == '0') z++;
                memmove(buf, buf + z, (m - z) * sizeof(buf[0]));
                m -= z;
                tf |= CZO_T_ZERO_STRIP;
                if (verdict == CZO_OK) {
                    verdict = CZO_AMBIGUOUS;
                    reason  = CZO_R_LEADING_ZERO_NUMERIC;
                }
            }
        }
    } else { /* CZO_CLASS_NAME */
        w = 0;
        for (j = 0; j < m; j++) {
            uint32_t c = buf[j];
            if (is_name_noise(c)) { tf |= CZO_T_SEP_STRIP; continue; }
            if (is_name_break(c)) { tf |= CZO_T_SEP_STRIP; c = 0x20u; }
            if (c == 0x20u && (w == 0 || buf[w-1] == 0x20u)) {
                tf |= CZO_T_WS_COLLAPSE;
                continue;
            }
            buf[w++] = c;
        }
        while (w > 0 && buf[w-1] == 0x20u) { w--; tf |= CZO_T_TRIM; }
        m = w;
    }

    /* --- 8. emptiness gate ------------------------------------------- */
    if (m == 0) {
        out->transforms = tf;
        return reject(out, CZO_R_EMPTY);
    }

    /* --- 9. encode + digest ------------------------------------------ */
    for (j = 0; j < m; j++) {
        if (enc_len + 4 >= CZO_MAX_CANON) return reject(out, CZO_R_TOO_LONG);
        enc_len += u8_encode(buf[j], enc + enc_len);
    }

    /* --- 10. skeleton ------------------------------------------------- */
    for (j = 0; j < m; j++) {
        uint32_t c = buf[j];
        char f = confusable_of(c);
        if (f) { c = (uint32_t)(unsigned char)f; confusable_hits = 1; }
        else if (cls == CZO_CLASS_NAME) {
            /* digit/letter confusion is only plausible inside names */
            if      (c == '0') { c = 'o'; confusable_hits = 1; }
            else if (c == '1') { c = 'l'; confusable_hits = 1; }
            else if (c == '5') { c = 's'; confusable_hits = 1; }
        }
        if (skel_len + 4 >= CZO_MAX_CANON) return reject(out, CZO_R_TOO_LONG);
        skel_len += u8_encode(c, skel + skel_len);
    }
    if (confusable_hits &&
        (skel_len != enc_len || memcmp(skel, enc, enc_len) != 0))
        tf |= CZO_T_SKELETON_DIFF;

    memcpy(out->canon, enc, enc_len);
    out->canon[enc_len] = '\0';
    out->canon_len  = enc_len;
    out->digest     = fnv1a64(enc, enc_len);
    out->skeleton   = fnv1a64(skel, skel_len);
    out->transforms = tf;
    out->verdict    = verdict;
    out->reason     = reason;
    return 0;
}

int czo_canonicalize_str(const char *input, czo_class cls, czo_result *out)
{
    return czo_canonicalize(input, input ? strlen(input) : 0, cls, out);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

const char *czo_verdict_str(czo_verdict v)
{
    switch (v) {
    case CZO_OK:        return "OK";
    case CZO_AMBIGUOUS: return "AMBIGUOUS";
    case CZO_REJECT:    return "REJECT";
    }
    return "?";
}

const char *czo_reason_str(czo_reason r)
{
    switch (r) {
    case CZO_R_NONE:                 return "-";
    case CZO_R_EMPTY:                return "empty";
    case CZO_R_TOO_LONG:             return "too_long";
    case CZO_R_INVALID_UTF8:         return "invalid_utf8";
    case CZO_R_NONPRINTABLE:         return "nonprintable";
    case CZO_R_MIXED_SCRIPT:         return "mixed_script";
    case CZO_R_LEADING_ZERO_NUMERIC: return "leading_zero_numeric";
    }
    return "?";
}

const char *czo_class_str(czo_class c)
{
    switch (c) {
    case CZO_CLASS_AUTO:    return "auto";
    case CZO_CLASS_ID:      return "id";
    case CZO_CLASS_NAME:    return "name";
    case CZO_CLASS_NUMERIC: return "numeric";
    }
    return "?";
}

size_t czo_transforms_str(uint32_t transforms, char *buf, size_t buflen)
{
    static const char *const NAMES[] = {
        "trim", "ws_normalize", "ws_collapse", "zw_strip", "ctrl_strip",
        "compat_fold", "diacritic_fold", "case_fold", "sep_strip",
        "zero_strip", "skeleton_diff"
    };
    const size_t n = sizeof(NAMES) / sizeof(NAMES[0]);
    size_t need = 0, k;
    int first = 1;

    if (buflen > 0) buf[0] = '\0';
    if (transforms == 0) {
        if (buflen > 1) { buf[0] = '-'; buf[1] = '\0'; }
        return 1;
    }
    for (k = 0; k < n; k++) {
        size_t l;
        if (!(transforms & (1u << k))) continue;
        l = strlen(NAMES[k]);
        if (!first) {
            if (need + 1 < buflen) buf[need] = ',';
            need++;
        }
        if (need + l < buflen) memcpy(buf + need, NAMES[k], l);
        need += l;
        first = 0;
    }
    if (buflen > 0) buf[(need < buflen) ? need : buflen - 1] = '\0';
    return need;
}

/* ------------------------------------------------------------------ */
/* Self-test                                                           */
/* ------------------------------------------------------------------ */

#define CHECK(cond, msg) \
    do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
         fails++; } } while (0)

int czo_selftest(void)
{
    czo_result a, b;
    int fails = 0;

    CHECK(strlen(LAFOLD) == 128, "LAFOLD table must be 128 entries");

    /* whitespace + case */
    czo_canonicalize_str("  ACME   Corp \t", CZO_CLASS_NAME, &a);
    CHECK(strcmp(a.canon, "acme corp") == 0, "trim/collapse/casefold");
    CHECK(a.verdict == CZO_OK, "plain name is OK");

    /* determinism + digest equality */
    czo_canonicalize_str("Acme Corp.", CZO_CLASS_NAME, &b);
    CHECK(a.digest == b.digest, "punctuation-insensitive name identity");

    /* zero-width injection is invisible to identity */
    czo_canonicalize_str("acme\xE2\x80\x8b corp", CZO_CLASS_NAME, &b);
    CHECK(b.digest == a.digest, "zero-width space stripped");
    CHECK((b.transforms & CZO_T_ZW_STRIP) != 0, "zw_strip recorded");

    /* diacritics fold to ASCII, both precomposed and decomposed */
    czo_canonicalize_str("Cr\xC3\xA8me", CZO_CLASS_NAME, &a);   /* è */
    czo_canonicalize_str("Cre\xCC\x80me", CZO_CLASS_NAME, &b);  /* e + U+0300 */
    CHECK(strcmp(a.canon, "creme") == 0, "precomposed diacritic fold");
    CHECK(a.digest == b.digest, "decomposed diacritic fold");

    /* eszett expands */
    czo_canonicalize_str("Stra\xC3\x9F" "e", CZO_CLASS_NAME, &a);
    CHECK(strcmp(a.canon, "strasse") == 0, "eszett expansion");

    /* identifier separators are noise */
    czo_canonicalize_str("INV-2024/0071", CZO_CLASS_ID, &a);
    CHECK(strcmp(a.canon, "inv20240071") == 0, "id separator strip");

    /* leading zeros: canonical but ambiguous */
    czo_canonicalize_str("0042", CZO_CLASS_ID, &a);
    czo_canonicalize_str("42",   CZO_CLASS_ID, &b);
    CHECK(strcmp(a.canon, "42") == 0, "leading zero strip");
    CHECK(a.digest == b.digest, "0042 and 42 share canonical identity");
    CHECK(a.verdict == CZO_AMBIGUOUS, "leading zero is ambiguous");
    CHECK(a.reason == CZO_R_LEADING_ZERO_NUMERIC, "leading zero reason");
    CHECK(b.verdict == CZO_OK, "42 alone is not ambiguous");

    /* all-zero identifier keeps one zero */
    czo_canonicalize_str("0000", CZO_CLASS_ID, &a);
    CHECK(strcmp(a.canon, "0") == 0, "all-zero collapses to 0");

    /* fullwidth digits are the same identifier */
    czo_canonicalize_str("\xEF\xBC\x94\xEF\xBC\x92", CZO_CLASS_ID, &a);
    CHECK(strcmp(a.canon, "42") == 0, "fullwidth digit fold");

    /* homograph: Cyrillic 'а' inside a Latin word */
    czo_canonicalize_str("p\xD0\xB0ypal", CZO_CLASS_NAME, &a);
    czo_canonicalize_str("paypal", CZO_CLASS_NAME, &b);
    CHECK(a.verdict == CZO_AMBIGUOUS, "mixed script is ambiguous");
    CHECK(a.reason == CZO_R_MIXED_SCRIPT, "mixed script reason");
    CHECK(a.digest != b.digest, "homograph is NOT silently merged");
    CHECK(a.skeleton == b.skeleton, "homograph shares a skeleton");
    CHECK((a.transforms & CZO_T_SKELETON_DIFF) != 0, "skeleton_diff recorded");

    /* case folding reaches Cyrillic and Greek, not just ASCII */
    czo_canonicalize_str("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0",
                         CZO_CLASS_NAME, &a);  /* Mосква */
    czo_canonicalize_str("\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0",
                         CZO_CLASS_NAME, &b);  /* москва */
    CHECK(a.digest == b.digest, "Cyrillic case fold");
    czo_canonicalize_str("\xCE\xA3\xCE\xB9\xCE\xB3\xCE\xBC\xCE\xB1",
                         CZO_CLASS_NAME, &a);  /* Σιγμα */
    czo_canonicalize_str("\xCF\x83\xCE\xB9\xCE\xB3\xCE\xBC\xCE\xB1",
                         CZO_CLASS_NAME, &b);  /* σιγμα */
    CHECK(a.digest == b.digest, "Greek case fold");

    /* single-script non-Latin is not a mixed-script event */
    czo_canonicalize_str("\xD0\xBC\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0",
                         CZO_CLASS_NAME, &a);
    CHECK(a.verdict == CZO_OK, "pure Cyrillic is fine");

    /* Latin + CJK is not a homograph risk */
    czo_canonicalize_str("Sony \xE3\x82\xBD\xE3\x83\x8B\xE3\x83\xBC",
                         CZO_CLASS_NAME, &a);
    CHECK(a.verdict == CZO_OK, "Latin+CJK not flagged");

    /* rejections */
    czo_canonicalize_str("", CZO_CLASS_AUTO, &a);
    CHECK(a.verdict == CZO_REJECT && a.reason == CZO_R_EMPTY, "empty reject");
    czo_canonicalize_str("   ", CZO_CLASS_AUTO, &a);
    CHECK(a.verdict == CZO_REJECT && a.reason == CZO_R_EMPTY, "blank reject");
    czo_canonicalize("bad\xC3", 4, CZO_CLASS_AUTO, &a);
    CHECK(a.reason == CZO_R_INVALID_UTF8, "truncated utf8 reject");
    czo_canonicalize("a\x01" "b", 3, CZO_CLASS_AUTO, &a);
    CHECK(a.reason == CZO_R_NONPRINTABLE, "control byte reject");
    czo_canonicalize("\xC0\xAF", 2, CZO_CLASS_AUTO, &a);
    CHECK(a.reason == CZO_R_INVALID_UTF8, "overlong encoding reject");
    czo_canonicalize("\xED\xA0\x80", 3, CZO_CLASS_AUTO, &a);
    CHECK(a.reason == CZO_R_INVALID_UTF8, "surrogate reject");
    CHECK(a.canon_len == 0, "rejected input yields no canonical form");

    /* class inference */
    czo_canonicalize_str("INV-001", CZO_CLASS_AUTO, &a);
    CHECK(a.resolved_class == CZO_CLASS_ID, "auto -> id");
    czo_canonicalize_str("Acme Corp", CZO_CLASS_AUTO, &a);
    CHECK(a.resolved_class == CZO_CLASS_NAME, "auto -> name");
    czo_canonicalize_str("00123", CZO_CLASS_AUTO, &a);
    CHECK(a.resolved_class == CZO_CLASS_NUMERIC, "auto -> numeric");

    /* idempotence: canonicalizing a canonical form is a fixed point */
    {
        static const char *const cases[] = {
            "  ACME   Corp. ", "INV-2024/0071", "Stra\xC3\x9F" "e",
            "Cr\xC3\xA8me Br\xC3\xBBl\xC3\xA9" "e", "0042"
        };
        size_t k;
        for (k = 0; k < sizeof(cases)/sizeof(cases[0]); k++) {
            czo_canonicalize_str(cases[k], CZO_CLASS_AUTO, &a);
            czo_canonicalize_str(a.canon, a.resolved_class, &b);
            CHECK(strcmp(a.canon, b.canon) == 0, "idempotence");
            CHECK(a.digest == b.digest, "idempotent digest");
        }
    }

    /* transform rendering never overflows */
    {
        char tiny[4];
        size_t need = czo_transforms_str(0xFFFFFFFFu, tiny, sizeof(tiny));
        CHECK(need > 3, "transforms_str reports required length");
        CHECK(tiny[3] == '\0', "transforms_str NUL-terminates");
    }

    if (fails == 0) printf("czo selftest: all checks passed\n");
    else            printf("czo selftest: %d failure(s)\n", fails);
    return fails;
}
