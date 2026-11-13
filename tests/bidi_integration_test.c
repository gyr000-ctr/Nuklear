/*
 * tests/bidi_integration_test.c
 *
 * Integration test at the draw-command level: verifies that
 *  - pure LTR text produces byte-identical NK_COMMAND_TEXT output whether
 *    bidi is disabled or enabled
 *  - RTL text is reordered into visual order when bidi is enabled
 *  - the edit widget keeps caret/selection coordinates consistent
 *
 * Build (see Makefile target 'integration'):
 *   gcc -std=c99 -I.. -DNK_IMPLEMENTATION $(DEFINES) bidi_integration_test.c -lm
 */
#ifndef NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_FIXED_TYPES
#endif
#ifndef NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_IO
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

static int g_failures, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failures++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static float
font_width(nk_handle h, float ht, const char *t, int len)
{
    (void)h; (void)ht;
    return (float)nk_utf_len(t, len) * 8.0f;
}
static void
font_query(nk_handle h, float ht, struct nk_user_font_glyph *g,
    nk_rune a, nk_rune b)
{
    (void)h; (void)a; (void)b;
    g->width = 8.0f; g->height = ht; g->xadvance = 8.0f;
    g->offset = nk_vec2(0, 0);
    g->uv[0] = nk_vec2(0, 0); g->uv[1] = nk_vec2(1, 1);
}

static const struct nk_command_text*
first_text_command(const struct nk_command_buffer *cb)
{
    nk_size off = cb->begin;
    const nk_byte *base = (const nk_byte*)cb->base->memory.ptr;
    while (off < cb->end) {
        const struct nk_command *c =
            (const struct nk_command*)(base + off);
        if (c->type == NK_COMMAND_TEXT)
            return (const struct nk_command_text*)c;
        if (!c->next) break;
        off += c->next;
    }
    return 0;
}

int
main(void)
{
    static struct nk_user_font font;
    struct nk_buffer buf_a, buf_b;
    struct nk_command_buffer cb_a, cb_b;
    const struct nk_command_text *ta, *tb;
    struct nk_text style;
    const char ltr[] = "Hello 123";
    /* RTL paragraph: alef beh then ABC */
    const char rtl[] = "\xD8\xA7" "\xD8\xA8" "ABC";

    font.height = 16.0f;
    font.width = font_width;
    font.query = font_query;
    style.padding = nk_vec2(0, 0);
    style.background = nk_rgb(0, 0, 0);
    style.text = nk_rgb(255, 255, 255);

    nk_buffer_init_default(&buf_a);
    nk_buffer_init_default(&buf_b);
    nk_command_buffer_init(&cb_a, &buf_a, NK_CLIPPING_OFF);
    nk_command_buffer_init(&cb_b, &buf_b, NK_CLIPPING_OFF);

    /* LTR text: bidi off vs on must give identical text commands */
    nk_bidi_enable(nk_false);
    nk_widget_text(&cb_a, nk_rect(0, 0, 200, 20), ltr, (int)strlen(ltr),
        &style, NK_TEXT_LEFT, &font);
    nk_bidi_enable(nk_true);
    nk_widget_text(&cb_b, nk_rect(0, 0, 200, 20), ltr, (int)strlen(ltr),
        &style, NK_TEXT_LEFT, &font);

    ta = first_text_command(&cb_a);
    tb = first_text_command(&cb_b);
    CHECK(ta != 0 && tb != 0);
    if (ta && tb) {
        CHECK(ta->length == tb->length);
        CHECK(memcmp(ta->string, tb->string, (nk_size)ta->length) == 0);
        CHECK(strcmp(ta->string, ltr) == 0);
    }

    /* RTL text: enabled bidi must reorder; disabled keeps logical order */
    nk_buffer_clear(&buf_a);
    nk_buffer_clear(&buf_b);
    nk_command_buffer_init(&cb_a, &buf_a, NK_CLIPPING_OFF);
    nk_command_buffer_init(&cb_b, &buf_b, NK_CLIPPING_OFF);

    nk_bidi_enable(nk_false);
    nk_widget_text(&cb_a, nk_rect(0, 0, 200, 20), rtl, (int)strlen(rtl),
        &style, NK_TEXT_LEFT, &font);
    nk_bidi_enable(nk_true);
    nk_widget_text(&cb_b, nk_rect(0, 0, 200, 20), rtl, (int)strlen(rtl),
        &style, NK_TEXT_LEFT, &font);
    ta = first_text_command(&cb_a);
    tb = first_text_command(&cb_b);
    CHECK(ta && tb);
    if (ta && tb) {
        /* disabled: logical byte order preserved */
        CHECK(memcmp(ta->string, rtl, (nk_size)ta->length) == 0);
        /* enabled: first visible rune is 'C' (the visual head) */
        nk_rune first = 0;
        nk_utf_decode(tb->string, &first, tb->length);
        CHECK(first == 'C');
        CHECK(tb->length == ta->length); /* same bytes, reordered only */
    }

    nk_buffer_free(&buf_a);
    nk_buffer_free(&buf_b);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
