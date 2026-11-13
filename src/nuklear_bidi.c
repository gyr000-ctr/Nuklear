#include "nuklear.h"
#include "nuklear_internal.h"

/* ===============================================================
 *
 *                          BIDI (UAX #9)
 *
 * Bidirectional text support. The implementation follows a practical
 * subset of the Unicode Bidirectional Algorithm (UAX #9):
 *
 *  - P2/P3:   paragraph base direction from first strong character
 *  - X1..X8:  explicit embedding/override/isolate controls
 *             (RLE/LRE/RLO/LRO/RLI/LRI/FSI/PDF/PDI)
 *  - W1..W7:  weak type rules (numbers, separators, NSM, ...)
 *  - N0:      simplified bracket pair resolution
 *  - N1..N3:  neutral type rules
 *  - I1/I2:   implicit levels
 *  - L1:      trailing whitespace / segment separator levels
 *  - L2:      visual reordering
 *  - L4:      mirrored glyph substitution for paired punctuation
 *
 * Everything in this file is compiled only when NK_INCLUDE_BIDI is
 * defined. When bidi is disabled at compile time or at runtime through
 * nk_bidi_enable(), all text layout behaves exactly like the LTR-only
 * implementation.
 *
 * Memory model: layout results live in persistent caches that are only
 * reallocated when a longer text is encountered. Steady-state rendering
 * performs no heap allocation proportional to text length. Any internal
 * failure (allocation failure, malformed UTF-8, nesting beyond the
 * supported limit, ...) makes the public helpers fall back to plain LTR
 * processing instead of crashing. The caches require
 * NK_INCLUDE_DEFAULT_ALLOCATOR; without it the bidi entry points compile
 * but always take the LTR fallback.
 *
 * ===============================================================*/
#ifdef NK_INCLUDE_BIDI

#if defined(NK_INCLUDE_DEFAULT_ALLOCATOR) || defined(NK_INCLUDE_STANDARD_IO)
#include <stdlib.h>
#endif

/* ---------------- Bidi character types (BD_*) ---------------- */
enum {
    BD_L = 0, BD_R, BD_AL, BD_EN, BD_ES, BD_ET, BD_AN, BD_CS,
    BD_B, BD_S, BD_WS, BD_ON,
    BD_BN, BD_RLE, BD_LRE, BD_RLO, BD_LRO, BD_PDF,
    BD_RLI, BD_LRI, BD_FSI, BD_PDI,
    BD_NSM, BD_COUNT
};

#define NK_BIDI_MAX_LEVEL 61
#define NK_BIDI_STACK_CAP 63
/* safety guard against pathological inputs */
#define NK_BIDI_MAX_RUNES (1 << 24)

struct nk_bidi_stack_entry {
    nk_ushort level;
    nk_ushort override;   /* BD_L, BD_R or BD_ON */
    nk_ushort isolate;    /* 1 for isolate initiator entries */
};

struct nk_bidi_line_layout {
    const char *text;
    int byte_len;
    int cap;
    float font_height;

    nk_rune *runes;       /* logical rune storage             [cap] */
    int *bytes;           /* byte offset of each rune         [cap] */
    nk_ushort *levels;    /* resolved embedding levels        [cap] */
    int *visual;          /* per-line: vis index->logical     [cap] */
    float *adv;           /* advance width of each rune       [cap] */
    float *caret_x;       /* caret x before rune i, plus end  [cap+1] */

    /* scratch storage for per-line resolution; persistent so a cache
     * recompute only grows them instead of allocating every frame */
    nk_byte *s_raw;       /* [cap] */
    nk_ushort *s_lvl;     /* [cap] */
    nk_rune *s_cr;        /* [cap] */
    int *s_cbytes;        /* [cap] */
    int *s_vis;           /* [cap] */
    int *s_order;         /* [cap] */
};

struct nk_bidi_line_info {
    int first;            /* first logical rune index (global) */
    int count;            /* rune count including BN controls, excluding NL */
    int visible_count;    /* rendered rune count (BN controls excluded) */
    int first_byte;
    int byte_count;
    nk_byte base;         /* paragraph base level */
    nk_byte has_newline;
    float line_width;
};

struct nk_bidi_layout {
    const char *text;
    int byte_len;
    float width;
    float font_height;

    struct nk_bidi_line_layout work;
    struct nk_bidi_line_info *lines;
    int line_count;
    int line_cap;
    int rune_count;
};

/* runtime switch: off by default so output is byte-identical when unused */
NK_GLOBAL nk_bool nk_bidi_active = nk_false;
NK_GLOBAL struct nk_bidi_layout *nk_bidi_cache;

#ifdef NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_BIDI_ALLOC(size) malloc(size)
#define NK_BIDI_FREE(ptr)   free(ptr)
#else
#define NK_BIDI_ALLOC(size) ((void)(size), (void*)0)
#define NK_BIDI_FREE(ptr)   ((void)(ptr))
#endif

/* ---------------- UTF-8 helpers ---------------- */
/* Decode one rune; on malformed input advance by at least one byte and
 * substitute U+FFFD so callers can never loop forever. */
NK_INTERN int
nk_bidi_decode(const char *text, int byte_len, int off, nk_rune *out)
{
    int glen;
    if (off >= byte_len) return 0;
    glen = nk_utf_decode(text + off, out, byte_len - off);
    if (glen <= 0) {
        *out = NK_UTF_INVALID;
        return 1;
    }
    return glen;
}

/* ---------------- Bidi_Class lookup ---------------- */
NK_INTERN nk_byte
nk_bidi_type(nk_rune cp)
{
    /* explicit formatting / controls */
    if (cp >= 0x202A && cp <= 0x202E) {
        switch (cp) {
        case 0x202A: return BD_LRE;
        case 0x202B: return BD_RLE;
        case 0x202C: return BD_PDF;
        case 0x202D: return BD_LRO;
        case 0x202E: return BD_RLO;
        default: break;
        }
    }    if (cp == 0x2066) return BD_LRI;
    if (cp == 0x2067) return BD_RLI;
    if (cp == 0x2068) return BD_FSI;
    if (cp == 0x2069) return BD_PDI;
    if (cp == 0x200E) return BD_L;  /* LRM */
    if (cp == 0x200F) return BD_R;  /* RLM */
    if (cp == 0x200C || cp == 0x200D) return BD_NSM; /* ZWNJ/ZWJ */
    if (cp >= 0x2060 && cp <= 0x2064) return BD_BN;  /* WJ, invisible operators */
    if (cp == 0xFEFF) return BD_BN;                  /* ZWNBSP */
    if (cp == 0x061C || cp == 0x200B) return BD_BN;  /* ALM / ZWSP */

    /* strong LTR: Latin and other LTR scripts */
    if ((cp >= 0x0041 && cp <= 0x005A) ||
        (cp >= 0x0061 && cp <= 0x007A) ||
        (cp >= 0x00C0 && cp <= 0x00D6) ||
        (cp >= 0x00D8 && cp <= 0x00F6) ||
        (cp >= 0x00F8 && cp <= 0x02B8) ||
        (cp >= 0x02BB && cp <= 0x02C7))
        return BD_L;
    if (cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA) return BD_L;
    if (cp >= 0x1D00 && cp <= 0x1D25) return BD_L;
    if (cp >= 0x1D2C && cp <= 0x1D5C) return BD_L;
    if (cp >= 0x1D62 && cp <= 0x1D65) return BD_L;
    if (cp >= 0x1D6B && cp <= 0x1D77) return BD_L;
    if (cp >= 0x1D79 && cp <= 0x1DBE) return BD_L;
    if (cp >= 0x1E00 && cp <= 0x1EFF) return BD_L;
    if (cp == 0x2071 || cp == 0x207F) return BD_L;
    if (cp >= 0x2090 && cp <= 0x209C) return BD_L;
    if (cp >= 0x212A && cp <= 0x212B) return BD_L;
    if (cp >= 0x24B6 && cp <= 0x24E9) return BD_L;
    if (cp >= 0x2C60 && cp <= 0x2C7F) return BD_L;
    if (cp >= 0xA720 && cp <= 0xA7FF) return BD_L;
    if (cp >= 0xFB00 && cp <= 0xFB06) return BD_L;
    if (cp >= 0xFF21 && cp <= 0xFF3A) return BD_L;
    if (cp >= 0xFF41 && cp <= 0xFF5A) return BD_L;

    /* Hebrew (R) */
    if ((cp >= 0x0591 && cp <= 0x05F4) || cp == 0xFB1D ||
        (cp >= 0xFB1F && cp <= 0xFB28) || (cp >= 0xFB2A && cp <= 0xFB36) ||
        (cp >= 0xFB38 && cp <= 0xFB3C) || cp == 0xFB3E ||
        (cp >= 0xFB40 && cp <= 0xFB41) || (cp >= 0xFB43 && cp <= 0xFB4F))
        return BD_R;

    /* Arabic (AL) */
    if ((cp >= 0x0608 && cp <= 0x061B) || (cp >= 0x061D && cp <= 0x064A) ||
        (cp >= 0x0656 && cp <= 0x0669) || (cp >= 0x0670 && cp <= 0x06DC) ||
        (cp >= 0x06DE && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
        (cp >= 0x08A0 && cp <= 0x08B4) || (cp >= 0x08B6 && cp <= 0x08BD) ||
        cp == 0xFB50 || (cp >= 0xFB52 && cp <= 0xFBB1) ||
        (cp >= 0xFBD3 && cp <= 0xFD3D) || (cp >= 0xFD50 && cp <= 0xFD8F) ||
        (cp >= 0xFD92 && cp <= 0xFDC7) || (cp >= 0xFDF0 && cp <= 0xFDFB) ||
        (cp >= 0xFE70 && cp <= 0xFEFE))
        return BD_AL;

    /* European numbers and number forms */
    if ((cp >= 0x0030 && cp <= 0x0039) ||
        (cp >= 0x00B2 && cp <= 0x00B3) || cp == 0x00B9 ||
        (cp >= 0x2070 && cp <= 0x2079) ||
        (cp >= 0x2080 && cp <= 0x2089) ||
        (cp >= 0x2460 && cp <= 0x249B) ||
        (cp >= 0x24EA && cp <= 0x24FF) ||
        (cp >= 0xFF10 && cp <= 0xFF19))
        return BD_EN;

    /* Arabic-Indic / Arabic decimal numbers (AN) */
    if ((cp >= 0x0660 && cp <= 0x0669) ||
        (cp >= 0x06F0 && cp <= 0x06F9) ||
        (cp >= 0x07C0 && cp <= 0x07C9) ||
        (cp >= 0x08E0 && cp <= 0x08E9))
        return BD_AN;

    /* European number separators/terminators */
    if (cp == 0x002B || cp == 0x002D) return BD_ES;
    if (cp == 0x0023 || cp == 0x0024 || cp == 0x0025) return BD_ET;
    if (cp == 0x00A2 || cp == 0x00A3 || cp == 0x00A5) return BD_ET;
    if (cp == 0x0606 || cp == 0x0607) return BD_ET;
    if (cp == 0x02B0 || cp == 0x02B1 || cp == 0x20A0 || cp == 0x20A1) return BD_ET;
    if (cp >= 0x20A3 && cp <= 0x20A8) return BD_ET;
    if (cp >= 0x20AA && cp <= 0x20CF) return BD_ET;

    /* common separators */
    if (cp == 0x002C || cp == 0x002E || cp == 0x003A || cp == 0x066C ||
        cp == 0x07F8 || cp == 0x2044 || cp == 0xFE10 || cp == 0xFE13 ||
        cp == 0xFE50 || cp == 0xFE52 || cp == 0xFE55 || cp == 0xFF0C ||
        cp == 0xFF0E || cp == 0xFF1A)
        return BD_CS;
    if (cp >= 0x066A && cp <= 0x066B) return BD_CS;
    if (cp >= 0x2030 && cp <= 0x2038) return BD_CS;

    /* block/segment separators and whitespace */
    if (cp == 0x000A || cp == 0x000D || cp == 0x0085 ||
        cp == 0x2028 || cp == 0x2029)
        return BD_B;
    if (cp == 0x0009 || cp == 0x000B || cp == 0x000C || cp == 0x001C ||
        cp == 0x001D || cp == 0x001E || cp == 0x2027)
        return BD_S;
    if (cp == 0x0020 || cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) ||
        cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0x1680)
        return BD_WS;

    /* common Arabic/Hebrew combining marks (NSM) */
    if (cp == 0x034F || (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) ||
        (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) ||
        (cp >= 0x06E7 && cp <= 0x06E8) || (cp >= 0x06EA && cp <= 0x06ED) ||
        (cp >= 0x0711 && cp <= 0x0711) || (cp >= 0x0730 && cp <= 0x074A) ||
        (cp >= 0x07A6 && cp <= 0x07B0) || (cp >= 0x07EB && cp <= 0x07F3) ||
        (cp >= 0x0816 && cp <= 0x0827) || (cp >= 0x0829 && cp <= 0x082D) ||
        (cp >= 0x0859 && cp <= 0x085B) || (cp >= 0x08D3 && cp <= 0x08E1) ||
        (cp >= 0x08E3 && cp <= 0x08FF))
        return BD_NSM;

    /* default: everything else (including CJK and unknown codepoints) is LTR */
    return BD_L;
}

/* ---------------- P2/P3: paragraph base direction ---------------- */
NK_INTERN nk_byte
nk_bidi_paragraph_base(const nk_rune *runes, const nk_byte *raw, int n)
{
    int i;
    int depth = 0;
    NK_UNUSED(runes);
    for (i = 0; i < n; ++i) {
        nk_byte t = raw[i];
        if (t == BD_LRI || t == BD_RLI || t == BD_FSI) { depth++; continue; }
        if (t == BD_PDI) { if (depth > 0) depth--; continue; }
        if (depth) continue;
        if (t == BD_L) return 0;
        if (t == BD_R || t == BD_AL) return 1;
    }
    return 0; /* P3 default */
}

NK_INTERN nk_byte
nk_bidi_fsi_direction(const nk_rune *runes, const nk_byte *raw, int n, int pos)
{
    int depth = 0, i;
    NK_UNUSED(runes);
    for (i = pos + 1; i < n; ++i) {
        nk_byte t = raw[i];
        if (t == BD_LRI || t == BD_RLI || t == BD_FSI) { depth++; continue; }
        if (t == BD_PDI) {
            if (depth == 0) break;
            depth--;
            continue;
        }
        if (depth) continue;
        if (t == BD_L) return 0;
        if (t == BD_R || t == BD_AL) return 1;
    }
    return 0;
}

/* ---------------- X rules: explicit levels and overrides ---------------- */
NK_INTERN void
nk_bidi_x_rules(const nk_rune *runes, nk_byte *raw, nk_ushort *levels, int n,
    nk_byte base)
{
    struct nk_bidi_stack_entry stk[NK_BIDI_STACK_CAP];
    int sp = 1;
    int i;

    stk[0].level = base;
    stk[0].override = BD_ON;
    stk[0].isolate = 0;

    for (i = 0; i < n; ++i) {
        nk_byte t = raw[i];
        struct nk_bidi_stack_entry top;
        switch (t) {
        case BD_RLE: case BD_LRE: case BD_RLO: case BD_LRO:
        case BD_RLI: case BD_LRI: case BD_FSI: {
            nk_byte dir;
            nk_byte ov;
            struct nk_bidi_stack_entry e;
            if (t == BD_FSI)
                dir = nk_bidi_fsi_direction(runes, raw, n, i);
            else if (t == BD_RLE || t == BD_RLO || t == BD_RLI)
                dir = 1;
            else
                dir = 0;
            ov = (t == BD_RLO) ? BD_R : (t == BD_LRO) ? BD_L : BD_ON;
            if (sp >= NK_BIDI_STACK_CAP) {
                /* overflow (X8): invalid/ignored, keep current context */
                raw[i] = BD_BN;
                levels[i] = stk[sp - 1].level;
                break;
            }
            e.level = (nk_ushort)((dir == 1) ?
                ((stk[sp-1].level + 1) | 1) :
                ((stk[sp-1].level + 2) & ~1));
            if (e.level > NK_BIDI_MAX_LEVEL)
                e.level = NK_BIDI_MAX_LEVEL;
            e.override = ov;
            e.isolate = (t == BD_RLI || t == BD_LRI || t == BD_FSI) ? 1 : 0;
            stk[sp++] = e;
            raw[i] = BD_BN;
            levels[i] = e.level;
            break;
        }
        case BD_PDF:
            if (sp > 1 && !stk[sp-1].isolate) sp--;
            raw[i] = BD_BN;
            levels[i] = stk[sp-1].level;
            break;
        case BD_PDI:
            while (sp > 1 && !stk[sp-1].isolate) sp--;
            if (sp > 1) sp--;
            raw[i] = BD_BN;
            levels[i] = stk[sp-1].level;
            break;
        case BD_B:
            /* paragraph separator: reset embedding to base */
            stk[0].level = base;
            stk[0].override = BD_ON;
            sp = 1;
            levels[i] = base;
            break;
        default:
            top = stk[sp-1];
            levels[i] = top.level;
            if (top.override == BD_L && (t == BD_L || t == BD_R || t == BD_AL))
                raw[i] = BD_L;
            else if (top.override == BD_R &&
                     (t == BD_L || t == BD_R || t == BD_AL))
                raw[i] = BD_R;
            break;
        }
    }
}

/* ---------------- W rules: weak types ---------------- */
NK_INTERN void
nk_bidi_w_rules(nk_byte *t, int n)
{
    int i;
    /* W1: NSM takes the type of the previous character */
    for (i = 0; i < n; ++i) {
        int j;
        if (t[i] != BD_NSM) continue;
        j = i - 1;
        while (j >= 0 && (t[j] == BD_BN || t[j] == BD_B)) j--;
        t[i] = (j >= 0 && t[j] != BD_B) ? t[j] : BD_ON;
    }
    /* W2: EN after AL (with only AN/EN between) becomes AN */
    for (i = 0; i < n; ++i) {
        int j;
        if (t[i] != BD_EN) continue;
        j = i - 1;
        while (j >= 0 && (t[j] == BD_AN || t[j] == BD_EN)) j--;
        if (j >= 0 && t[j] == BD_AL) t[i] = BD_AN;
    }
    /* W3: AL becomes R */
    for (i = 0; i < n; ++i)
        if (t[i] == BD_AL) t[i] = BD_R;
    /* W4: ES between EN -> EN; CS between EN/EN or AN/AN */
    for (i = 1; i < n - 1; ++i) {
        if (t[i] == BD_ES && t[i-1] == BD_EN && t[i+1] == BD_EN)
            t[i] = BD_EN;
        else if (t[i] == BD_CS &&
                 ((t[i-1] == BD_EN && t[i+1] == BD_EN) ||
                  (t[i-1] == BD_AN && t[i+1] == BD_AN)))
            t[i] = (t[i-1] == BD_AN) ? BD_AN : BD_EN;
    }
    /* W5: ET sequences adjacent to EN become EN */
    for (i = 0; i < n; ++i) {
        int j, k, prev, next;
        if (t[i] != BD_ET) continue;
        j = i;
        while (j < n && t[j] == BD_ET) j++;
        prev = i - 1;
        next = j;
        if ((prev >= 0 && t[prev] == BD_EN) ||
            (next < n && t[next] == BD_EN)) {
            for (k = i; k < j; ++k) t[k] = BD_EN;
        }
        i = j - 1;
    }
    /* W6: remaining separators and ET default to ON */
    for (i = 0; i < n; ++i) {
        if (t[i] == BD_ES || t[i] == BD_ET || t[i] == BD_CS)
            t[i] = BD_ON;
    }
    /* W7: EN becomes L when an L is the previous strong type */
    for (i = 0; i < n; ++i) {
        int j;
        if (t[i] != BD_EN) continue;
        j = i - 1;
        while (j >= 0 && t[j] != BD_L && t[j] != BD_R) j--;
        if (j >= 0 && t[j] == BD_L) t[i] = BD_L;
    }
}

/* ---------------- L4: mirrored glyphs ---------------- */
NK_INTERN nk_rune
nk_bidi_mirror(nk_rune cp)
{
    static const struct { nk_rune a, b; } pairs[] = {
        {0x0028, 0x0029}, {0x003C, 0x003E}, {0x005B, 0x005D},
        {0x007B, 0x007D}, {0x00AB, 0x00BB}, {0x2039, 0x203A},
        {0x208D, 0x208E}, {0x2329, 0x232A}, {0x2768, 0x2769},
        {0x276A, 0x276B}, {0x276C, 0x276D}, {0x276E, 0x276F},
        {0x2770, 0x2771}, {0x2772, 0x2773}, {0x2774, 0x2775},
        {0x27C5, 0x27C6}, {0x27E6, 0x27E7}, {0x27E8, 0x27E9},
        {0x27EA, 0x27EB}, {0x2983, 0x2984}, {0x2985, 0x2986},
        {0x2987, 0x2988}, {0x2989, 0x298A}, {0x298B, 0x298C},
        {0x298D, 0x298E}, {0x298F, 0x2990}, {0x2991, 0x2992},
        {0x2993, 0x2994}, {0x2995, 0x2996}, {0x2997, 0x2998},
        {0x29D8, 0x29D9}, {0x29DA, 0x29DB}, {0x29FC, 0x29FD},
        {0x3008, 0x3009}, {0x300A, 0x300B}, {0x300C, 0x300D},
        {0x300E, 0x300F}, {0x3010, 0x3011}, {0x3014, 0x3015},
        {0x3016, 0x3017}, {0x3018, 0x3019}, {0x301A, 0x301B},
        {0xFE59, 0xFE5A}, {0xFE5B, 0xFE5C}, {0xFE5D, 0xFE5E},
        {0xFE64, 0xFE65}, {0xFF08, 0xFF09}, {0xFF1B, 0xFF1D},
        {0xFF3B, 0xFF3D}, {0xFF5B, 0xFF5D}
    };
    int lo = 0, hi = (int)(sizeof(pairs)/sizeof(pairs[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cp == pairs[mid].a) return pairs[mid].b;
        if (cp == pairs[mid].b) return pairs[mid].a;
        if (cp < pairs[mid].a) hi = mid - 1;
        else lo = mid + 1;
    }
    return cp;
}

NK_INTERN nk_bool
nk_bidi_is_open(nk_rune cp)
{
    static const nk_rune opens[] = {
        0x0028, 0x003C, 0x005B, 0x007B, 0x00AB, 0x2039, 0x2329,
        0x2768, 0x276A, 0x276C, 0x276E, 0x2770, 0x2772, 0x2774,
        0x27E6, 0x27E8, 0x27EA, 0x3008, 0x300A, 0x300C, 0x300E,
        0x3010, 0x3014, 0x3016, 0x3018, 0x301A, 0xFE59, 0xFE5B,
        0xFE5D, 0xFF08, 0xFF1B, 0xFF3B, 0xFF5B
    };
    int lo = 0, hi = (int)(sizeof(opens)/sizeof(opens[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cp == opens[mid]) return nk_true;
        if (cp < opens[mid]) hi = mid - 1;
        else lo = mid + 1;
    }
    return nk_false;
}

/* ---------------- N0: bracket pairs (simplified) ---------------- */
NK_INTERN void
nk_bidi_n0_rules(nk_byte *t, const nk_rune *runes, int n)
{
    int stack[NK_BIDI_STACK_CAP];
    int sp = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (t[i] != BD_ON) continue;
        if (nk_bidi_is_open(runes[i])) {
            if (sp < NK_BIDI_STACK_CAP)
                stack[sp++] = i;
        } else if (sp > 0 &&
                   nk_bidi_mirror(runes[stack[sp-1]]) == runes[i]) {
            int open = stack[--sp];
            int j;
            nk_byte strong = BD_ON;
            for (j = open - 1; j >= 0; --j) {
                if (t[j] == BD_L || t[j] == BD_R) { strong = t[j]; break; }
            }
            if (strong == BD_ON) strong = BD_L;
            t[open] = strong;
            t[i] = strong;
        }
    }
}

/* ---------------- N rules: neutral types ---------------- */
NK_INTERN void
nk_bidi_n_rules(nk_byte *t, const nk_ushort *levels, int n, nk_byte base)
{
    int i = 0;
    NK_UNUSED(base);
    while (i < n) {
        int s, e, prev, next, k;
        nk_byte t0 = t[i];
        if (t0 != BD_ON && t0 != BD_B && t0 != BD_S && t0 != BD_WS) {
            ++i;
            continue;
        }
        s = i; e = i;
        while (e < n && (t[e] == BD_ON || t[e] == BD_B ||
                         t[e] == BD_S || t[e] == BD_WS))
            ++e;
        prev = s - 1;
        next = e;
        if (prev >= 0 && next < n && t[prev] == t[next] &&
            (t[prev] == BD_R || t[prev] == BD_L)) {
            for (k = s; k < e; ++k) t[k] = t[prev];      /* N1 */
        } else if (prev >= 0 && (t[prev] == BD_R || t[prev] == BD_L)) {
            for (k = s; k < e; ++k) t[k] = t[prev];      /* N2 */
        } else if (next < n && (t[next] == BD_R || t[next] == BD_L)) {
            for (k = s; k < e; ++k) t[k] = t[next];
        } else {
            nk_byte dir = (levels[s] & 1) ? BD_R : BD_L; /* N3 */
            for (k = s; k < e; ++k) t[k] = dir;
        }
        i = e;
    }
}

/* ---------------- I rules: implicit levels ---------------- */
NK_INTERN void
nk_bidi_i_rules(nk_byte *t, nk_ushort *levels, int n)
{
    int i;
    for (i = 0; i < n; ++i) {
        nk_ushort lvl = levels[i];
        if (lvl & 1) {
            if (t[i] == BD_R)
                levels[i] = lvl + 1;
            else if (t[i] == BD_AN || t[i] == BD_EN)
                levels[i] = lvl + 2;
            else
                levels[i] = lvl + 1;
        } else {
            if (t[i] == BD_R)
                levels[i] = lvl + 1;
            else if (t[i] == BD_AN || t[i] == BD_EN)
                levels[i] = lvl + 2;
        }
    }
}

/* ---------------- L1: whitespace/separator levels ---------------- */
NK_INTERN void
nk_bidi_l1_rules(nk_byte *t, nk_ushort *levels, int n, nk_byte base)
{
    int i;
    int sos, eos;
    /* B/S run at end of sequence takes paragraph embedding level */
    for (i = n - 1; i >= 0; --i) {
        if (t[i] == BD_B || t[i] == BD_S)
            levels[i] = base;
        else if (t[i] == BD_WS)
            levels[i] = base;
        else
            break;
    }
    /* any remaining B/S gets paragraph level */
    for (i = 0; i < n; ++i)
        if (t[i] == BD_B || t[i] == BD_S)
            levels[i] = base;
    sos = base;
    eos = (n > 0) ? levels[n-1] : base;
    if ((eos & 1) != (base & 1)) eos = base;
    NK_UNUSED(sos);
}

/* ---------------- L2: visual reordering ---------------- */
NK_INTERN void
nk_bidi_reverse(int *order, int lo, int hi)
{
    while (lo < hi) {
        int tmp = order[lo];
        order[lo] = order[hi];
        order[hi] = tmp;
        ++lo; --hi;
    }
}

NK_INTERN void
nk_bidi_l2(const int *order_in, int *order_out, const nk_ushort *levels,
    int n, nk_byte base_level)
{
    int i;
    int max_level = 0;
    int lowest_odd;
    for (i = 0; i < n; ++i) {
        order_out[i] = order_in[i];
        if (levels[i] > max_level) max_level = levels[i];
    }
    /* Reorder from the lowest odd level above the paragraph level. For an
     * odd (RTL) base that is base+1; for an even (LTR) base the first
     * odd level is base+1, but even embedding runs (e.g. RLE in an LTR
     * paragraph) must still reverse, so start at base+2 there. */
    lowest_odd = (base_level & 1) ? base_level + 1 : base_level + 2;
    if (lowest_odd > max_level) return;
    for (i = lowest_odd; i <= max_level; ++i) {
        int lo = -1;
        int j;
        for (j = 0; j <= n; ++j) {
            if (j < n && levels[order_out[j]] >= i) {
                if (lo < 0) lo = j;
            } else if (lo >= 0) {
                nk_bidi_reverse(order_out, lo, j - 1);
                lo = -1;
            }
        }
    }
}

/* ---------------- persistent work storage ---------------- */
NK_INTERN nk_bool
nk_bidi_grow_line(struct nk_bidi_line_layout *l, int need)
{
    if (need <= l->cap) return nk_true;
    {
        int ncap = l->cap ? l->cap : 16;
        void *p;
        while (ncap < need) ncap *= 2;
        if (ncap > NK_BIDI_MAX_RUNES) return nk_false;

        p = NK_BIDI_ALLOC(sizeof(nk_rune) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->runes) { NK_MEMCPY(p, l->runes, sizeof(nk_rune)*(nk_size)l->cap); NK_BIDI_FREE(l->runes); }
        l->runes = (nk_rune*)p;

        p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->bytes) { NK_MEMCPY(p, l->bytes, sizeof(int)*(nk_size)l->cap); NK_BIDI_FREE(l->bytes); }
        l->bytes = (int*)p;

        p = NK_BIDI_ALLOC(sizeof(nk_ushort) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->levels) { NK_MEMCPY(p, l->levels, sizeof(nk_ushort)*(nk_size)l->cap); NK_BIDI_FREE(l->levels); }
        l->levels = (nk_ushort*)p;

        p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->visual) NK_BIDI_FREE(l->visual);
        l->visual = (int*)p;

        p = NK_BIDI_ALLOC(sizeof(float) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->adv) NK_BIDI_FREE(l->adv);
        l->adv = (float*)p;

        p = NK_BIDI_ALLOC(sizeof(float) * (nk_size)(ncap + 1));
        if (!p) return nk_false;
        if (l->caret_x) NK_BIDI_FREE(l->caret_x);
        l->caret_x = (float*)p;

        /* scratch arrays: content never needs to survive a grow */
        p = NK_BIDI_ALLOC(sizeof(nk_byte) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_raw) NK_BIDI_FREE(l->s_raw);
        l->s_raw = (nk_byte*)p;

        p = NK_BIDI_ALLOC(sizeof(nk_ushort) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_lvl) NK_BIDI_FREE(l->s_lvl);
        l->s_lvl = (nk_ushort*)p;

        p = NK_BIDI_ALLOC(sizeof(nk_rune) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_cr) NK_BIDI_FREE(l->s_cr);
        l->s_cr = (nk_rune*)p;

        p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_cbytes) NK_BIDI_FREE(l->s_cbytes);
        l->s_cbytes = (int*)p;

        p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_vis) NK_BIDI_FREE(l->s_vis);
        l->s_vis = (int*)p;

        p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
        if (!p) return nk_false;
        if (l->s_order) NK_BIDI_FREE(l->s_order);
        l->s_order = (int*)p;

        l->cap = ncap;
    }
    return nk_true;
}

/* ---------------- per-line bidi resolution ---------------- */
NK_INTERN nk_bool
nk_bidi_resolve_line(struct nk_bidi_line_layout *l,
    struct nk_bidi_line_info *info, const struct nk_user_font *font)
{
    int first = info->first;
    int count = info->count;
    nk_byte base;
    nk_byte *raw;       /* compacted types, visible runes only  */
    nk_ushort *lvl;    /* compacted levels, visible runes only  */
    nk_rune *cr;       /* compacted runes                        */
    int *cbytes;       /* compacted byte offsets                 */
    int *vis;          /* compacted index -> local logical index */
    int *order;        /* local visual order                     */
    int i, vcount = 0;

    if (count <= 0) {
        info->base = 0;
        info->visible_count = 0;
        info->line_width = 0;
        return nk_true;
    }
    if (!l->s_raw) return nk_false;
    raw = l->s_raw;
    lvl = l->s_lvl;
    cr = l->s_cr;
    cbytes = l->s_cbytes;
    vis = l->s_vis;
    order = l->s_order;

    /* X rules operate on the full logical range; runes/bytes/levels are
     * never moved in l->*, so editor logical indices stay valid */
    for (i = 0; i < count; ++i) {
        raw[i] = nk_bidi_type(l->runes[first + i]);
        l->levels[first + i] = 0;
    }
    base = nk_bidi_paragraph_base(l->runes + first, raw, count);
    info->base = base;
    nk_bidi_x_rules(l->runes + first, raw, l->levels + first, count, base);

    /* build compacted working copies; BN controls are simply omitted */
    for (i = 0; i < count; ++i) {
        if (raw[i] == BD_BN) continue;
        vis[vcount] = i;
        cr[vcount] = l->runes[first + i];
        cbytes[vcount] = l->bytes[first + i];
        lvl[vcount] = l->levels[first + i];
        raw[vcount] = raw[i];
        vcount++;
    }
    info->visible_count = vcount;
    if (vcount == 0) {
        info->line_width = 0;
        return nk_true;
    }

    nk_bidi_w_rules(raw, vcount);
    nk_bidi_n0_rules(raw, cr, vcount);
    nk_bidi_n_rules(raw, lvl, vcount, base);
    nk_bidi_i_rules(raw, lvl, vcount);
    nk_bidi_l1_rules(raw, lvl, vcount, base);

    /* resolve visual order locally, then map back to global indices */
    for (i = 0; i < vcount; ++i) order[i] = i;
    nk_bidi_l2(order, order, lvl, vcount, base);
    for (i = 0; i < vcount; ++i)
        l->visual[first + i] = first + vis[order[i]];

    /* glyph advances and caret positions in visual order. Results are
     * stored in the global slot of each rune so caret lookups by logical
     * index work directly. */
    {
        float x = 0;
        int vi;
        for (vi = 0; vi < vcount; ++vi) {
            int local = order[vi];
            int gslot = first + vis[local];
            int glen;
            nk_rune u;
            glen = nk_bidi_decode(l->text, l->byte_len,
                cbytes[local], &u);
            l->adv[gslot] = font->width(font->userdata, font->height,
                l->text + cbytes[local], glen);
        }
        for (vi = 0; vi < vcount; ++vi) {
            int local = order[vi];
            int gslot = first + vis[local];
            l->caret_x[gslot] = x;
            l->levels[gslot] = lvl[local];
            x += l->adv[gslot];
        }
        /* BN control slots carry no width; assign them the caret x of the
         * next visible rune (or the total width when trailing). Visible
         * slots keep the values computed above. */
        {
            nk_byte *is_visible = raw; /* reuse scratch as a marker */
            int k;
            for (i = 0; i < count; ++i) is_visible[i] = 0;
            for (k = 0; k < vcount; ++k) is_visible[vis[k]] = 1;
            for (i = 0; i < count; ++i) {
                int gslot = first + i;
                int found = 0;
                if (is_visible[i]) continue;
                for (k = 0; k < vcount; ++k) {
                    if (vis[k] > i) {
                        l->caret_x[gslot] =
                            l->caret_x[first + vis[k]];
                        l->adv[gslot] = 0;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    l->caret_x[gslot] = x;
                    l->adv[gslot] = 0;
                }
            }
        }
        l->caret_x[first + count] = x;
        info->line_width = x;
    }
    /* scratch arrays are owned by l and reused across lines/frames */
    return nk_true;
}

/* ---------------- full layout with persistent cache ---------------- */
NK_INTERN nk_bool
nk_bidi_layout_alloc(struct nk_bidi_layout *lay)
{
    nk_zero_struct(*lay);
    lay->line_cap = 8;
    lay->lines = (struct nk_bidi_line_info*)NK_BIDI_ALLOC(
        sizeof(struct nk_bidi_line_info) * (nk_size)lay->line_cap);
    return lay->lines != 0;
}

NK_INTERN nk_bool
nk_bidi_layout_valid(const struct nk_bidi_layout *lay, const char *text,
    int byte_len, float width, float font_height)
{
    return lay->text == text && lay->byte_len == byte_len &&
           lay->width == width && lay->font_height == font_height &&
           lay->line_count > 0;
}

NK_INTERN nk_bool
nk_bidi_layout_compute(struct nk_bidi_layout *lay, const char *text,
    int byte_len, float width, const struct nk_user_font *font)
{
    int off = 0;
    int rune_total = 0;
    int line_idx = 0;
    struct nk_bidi_line_layout *w = &lay->work;

    lay->text = 0;
    lay->byte_len = 0;
    lay->line_count = 0;
    lay->width = width;
    lay->font_height = font ? font->height : 0;

    if (!text || byte_len <= 0 || !font)
        return nk_false;

    /* decode once; size work arrays from the rune count */
    {
        nk_rune u;
        int glen = nk_bidi_decode(text, byte_len, 0, &u);
        int pos = 0;
        while (glen > 0 && pos < byte_len) {
            rune_total++;
            pos += glen;
            glen = nk_bidi_decode(text, byte_len, pos, &u);
        }
        if (rune_total == 0 || rune_total > NK_BIDI_MAX_RUNES)
            return nk_false;
    }
    if (!nk_bidi_grow_line(w, rune_total + 1)) return nk_false;

    {
        nk_rune u;
        int glen = nk_bidi_decode(text, byte_len, 0, &u);
        int idx = 0;
        while (glen > 0 && off < byte_len && idx < rune_total) {
            if (u == NK_UTF_INVALID)
                return nk_false;
            w->runes[idx] = u;
            w->bytes[idx] = off;
            off += glen;
            idx++;
            glen = nk_bidi_decode(text, byte_len, off, &u);
        }
        rune_total = idx;
        w->text = text;
        w->byte_len = byte_len;
        w->font_height = font->height;
    }
    lay->rune_count = rune_total;

    /* line breaking: explicit newlines, then word wrap at `width` */
    {
        int li = 0;
        for (;;) {
            int start = li;
            int word_start = li;
            float x = 0;
            struct nk_bidi_line_info *info;

            if (li >= rune_total) break;
            if (line_idx >= lay->line_cap) {
                int ncap = lay->line_cap * 2;
                void *p = NK_BIDI_ALLOC(
                    sizeof(struct nk_bidi_line_info) * (nk_size)ncap);
                if (!p) return nk_false;
                NK_MEMCPY(p, lay->lines,
                    sizeof(struct nk_bidi_line_info) * (nk_size)lay->line_cap);
                NK_BIDI_FREE(lay->lines);
                lay->lines = (struct nk_bidi_line_info*)p;
                lay->line_cap = ncap;
            }
            info = &lay->lines[line_idx];
            nk_zero_struct(*info);

            while (li < rune_total) {
                nk_rune u = w->runes[li];
                int glen2;
                float adv;
                if (u == '\n') {
                    li++;
                    info->has_newline = 1;
                    break;
                }
                {
                    nk_rune tmp;
                    int bo = w->bytes[li];
                    glen2 = nk_bidi_decode(text, byte_len, bo, &tmp);
                    adv = font->width(font->userdata, font->height,
                        text + bo, glen2);
                }
                if (u == ' ' || u == '\t')
                    word_start = li + 1;
                if (width > 0 && x + adv > width && li > start) {
                    if (word_start > start && word_start < li)
                        li = word_start;
                    break;
                }
                x += adv;
                li++;
            }
            info->first = start;
            info->count = li - start - (info->has_newline ? 1 : 0);
            if (info->count < 0) info->count = 0;
            info->first_byte = w->bytes[start];
            info->byte_count = ((li < rune_total) ? w->bytes[li] : byte_len)
                - w->bytes[start];
            line_idx++;
        }
        /* trailing newline / empty paragraph produces an empty final line */
        if (rune_total > 0 && w->runes[rune_total - 1] == '\n') {
            struct nk_bidi_line_info *info;
            if (line_idx >= lay->line_cap) {
                int ncap = lay->line_cap * 2;
                void *p = NK_BIDI_ALLOC(
                    sizeof(struct nk_bidi_line_info) * (nk_size)ncap);
                if (!p) return nk_false;
                NK_MEMCPY(p, lay->lines,
                    sizeof(struct nk_bidi_line_info) * (nk_size)lay->line_cap);
                NK_BIDI_FREE(lay->lines);
                lay->lines = (struct nk_bidi_line_info*)p;
                lay->line_cap = ncap;
            }
            info = &lay->lines[line_idx];
            nk_zero_struct(*info);
            info->first = rune_total;
            info->count = 0;
            info->first_byte = byte_len;
            line_idx++;
        }
    }
    lay->line_count = line_idx;

    /* resolve bidi per line; any failure invalidates the whole layout */
    {
        int k;
        for (k = 0; k < line_idx; ++k) {
            if (!nk_bidi_resolve_line(w, &lay->lines[k], font)) {
                lay->line_count = 0;
                return nk_false;
            }
        }
    }

    lay->text = text;
    lay->byte_len = byte_len;
    return nk_true;
}

NK_INTERN struct nk_bidi_layout*
nk_bidi_get_layout(const char *text, int byte_len, float width,
    const struct nk_user_font *font)
{
    if (!nk_bidi_active) return 0;
    if (!nk_bidi_cache) {
        nk_bidi_cache = (struct nk_bidi_layout*)NK_BIDI_ALLOC(
            sizeof(struct nk_bidi_layout));
        if (!nk_bidi_cache) return 0;
        if (!nk_bidi_layout_alloc(nk_bidi_cache)) {
            NK_BIDI_FREE(nk_bidi_cache);
            nk_bidi_cache = 0;
            return 0;
        }
    }
    if (nk_bidi_layout_valid(nk_bidi_cache, text, byte_len, width,
            font ? font->height : 0))
        return nk_bidi_cache;
    if (!nk_bidi_layout_compute(nk_bidi_cache, text, byte_len, width, font))
        return 0;
    return nk_bidi_cache;
}

/* ===============================================================
 *                          PUBLIC API
 * ===============================================================*/
NK_API void
nk_bidi_enable(nk_bool enabled)
{
    nk_bidi_active = enabled ? nk_true : nk_false;
    if (!enabled && nk_bidi_cache) {
        /* keep allocated storage for reuse; just invalidate content */
        nk_bidi_cache->text = 0;
        nk_bidi_cache->byte_len = 0;
        nk_bidi_cache->line_count = 0;
    }
}

NK_API nk_bool
nk_bidi_is_enabled(void)
{
    return nk_bidi_active;
}

/* base direction without a font: decode + P2/P3 only, never allocates */
NK_API nk_bool
nk_bidi_text_direction(const char *text, int byte_len)
{
    nk_rune u;
    int off = 0;
    int depth = 0;
    if (!text || byte_len <= 0) return nk_false;
    while (off < byte_len) {
        nk_byte t;
        int glen = nk_bidi_decode(text, byte_len, off, &u);
        if (u == NK_UTF_INVALID) return nk_false;
        off += glen;
        t = nk_bidi_type(u);
        if (t == BD_LRI || t == BD_RLI || t == BD_FSI) { depth++; continue; }
        if (t == BD_PDI) { if (depth > 0) depth--; continue; }
        if (depth) continue;
        if (t == BD_L) return nk_false;
        if (t == BD_R || t == BD_AL) return nk_true;
    }
    return nk_false;
}

NK_API nk_bool
nk_bidi_line_direction(const char *text, int byte_len, int line_index)
{
    if (!nk_bidi_active || !text || byte_len <= 0)
        return nk_bidi_text_direction(text, byte_len);
    /* direction does not need font metrics; scan directly */
    {
        nk_rune u;
        int off = 0, line = 0, depth = 0;
        while (off < byte_len) {
            nk_byte t;
            int glen = nk_bidi_decode(text, byte_len, off, &u);
            if (u == NK_UTF_INVALID) return nk_false;
            off += glen;
            if (u == '\n') { line++; continue; }
            if (line != line_index) continue;
            t = nk_bidi_type(u);
            if (t == BD_LRI || t == BD_RLI || t == BD_FSI) { depth++; continue; }
            if (t == BD_PDI) { if (depth > 0) depth--; continue; }
            if (depth) continue;
            if (t == BD_L) return nk_false;
            if (t == BD_R || t == BD_AL) return nk_true;
        }
    }
    return nk_false;
}

NK_API int
nk_bidi_hit_test(const char *text, int byte_len, int line_index, float x,
    const struct nk_user_font *font, float width)
{
    struct nk_bidi_layout *lay;
    struct nk_bidi_line_info *info;
    struct nk_bidi_line_layout *w;
    int vi;
    if (!nk_bidi_active || !font) return -1;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay || line_index < 0 || line_index >= lay->line_count)
        return -1;
    info = &lay->lines[line_index];
    if (info->count <= 0) return info->first;
    w = &lay->work;
    for (vi = 0; vi < info->visible_count; ++vi) {
        int li2 = w->visual[info->first + vi];
        float before = w->caret_x[li2];
        /* visual right edge is this rune's own advance, NOT caret_x[li2+1]:
         * the logical neighbor can lie visually to the left in RTL runs */
        float after = before + w->adv[li2];
        if (x < before + (after - before) * 0.5f)
            return li2;
    }
    return info->first + info->count;
}

NK_API float
nk_bidi_caret_x(const char *text, int byte_len, int logical_index,
    int *line_index_out, const struct nk_user_font *font, float width)
{
    struct nk_bidi_layout *lay;
    int k;
    if (!nk_bidi_active || !font) return 0;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay) return 0;
    for (k = 0; k < lay->line_count; ++k) {
        struct nk_bidi_line_info *info = &lay->lines[k];
        int end = info->first + info->count;
        if (logical_index >= info->first && logical_index <= end) {
            if (line_index_out) *line_index_out = k;
            return lay->work.caret_x[logical_index];
        }
    }
    if (line_index_out && lay->line_count > 0)
        *line_index_out = lay->line_count - 1;
    if (lay->line_count > 0) {
        struct nk_bidi_line_info *info = &lay->lines[lay->line_count - 1];
        return lay->work.caret_x[info->first + info->count];
    }
    return 0;
}

/* find the line containing a logical index */
NK_INTERN int
nk_bidi_line_of(struct nk_bidi_layout *lay, int logical_index)
{
    int k;
    for (k = 0; k < lay->line_count; ++k) {
        struct nk_bidi_line_info *info = &lay->lines[k];
        if (logical_index >= info->first &&
            logical_index <= info->first + info->count)
            return k;
    }
    return lay->line_count - 1;
}

NK_API int
nk_bidi_line_count(const char *text, int byte_len,
    const struct nk_user_font *font, float width)
{
    struct nk_bidi_layout *lay;
    if (!nk_bidi_active || !font) return 0;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay) return 0;
    return lay->line_count;
}

NK_API nk_bool
nk_bidi_line_info(const char *text, int byte_len, int line_index,
    int *first_rune, int *rune_count, float *line_width,
    const struct nk_user_font *font, float width)
{
    struct nk_bidi_layout *lay;
    struct nk_bidi_line_info *info;
    if (!nk_bidi_active || !font) return nk_false;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay || line_index < 0 || line_index >= lay->line_count)
        return nk_false;
    info = &lay->lines[line_index];
    if (first_rune) *first_rune = info->first;
    if (rune_count) *rune_count = info->count;
    if (line_width) *line_width = info->line_width;
    return nk_true;
}

NK_API int
nk_bidi_visual_neighbor(const char *text, int byte_len, int logical_index,
    int direction, const struct nk_user_font *font, float width)
{
    struct nk_bidi_layout *lay;
    struct nk_bidi_line_info *info;
    struct nk_bidi_line_layout *w;
    int k, vi;
    if (!nk_bidi_active || !font) return logical_index + direction;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay) return logical_index + direction;
    k = nk_bidi_line_of(lay, logical_index);
    if (k < 0) return logical_index;
    info = &lay->lines[k];
    w = &lay->work;
    /* locate logical index in the visual order of its line; BN control
     * positions are not visible so they are skipped via visible_count */
    for (vi = 0; vi < info->visible_count; ++vi) {
        if (w->visual[info->first + vi] == logical_index) {
            int nvi = vi + (direction < 0 ? -1 : 1);
            if (nvi >= 0 && nvi < info->visible_count)
                return w->visual[info->first + nvi];
            return logical_index; /* line boundary: keep position */
        }
    }
    /* index at line end (including after a trailing BN control) */
    if (logical_index == info->first + info->count &&
        info->visible_count > 0) {
        if (direction < 0)
            return w->visual[info->first + info->visible_count - 1];
    }
    return logical_index;
}

NK_API void
nk_bidi_selection_rects(const char *text, int byte_len, int line_index,
    int sel_begin, int sel_end, const struct nk_user_font *font, float width,
    float *x0_out, float *x1_out)
{
    struct nk_bidi_layout *lay;
    struct nk_bidi_line_info *info;
    struct nk_bidi_line_layout *w;
    int vi;
    float lo = 1e30f, hi = 0;
    if (x0_out) *x0_out = 0;
    if (x1_out) *x1_out = 0;
    if (!nk_bidi_active || !font) return;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay || line_index < 0 || line_index >= lay->line_count) return;
    info = &lay->lines[line_index];
    w = &lay->work;
    for (vi = 0; vi < info->visible_count; ++vi) {
        int li2 = w->visual[info->first + vi];
        if (li2 >= sel_begin && li2 < sel_end) {
            float a = w->caret_x[li2];
            float b = a + w->adv[li2];
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        }
    }
    if (hi > lo) {
        if (x0_out) *x0_out = lo;
        if (x1_out) *x1_out = hi;
    }
}

NK_API int
nk_bidi_visual_segments(const char *text, int byte_len, int line_index,
    const struct nk_user_font *font, float width,
    struct nk_bidi_visual_segment *segments, int max_segments)
{
    struct nk_bidi_layout *lay;
    struct nk_bidi_line_info *info;
    struct nk_bidi_line_layout *w;
    int vi;
    int out = 0;
    if (!nk_bidi_active || !font || !segments || max_segments <= 0) return 0;
    lay = nk_bidi_get_layout(text, byte_len, width, font);
    if (!lay || line_index < 0 || line_index >= lay->line_count) return 0;
    info = &lay->lines[line_index];
    w = &lay->work;
    for (vi = 0; vi < info->visible_count && out < max_segments; ++vi) {
        int li2 = w->visual[info->first + vi];
        nk_rune u = w->runes[li2];
        struct nk_bidi_visual_segment *s = &segments[out];
        s->byte_offset = w->bytes[li2];
        s->original = u;
        s->mapped = ((w->levels[li2] & 1) ? nk_bidi_mirror(u) : u);
        s->x = w->caret_x[li2];
        s->width = w->adv[li2];
        s->rtl = (w->levels[li2] & 1) ? nk_true : nk_false;
        out++;
    }
    return out;
}

/* ---------------- one-shot visual transform (labels) ---------------- */
struct nk_bidi_visual_cache {
    const char *text;
    int byte_len;
    float font_height;
    char *buf;
    int cap;
    int out_len;
    nk_byte base;

    /* persistent scratch; only reallocated when a longer text appears,
     * so steady-state label rendering performs no heap allocation */
    int scap;
    nk_rune *runes;
    nk_byte *raw;
    nk_ushort *levels;
    int *order;
    int *bytes;
};
NK_GLOBAL struct nk_bidi_visual_cache nk_bidi_visual_cache_g;

NK_INTERN nk_bool
nk_bidi_visual_grow(struct nk_bidi_visual_cache *c, int need)
{
    int ncap = c->scap ? c->scap : 16;
    void *p;
    while (ncap < need) ncap *= 2;
    if (ncap > NK_BIDI_MAX_RUNES) return nk_false;

    p = NK_BIDI_ALLOC(sizeof(nk_rune) * (nk_size)ncap);
    if (!p) return nk_false;
    if (c->runes) { NK_MEMCPY(p, c->runes, sizeof(nk_rune)*(nk_size)c->scap); NK_BIDI_FREE(c->runes); }
    c->runes = (nk_rune*)p;

    p = NK_BIDI_ALLOC(sizeof(nk_byte) * (nk_size)ncap);
    if (!p) return nk_false;
    if (c->raw) { NK_MEMCPY(p, c->raw, sizeof(nk_byte)*(nk_size)c->scap); NK_BIDI_FREE(c->raw); }
    c->raw = (nk_byte*)p;

    p = NK_BIDI_ALLOC(sizeof(nk_ushort) * (nk_size)ncap);
    if (!p) return nk_false;
    if (c->levels) { NK_MEMCPY(p, c->levels, sizeof(nk_ushort)*(nk_size)c->scap); NK_BIDI_FREE(c->levels); }
    c->levels = (nk_ushort*)p;

    p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
    if (!p) return nk_false;
    if (c->order) { NK_MEMCPY(p, c->order, sizeof(int)*(nk_size)c->scap); NK_BIDI_FREE(c->order); }
    c->order = (int*)p;

    p = NK_BIDI_ALLOC(sizeof(int) * (nk_size)ncap);
    if (!p) return nk_false;
    if (c->bytes) { NK_MEMCPY(p, c->bytes, sizeof(int)*(nk_size)c->scap); NK_BIDI_FREE(c->bytes); }
    c->bytes = (int*)p;

    c->scap = ncap;
    return nk_true;
}

NK_API const char*
nk_bidi_visual(const char *text, int byte_len,
    const struct nk_user_font *font, int *out_len, nk_bool *out_rtl)
{
    struct nk_bidi_visual_cache *c = &nk_bidi_visual_cache_g;
    int n = 0, off = 0, i;
    nk_byte base;

    if (out_len) *out_len = byte_len;
    if (out_rtl) *out_rtl = nk_false;
    if (!nk_bidi_active || !text || byte_len <= 0 || !font)
        return text;

    if (c->text == text && c->byte_len == byte_len &&
        c->font_height == font->height && c->buf) {
        if (out_len) *out_len = c->out_len;
        if (out_rtl) *out_rtl = (c->base & 1) ? nk_true : nk_false;
        return c->buf;
    }

    /* decode into persistent scratch (grows only when needed) */
    while (off < byte_len) {
        nk_rune u;
        int glen = nk_bidi_decode(text, byte_len, off, &u);
        if (u == NK_UTF_INVALID) goto fail;
        if (n >= c->scap) {
            if (!nk_bidi_visual_grow(c, n + 1)) goto fail;
        }
        c->runes[n] = u;
        c->bytes[n] = off;
        c->raw[n] = nk_bidi_type(u);
        c->levels[n] = 0;
        c->order[n] = n;
        n++;
        off += glen;
    }
    if (n == 0) goto fail;

    base = nk_bidi_paragraph_base(c->runes, c->raw, n);
    nk_bidi_x_rules(c->runes, c->raw, c->levels, n, base);
    {
        /* compact BN controls into the front of the scratch arrays */
        int w2 = 0;
        for (i = 0; i < n; ++i) {
            if (c->raw[i] == BD_BN) continue;
            c->runes[w2] = c->runes[i];
            c->bytes[w2] = c->bytes[i];
            c->raw[w2] = c->raw[i];
            c->levels[w2] = c->levels[i];
            c->order[w2] = w2;
            w2++;
        }
        n = w2;
    }
    if (n == 0) goto fail;
    nk_bidi_w_rules(c->raw, n);
    nk_bidi_n0_rules(c->raw, c->runes, n);
    nk_bidi_n_rules(c->raw, c->levels, n, base);
    nk_bidi_i_rules(c->raw, c->levels, n);
    nk_bidi_l1_rules(c->raw, c->levels, n, base);
    nk_bidi_l2(c->order, c->order, c->levels, n, base);

    if (c->cap < byte_len + 4) {
        int ncap = c->cap ? c->cap : 256;
        void *p;
        while (ncap < byte_len + 4) ncap *= 2;
        p = NK_BIDI_ALLOC((nk_size)ncap);
        if (!p) goto fail;
        if (c->buf) NK_BIDI_FREE(c->buf);
        c->buf = (char*)p;
        c->cap = ncap;
    }
    {
        int wp = 0;
        for (i = 0; i < n; ++i) {
            nk_rune u = c->runes[c->order[i]];
            int el;
            if (c->levels[c->order[i]] & 1)
                u = nk_bidi_mirror(u);
            el = nk_utf_encode(u, c->buf + wp, 4);
            if (el <= 0 || wp + el >= c->cap) goto fail;
            wp += el;
        }
        c->out_len = wp;
    }

    c->text = text;
    c->byte_len = byte_len;
    c->font_height = font->height;
    c->base = base;

    if (out_len) *out_len = c->out_len;
    if (out_rtl) *out_rtl = (base & 1) ? nk_true : nk_false;
    return c->buf;

fail:
    if (out_len) *out_len = byte_len;
    if (out_rtl) *out_rtl = nk_false;
    return text;
}

/* ---------------- word-wrap aware line iteration ---------------- */
NK_API int
nk_bidi_wrap_lines(const char *text, int byte_len, float width,
    const struct nk_user_font *font, struct nk_bidi_wrap_line *out,
    int max_lines)
{
    int off = 0;
    int lines = 0;
    if (!nk_bidi_active || !text || byte_len <= 0 || !font || !out)
        return 0;

    while (off < byte_len && lines < max_lines) {
        int line_start = off;
        int word_start = off;
        float x = 0;
        struct nk_bidi_wrap_line *wl = &out[lines];
        nk_zero_struct(*wl);
        while (off < byte_len) {
            nk_rune u;
            int glen = nk_bidi_decode(text, byte_len, off, &u);
            float adv;
            if (u == NK_UTF_INVALID) { off = byte_len; break; }
            if (u == '\n') { off += glen; goto emit; }
            adv = font->width(font->userdata, font->height, text + off, glen);
            if (u == ' ' || u == '\t') word_start = off + glen;
            if (width > 0 && x + adv > width && off > line_start) {
                if (word_start > line_start && word_start < off)
                    off = word_start;
                goto emit;
            }
            x += adv;
            off += glen;
        }
emit:
        wl->byte_offset = line_start;
        wl->byte_length = off - line_start;
        wl->line_width = x;
        wl->rtl = nk_bidi_text_direction(text + line_start,
            off - line_start);
        lines++;
    }
    return lines;
}

#endif /* NK_INCLUDE_BIDI */
