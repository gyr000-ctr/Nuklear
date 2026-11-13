/*
 * tests/bidi_test.c
 *
 * Unit tests for the optional Unicode bidirectional text support
 * (NK_INCLUDE_BIDI). Covers:
 *
 *  - paragraph base direction for Arabic/Hebrew/LTR/mixed text
 *  - visual reordering and mirrored punctuation
 *  - European and Arabic numbers inside RTL text
 *  - nested embedding controls and unmatched control recovery
 *  - empty paragraphs, overlong runs, malformed UTF-8 fallback
 *  - word wrapping and clipping
 *  - caret hit-testing, logical<->visual mapping and selection extents
 *  - text editor caret/selection behavior on RTL content
 *  - byte-identical LTR output while bidi is disabled
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -I.. -DNK_INCLUDE_BIDI \
 *       -DNK_IMPLEMENTATION bidi_test.c -o bidi_test -lm
 */
#ifndef NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_FIXED_TYPES
#endif
#ifndef NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_IO
#endif
#ifndef NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_STANDARD_VARARGS
#endif
#ifndef NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#endif
#ifndef NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#endif
#ifndef NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_FONT_BAKING
#endif
#ifndef NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_DEFAULT_FONT
#endif
#ifndef NK_INCLUDE_BIDI
#define NK_INCLUDE_BIDI
#endif
#ifndef NK_IMPLEMENTATION
#define NK_IMPLEMENTATION
#endif
#include "nuklear.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_failures;
static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } } while (0)

#define CHECK_EQ_INT(a, b) do { \
    int _va = (int)(a), _vb = (int)(b); \
    g_checks++; \
    if (_va != _vb) { \
        g_failures++; \
        printf("FAIL %s:%d: %s == %d, got %d (expected %d)\n", \
            __FILE__, __LINE__, #a, _va, _va, _vb); \
    } } while (0)

/* ---------------- test font: every rune is 8px wide ---------------- */
static float
test_font_width(nk_handle handle, float height, const char *text, int len)
{
    int runes = nk_utf_len(text, len);
    NK_UNUSED(handle); NK_UNUSED(height);
    return (float)runes * 8.0f;
}

static void
test_font_query(nk_handle handle, float height,
    struct nk_user_font_glyph *glyph, nk_rune cp, nk_rune next)
{
    NK_UNUSED(handle); NK_UNUSED(height); NK_UNUSED(cp); NK_UNUSED(next);
    glyph->width = 8.0f;
    glyph->height = height;
    glyph->xadvance = 8.0f;
    glyph->offset = nk_vec2(0, 0);
    glyph->uv[0] = nk_vec2(0, 0);
    glyph->uv[1] = nk_vec2(1, 1);
}

static struct nk_user_font*
make_font(void)
{
    static struct nk_user_font font;
    memset(&font, 0, sizeof(font));
    font.height = 16.0f;
    font.width = test_font_width;
    font.query = test_font_query;
    font.userdata = nk_handle_ptr(0);
    return &font;
}

/* decode helpers for assertions */
static int
runes_in(const char *s, int len)
{
    return nk_utf_len(s, len);
}

static nk_rune
first_rune(const char *s)
{
    nk_rune u = 0;
    nk_utf_decode(s, &u, 8);
    return u;
}

/* ============================================================= */
/* Paragraph base direction                                       */
/* ============================================================= */
static void
test_base_direction(void)
{
    /* "abc" -> LTR */
    CHECK(nk_bidi_text_direction("abc", 3) == nk_false);
    /* Hebrew aleph (U+05D0) -> RTL */
    const char hebrew[] = {(char)0xD7, (char)0x90, 0};
    CHECK(nk_bidi_text_direction(hebrew, 2) == nk_true);
    /* Arabic alef (U+0627) -> RTL */
    const char arabic[] = {(char)0xD8, (char)0xA7, 0};
    CHECK(nk_bidi_text_direction(arabic, 2) == nk_true);
    /* digits/punctuation first, then Arabic -> RTL */
    const char mixed_num[] = "123 "
        "\xD8\xA7" "\xD8\xA8" "\xD8\xA9";
    CHECK(nk_bidi_text_direction(mixed_num, (int)strlen(mixed_num))
        == nk_true);
    /* LTR strong first even with later Arabic */
    const char mixed_ltr[] = "a " "\xD8\xA7" "\xD8\xA8";
    CHECK(nk_bidi_text_direction(mixed_ltr, (int)strlen(mixed_ltr))
        == nk_false);
    /* only weak/neutral content defaults to LTR */
    CHECK(nk_bidi_text_direction("123 456...", 10) == nk_false);
    /* empty/NULL is safe */
    CHECK(nk_bidi_text_direction("", 0) == nk_false);
    CHECK(nk_bidi_text_direction(0, 0) == nk_false);
}

/* ============================================================= */
/* Visual reordering                                              */
/* ============================================================= */
static void
test_visual_reorder(void)
{
    struct nk_user_font *font = make_font();
    const char *vis;
    int out_len = 0;
    nk_bool rtl = nk_false;

    /* Pure Arabic: three runes ABa (alef, beh, teh) must reverse */
    const char ar[] = "\xD8\xA7" "\xD8\xA8" "\xD8\xAA";
    vis = nk_bidi_visual(ar, 6, font, &out_len, &rtl);
    CHECK(rtl == nk_true);
    CHECK_EQ_INT(runes_in(vis, out_len), 3);
    CHECK(first_rune(vis) == 0x062A); /* teh first visually */

    /* Mixed: two Arabic runes followed by "ABC" in an RTL paragraph:
     * logical: a1 a2 A B C. L runes resolve to level 2, Arabic to 1.
     * Visual order: C B A a2 a1 */
    const char mix[] = "\xD8\xA7" "\xD8\xA8" "ABC";
    vis = nk_bidi_visual(mix, 7, font, &out_len, &rtl);
    CHECK(rtl == nk_true);
    {
        nk_rune u; int glen; int pos = 0;
        nk_rune order[5]; int k = 0;
        while (pos < out_len && k < 5) {
            glen = nk_utf_decode(vis + pos, &u, out_len - pos);
            CHECK(glen > 0);
            order[k++] = u;
            pos += glen;
        }
        CHECK_EQ_INT(k, 5);
        CHECK(order[0] == 'C');
        CHECK(order[1] == 'B');
        CHECK(order[2] == 'A');
        CHECK(order[3] == 0x0628);
        CHECK(order[4] == 0x0627);
    }

    /* Pure LTR stays unchanged */
    vis = nk_bidi_visual("hello", 5, font, &out_len, &rtl);
    CHECK(rtl == nk_false);
    CHECK(out_len == 5);
    CHECK(memcmp(vis, "hello", 5) == 0);
}

/* ============================================================= */
/* Numbers inside RTL text                                       */
/* ============================================================= */
static void
test_numbers(void)
{
    struct nk_user_font *font = make_font();
    const char *vis;
    int out_len = 0;
    nk_bool rtl = nk_false;

    /* Arabic runes around a European number (RTL base paragraph):
     * logical: a1 a2 '1' '2' '3' a3
     * levels:  1  1   2  2  2  1
     * L2 yields visual: a3 1 2 3 a2 a1 */
    const char s[] = "\xD8\xA7" "\xD8\xA8" "123" "\xD8\xAA";
    vis = nk_bidi_visual(s, 9, font, &out_len, &rtl);
    CHECK(rtl == nk_true);
    {
        nk_rune u; int glen; int pos = 0;
        nk_rune order[6]; int k = 0;
        while (pos < out_len && k < 6) {
            glen = nk_utf_decode(vis + pos, &u, out_len - pos);
            CHECK(glen > 0);
            order[k++] = u;
            pos += glen;
        }
        CHECK_EQ_INT(k, 6);
        CHECK(order[0] == 0x062A);
        CHECK(order[1] == '1');
        CHECK(order[2] == '2');
        CHECK(order[3] == '3');
        CHECK(order[4] == 0x0628);
        CHECK(order[5] == 0x0627);
    }

    /* Arabic-Indic digits (AN) should not crash and stay ordered */
    const char an[] = "\xD9\xA0\xD9\xA1\xD9\xA2"; /* U+0660..0662 */
    vis = nk_bidi_visual(an, 6, font, &out_len, &rtl);
    CHECK(out_len == 6);
    CHECK(runes_in(vis, out_len) == 3);
}

/* ============================================================= */
/* Mirrored punctuation (L4)                                     */
/* ============================================================= */
static void
test_mirroring(void)
{
    struct nk_user_font *font = make_font();
    const char *vis;
    int out_len = 0;
    nk_bool rtl = nk_false;

    /* "(abc)" inside an RTL paragraph: opening '(' must map to ')' etc. */
    const char s[] = "\xD8\xA7" "(abc)";
    vis = nk_bidi_visual(s, 8, font, &out_len, &rtl);
    CHECK(rtl == nk_true);
    {
        nk_rune u; int glen; int pos = 0;
        int saw_close = 0, saw_open = 0;
        while (pos < out_len) {
            glen = nk_utf_decode(vis + pos, &u, out_len - pos);
            CHECK(glen > 0);
            if (u == ')') saw_close++;
            if (u == '(') saw_open++;
            pos += glen;
        }
        CHECK(saw_close == 1);
        CHECK(saw_open == 1);
    }

    /* brackets and square brackets */
    const char s2[] = "\xD8\xA7" "[x]";
    vis = nk_bidi_visual(s2, 6, font, &out_len, &rtl);
    {
        nk_rune u; int glen; int pos = 0;
        int saw_rsq = 0, saw_lsq = 0;
        while (pos < out_len) {
            glen = nk_utf_decode(vis + pos, &u, out_len - pos);
            if (u == ']') saw_rsq++;
            if (u == '[') saw_lsq++;
            pos += glen;
        }
        CHECK(saw_rsq == 1);
        CHECK(saw_lsq == 1);
    }
}

/* ============================================================= */
/* Explicit embedding controls / unmatched controls              */
/* ============================================================= */
static void
test_embedding_controls(void)
{
    struct nk_user_font *font = make_font();
    const char *vis;
    int out_len = 0;
    nk_bool rtl = nk_false;

    /* RLE (U+202B) + Latin + PDF: Latin inside an RTL embedding takes
     * level 2 and is visually reversed relative to the paragraph.
     * The control characters themselves must not appear in output.
     * 3-byte RLE + ABC + 3-byte PDF = 9 bytes, 5 runes in, 3 out. */
    const char s[] = "\xE2\x80\xAB" "ABC" "\xE2\x80\xAC";
    vis = nk_bidi_visual(s, 9, font, &out_len, &rtl);
    CHECK(runes_in(vis, out_len) == 3);
    {
        nk_rune u; int glen; int pos = 0;
        nk_rune order[3]; int k = 0;
        while (pos < out_len && k < 3) {
            glen = nk_utf_decode(vis + pos, &u, out_len - pos);
            order[k++] = u;
            pos += glen;
        }
        CHECK(order[0] == 'C');
        CHECK(order[1] == 'B');
        CHECK(order[2] == 'A');
    }

    /* unmatched PDF must be tolerated (no crash, simply dropped).
     * 7 bytes = 4 content runes + one 3-byte PDF control */
    const char bad[] = "AB" "\xE2\x80\xAC" "CD";
    vis = nk_bidi_visual(bad, 7, font, &out_len, &rtl);
    CHECK(runes_in(vis, out_len) == 4);

    /* many unmatched RLEs beyond stack capacity must not overflow */
    {
        char big[300];
        int p = 0, i;
        for (i = 0; i < 90 && p + 3 < (int)sizeof(big) - 2; ++i) {
            big[p++] = (char)0xE2; big[p++] = (char)0x80; big[p++] = (char)0xAB;
        }
        big[p++] = 'x';
        vis = nk_bidi_visual(big, p, font, &out_len, &rtl);
        CHECK(runes_in(vis, out_len) >= 1);
    }

    /* LRI (U+2066 = E2 81 A6) ... PDI (U+2069 = E2 81 A9) isolate
     * handling: 3 content runes, controls dropped (9 bytes) */
    const char iso[] = "\xE2\x81\xA6" "AB" "\xE2\x81\xA9" "C";
    vis = nk_bidi_visual(iso, 9, font, &out_len, &rtl);
    CHECK(runes_in(vis, out_len) == 3);
}

/* ============================================================= */
/* Empty paragraph / malformed input / overlong run               */
/* ============================================================= */
static void
test_degenerate_inputs(void)
{
    struct nk_user_font *font = make_font();
    int out_len = 0;
    nk_bool rtl = nk_true;

    /* empty string: identity result, no crash */
    const char *vis = nk_bidi_visual("", 0, font, &out_len, &rtl);
    CHECK(vis != 0);
    CHECK(out_len == 0);

    /* malformed UTF-8: falls back to identity/LTR */
    const char broken[] = {(char)0xFF, (char)0xFE, 'a', 0};
    rtl = nk_true;
    vis = nk_bidi_visual(broken, 3, font, &out_len, &rtl);
    CHECK(vis == broken);
    CHECK(rtl == nk_false);
    CHECK(out_len == 3);

    /* truncated multibyte sequence */
    const char trunc[] = {(char)0xD8, 0};
    vis = nk_bidi_visual(trunc, 1, font, &out_len, &rtl);
    CHECK(vis == trunc);

    /* very long pure-LTR run: must complete and not allocate per frame */
    {
        int n = 20000;
        char *big = (char*)malloc((nk_size)n + 1);
        int i;
        for (i = 0; i < n; ++i) big[i] = (char)('a' + (i % 26));
        big[n] = 0;
        vis = nk_bidi_visual(big, n, font, &out_len, &rtl);
        CHECK(vis != 0);
        CHECK_EQ_INT(runes_in(vis, out_len), n);
        /* second call reuses the cache */
        vis = nk_bidi_visual(big, n, font, &out_len, &rtl);
        CHECK_EQ_INT(out_len, n);
        free(big);
    }
}

/* ============================================================= */
/* Wrapping and line breaking                                     */
/* ============================================================= */
static void
test_wrapping(void)
{
    struct nk_user_font *font = make_font();
    struct nk_bidi_wrap_line lines[16];
    int n;
    const char text[] = "one two three four";

    /* width 24 => 3 runes per line (8px each) */
    n = nk_bidi_wrap_lines(text, (int)strlen(text), 24.0f, font,
        lines, 16);
    CHECK(n >= 4);
    CHECK(lines[0].byte_length >= 3);

    /* width 0 disables wrapping; single line */
    n = nk_bidi_wrap_lines(text, (int)strlen(text), 0, font, lines, 16);
    CHECK_EQ_INT(n, 1);
    CHECK_EQ_INT(lines[0].byte_length, (int)strlen(text));

    /* newlines force breaks */
    const char nl[] = "ab\ncd\nef";
    n = nk_bidi_wrap_lines(nl, (int)strlen(nl), 0, font, lines, 16);
    CHECK_EQ_INT(n, 3);

    /* RTL wrapped line reports RTL direction */
    const char ar[] = "\xD8\xA7" "\xD8\xA8" " " "\xD8\xAA";
    n = nk_bidi_wrap_lines(ar, 7, 100.0f, font, lines, 16);
    CHECK_EQ_INT(n, 1);
    CHECK(lines[0].rtl == nk_true);
}

/* ============================================================= */
/* Caret hit testing and visual neighbor mapping                  */
/* ============================================================= */
static void
test_caret_mapping(void)
{
    struct nk_user_font *font = make_font();
    /* RTL base paragraph: alef beh A B C
     * logical indices 0..4; levels 1 1 2 2 2
     * visual order: C(4) B(3) A(2) beh(1) alef(0) */
    const char s[] = "\xD8\xA7" "\xD8\xA8" "ABC";
    int hit;
    float x;
    int line = -1;
    int neighbor;

    /* click at far left (x=0) must hit the visually-first rune,
     * which is the last logical rune (C, index 4) */
    hit = nk_bidi_hit_test(s, 7, 0, 1.0f, font, 0);
    CHECK_EQ_INT(hit, 4);

    /* click far right (past the last glyph center) returns the line end,
     * i.e. logical index 5 (one past the 5 runes) */
    hit = nk_bidi_hit_test(s, 7, 0, 5 * 8.0f - 1.0f, font, 0);
    CHECK_EQ_INT(hit, 5);

    /* clicking on the rightmost glyph (alef, index 0) hits index 0.
     * alef visually occupies x in [32,40), click its left half at 33 */
    hit = nk_bidi_hit_test(s, 7, 0, 33.0f, font, 0);
    CHECK_EQ_INT(hit, 0);

    /* caret x for logical index 0 (alef) is at the right edge */
    x = nk_bidi_caret_x(s, 7, 0, &line, font, 0);
    CHECK_EQ_INT(line, 0);
    CHECK(x > 4 * 8.0f - 0.001f);

    /* caret x for the visually-first logical rune (index 4, C) is zero */
    x = nk_bidi_caret_x(s, 7, 4, &line, font, 0);
    CHECK(x < 0.001f);

    /* visual right from index 4 (C) reaches index 3 (B) */
    neighbor = nk_bidi_visual_neighbor(s, 7, 4, 1, font, 0);
    CHECK_EQ_INT(neighbor, 3);
    /* visual left from index 0 (alef) reaches index 1 (beh) */
    neighbor = nk_bidi_visual_neighbor(s, 7, 0, -1, font, 0);
    CHECK_EQ_INT(neighbor, 1);

    /* selection rect covers the selected visual extent */
    {
        float x0 = -1, x1 = -1;
        /* select logical 0..1 (the two Arabic runes), visually at the
         * rightmost 16px: x in [24, 40) */
        nk_bidi_selection_rects(s, 7, 0, 0, 2, font, 0, &x0, &x1);
        CHECK(x0 >= 3 * 8.0f - 0.001f);
        CHECK(x1 > x0);
        CHECK(x1 <= 5 * 8.0f + 0.001f);
        /* an empty (zero-width) selection yields an empty rect */
        x0 = x1 = -1;
        nk_bidi_selection_rects(s, 7, 0, 2, 2, font, 0, &x0, &x1);
        CHECK(x0 == 0 && x1 == 0);
    }
}

/* ============================================================= */
/* Text editor integration                                        */
/* ============================================================= */
static void
test_text_editor(void)
{
    struct nk_user_font *font = make_font();
    struct nk_text_edit edit;
    char memory[512];
    /* RTL base: alef beh A B -> visual B A beh alef */
    const char rtl_text[] = "\xD8\xA7" "\xD8\xA8" "AB";

    nk_textedit_init_fixed(&edit, memory, sizeof(memory));
    edit.mode = NK_TEXT_EDIT_MODE_INSERT;
    /* nk_textedit_paste takes a rune count, not a byte count */
    nk_textedit_paste(&edit, rtl_text, nk_utf_len(rtl_text,
        (int)strlen(rtl_text)));
    CHECK_EQ_INT(edit.string.len, 4);

    /* click at the visual left edge (B's half) must place caret at the
     * last logical content index */
    nk_textedit_click(&edit, 1.0f, 1.0f, font, font->height);
    CHECK_EQ_INT(edit.cursor, 3);
    CHECK_EQ_INT(edit.select_start, edit.select_end);

    /* click on the visually-rightmost glyph (alef) hits logical index 0.
     * Visual order B A beh alef; alef occupies x in [24,32) */
    nk_textedit_click(&edit, 25.0f, 1.0f, font, font->height);
    CHECK_EQ_INT(edit.cursor, 0);

    /* shift+right extends the selection by the visual neighbor. From
     * index 3 (A) the visual-right neighbor is index 2 (beh) */
    nk_textedit_click(&edit, 1.0f, 1.0f, font, font->height);
    nk_textedit_key(&edit, NK_KEY_RIGHT, 1, font, font->height);
    CHECK(edit.select_end != edit.select_start);
    CHECK_EQ_INT(edit.cursor, 2);

    /* multiline: caret y changes across newline lines */
    {
        struct nk_text_edit e2;
        char mem2[256];
        const char two[] = "ab\n" "\xD8\xA7" "\xD8\xA8";
        int line = -1;
        float cx;
        nk_textedit_init_fixed(&e2, mem2, sizeof(mem2));
        e2.mode = NK_TEXT_EDIT_MODE_INSERT;
        nk_textedit_paste(&e2, two, nk_utf_len(two, (int)strlen(two)));
        CHECK_EQ_INT(e2.string.len, 5);
        /* logical index 3 is on the second line */
        cx = nk_bidi_caret_x(nk_str_get_const(&e2.string),
            nk_str_len_char(&e2.string), 3, &line, font, 0);
        CHECK_EQ_INT(line, 1);
        CHECK(cx >= 0.0f);
        /* hit test on line 1 */
        {
            int hit = nk_bidi_hit_test(nk_str_get_const(&e2.string),
                nk_str_len_char(&e2.string), 1, 1.0f, font, 0);
            CHECK(hit >= 3);
        }
        nk_textedit_free(&e2);
    }

    nk_textedit_free(&edit);
}

/* ============================================================= */
/* Disabled mode is identical to LTR behavior                     */
/* ============================================================= */
static void
test_disabled_identity(void)
{
    struct nk_user_font *font = make_font();
    const char ar[] = "\xD8\xA7" "\xD8\xA8" "\xD8\xAA";
    const char *vis;
    int out_len = 0;
    nk_bool rtl = nk_true;

    nk_bidi_enable(nk_false);
    CHECK(nk_bidi_is_enabled() == nk_false);
    vis = nk_bidi_visual(ar, 6, font, &out_len, &rtl);
    CHECK(vis == ar);             /* identity pointer */
    CHECK(out_len == 6);
    CHECK(rtl == nk_false);
    CHECK(nk_bidi_hit_test(ar, 6, 0, 0.0f, font, 0) == -1);
    CHECK_EQ_INT(nk_bidi_line_count(ar, 6, font, 0), 0);
    nk_bidi_enable(nk_true);
}

/* ============================================================= */
/* Cache reuse: no repeated allocation for identical text         */
/* ============================================================= */
static void
test_cache_reuse(void)
{
    struct nk_user_font *font = make_font();
    const char s[] = "\xD8\xA7" "\xD8\xA8" "abc"; /* 7 bytes, 5 runes */
    const char *v1, *v2;
    int l1 = 0, l2 = 0;
    nk_bool r1 = nk_false, r2 = nk_false;
    v1 = nk_bidi_visual(s, 7, font, &l1, &r1);
    v2 = nk_bidi_visual(s, 7, font, &l2, &r2);
    CHECK(v1 == v2);            /* same cache buffer reused */
    CHECK(l1 == l2);
    CHECK(r1 == r2);
}

/* ============================================================= */
/* Mixed-script boundaries and multi-paragraph content            */
/* ============================================================= */
static void
test_mixed_and_paragraphs(void)
{
    struct nk_user_font *font = make_font();
    struct nk_bidi_wrap_line lines[16];
    int n;
    int out_len = 0;
    nk_bool rtl = nk_false;
    const char *vis;

    /* Hebrew + Arabic + Latin + digits + punctuation in one paragraph */
    const char mix[] =
        "\xD7\x90"                 /* Hebrew alef   */
        "\xD8\xA7"                 /* Arabic alef   */
        "x12y,"
        "\xD7\x91";                /* Hebrew bet    */
    vis = nk_bidi_visual(mix, (int)strlen(mix), font, &out_len, &rtl);
    CHECK(rtl == nk_true);
    /* no rune lost or duplicated */
    CHECK_EQ_INT(nk_utf_len(vis, out_len),
        nk_utf_len(mix, (int)strlen(mix)));

    /* several newline-separated paragraphs, including an empty one */
    {
        const char para[] = "abc\n" "\xD8\xA7" "\xD8\xA8" "\n\n" "end";
        n = nk_bidi_wrap_lines(para, (int)strlen(para), 0, font,
            lines, 16);
        CHECK_EQ_INT(n, 4);                 /* ab, ar, empty, end */
        CHECK(lines[0].rtl == nk_false);
        CHECK(lines[1].rtl == nk_true);
        /* the empty paragraph line contains only its newline byte */
        CHECK(lines[2].byte_length == 1);
        CHECK(lines[3].rtl == nk_false);
        /* line count through the cached layout matches */
        CHECK_EQ_INT(nk_bidi_line_count(para, (int)strlen(para),
            font, 0), 4);
    }

    /* empty string layout is safe and reports no lines */
    CHECK_EQ_INT(nk_bidi_line_count("", 0, font, 0), 0);
    {
        int first = -1, cnt = -1;
        float lw = -1;
        CHECK(nk_bidi_line_info("", 0, 0, &first, &cnt, &lw, font, 0)
            == nk_false);
    }

    /* hit testing beyond the last line fails gracefully */
    CHECK(nk_bidi_hit_test("ab", 2, 9, 0.0f, font, 0) == -1);
}

/* ============================================================= */
/* Steady-state layout does not depend on frame count             */
/* ============================================================= */
static void
test_steady_state(void)
{
    struct nk_user_font *font = make_font();
    const char s[] = "a " "\xD8\xA7" " 12 " "\xD7\x90" " b";
    int i;
    int ref_len = 0;
    const char *ref = 0;
    nk_bool rtl = nk_false;

    ref = nk_bidi_visual(s, (int)strlen(s), font, &ref_len, &rtl);
    /* many frames with the same text must keep returning the same buffer
     * and produce identical bytes (ASan verifies no leak/overflow) */
    for (i = 0; i < 1000; ++i) {
        int ol = 0;
        const char *v = nk_bidi_visual(s, (int)strlen(s), font, &ol, 0);
        CHECK(v == ref);
        CHECK(ol == ref_len);
        CHECK(memcmp(v, ref, (nk_size)ol) == 0);
    }

    /* interleaving different texts keeps each result correct */
    {
        int la = 0, lb = 0;
        const char *va = nk_bidi_visual("abc", 3, font, &la, 0);
        CHECK_EQ_INT(la, 3);
        CHECK(memcmp(va, "abc", 3) == 0);
        /* RTL text replaces the single-entry visual cache */
        {
            const char *vb = nk_bidi_visual(
                "\xD8\xA7" "\xD8\xA8", 4, font, &lb, 0);
            CHECK_EQ_INT(nk_utf_len(vb, lb), 2);
        }
        /* going back to the LTR text still yields the identity result */
        la = 0;
        va = nk_bidi_visual("abc", 3, font, &la, 0);
        CHECK_EQ_INT(la, 3);
        CHECK(memcmp(va, "abc", 3) == 0);
    }
}

/* ============================================================= */
/* Explicit controls do not disturb caret/line indices             */
/* ============================================================= */
static void
test_control_caret_boundaries(void)
{
    struct nk_user_font *font = make_font();
    /* RLE + three Latin + PDF; only 3 visible runes remain */
    const char s[] = "\xE2\x80\xAB" "ABC" "\xE2\x80\xAC";
    int first = -1, cnt = -1;
    float lw = -1;
    int hit;

    CHECK_EQ_INT(nk_bidi_line_count(s, 9, font, 0), 1);
    CHECK(nk_bidi_line_info(s, 9, 0, &first, &cnt, &lw, font, 0));
    CHECK_EQ_INT(first, 0);
    /* logical rune count keeps the two formatting controls (RLE/PDF) so
     * editor indices remain stable; only visible glyphs are reordered */
    CHECK_EQ_INT(cnt, 5);

    /* caret positions cover exactly the three visible glyphs */
    CHECK(nk_bidi_caret_x(s, 9, 0, 0, font, 0) >= 0.0f);
    CHECK(nk_bidi_caret_x(s, 9, 5, 0, font, 0) <= 3 * 8.0f + 0.001f);

    /* hit testing stays inside the logical range */
    hit = nk_bidi_hit_test(s, 9, 0, 1.0f, font, 0);
    CHECK(hit >= 0 && hit <= 5);

    /* unmatched controls on an otherwise LTR string still give a valid
     * identity-style layout rather than failing */
    {
        const char u[] = "X" "\xE2\x80\xAC" "Y";
        int ol = 0;
        const char *v = nk_bidi_visual(u, 5, font, &ol, 0);
        CHECK_EQ_INT(nk_utf_len(v, ol), 2);
    }
}

int
main(void)
{
    nk_bidi_enable(nk_true);
    CHECK(nk_bidi_is_enabled() == nk_true);

    test_base_direction();
    test_visual_reorder();
    test_numbers();
    test_mirroring();
    test_embedding_controls();
    test_degenerate_inputs();
    test_wrapping();
    test_caret_mapping();
    test_text_editor();
    test_mixed_and_paragraphs();
    test_control_caret_boundaries();
    test_steady_state();
    test_disabled_identity();
    test_cache_reuse();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
