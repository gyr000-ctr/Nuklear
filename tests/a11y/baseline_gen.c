/*
 * Deterministic accessibility snapshot sample and regression baseline.
 *
 * Builds a fixed UI exactly once and prints its semantic tree as JSON.
 * Frame sequence, widget count, labels and values are fixed so the
 * output is byte-for-byte reproducible across runs and platforms.
 *
 * Build: make -C tests/a11y
 */
#include <stdio.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_IMPLEMENTATION
#include "nuklear.h"

static float
sample_width(nk_handle h, float hgt, const char *txt, int len)
{
    (void)h; (void)hgt; (void)txt;
    return (float)len * 8.0f;
}

int
main(void)
{
    static struct nk_context ctx;
    static struct nk_user_font font;
    struct nk_allocator alloc;
    char json[16384];
    int len;
    nk_bool agree = nk_true;
    float slider = 30.0f;
    nk_size progress = 40;
    char edit[64];
    int row;

    memset(&alloc, 0, sizeof(alloc));
    alloc.alloc = nk_malloc;
    alloc.free = nk_mfree;
    font.height = 16.0f;
    font.width = sample_width;
    if (!nk_init(&ctx, &alloc, &font)) return 2;
    if (!nk_a11y_enable(&ctx, &alloc)) return 3;

    strcpy(edit, "a\"b\\c\nd\x01");

    /* one and only one frame, so every sequence number is fixed */
    nk_clear(&ctx);
    nk_input_begin(&ctx);
    if (nk_begin(&ctx, "Main", nk_rect(10, 10, 420, 760),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(&ctx, 28, 2);
        nk_button_label(&ctx, "OK");
        nk_button_label(&ctx, "Cancel");
        nk_checkbox_label(&ctx, "Agree", &agree);
        nk_option_label(&ctx, "A", nk_true);
        nk_option_label(&ctx, "B", nk_false);

        nk_layout_row_dynamic(&ctx, 24, 1);
        nk_edit_string_zero_terminated(&ctx, NK_EDIT_FIELD,
            edit, (int)sizeof(edit), nk_filter_default);
        nk_slider_float(&ctx, 0.0f, &slider, 100.0f, 1.0f);
        nk_progress(&ctx, &progress, 100, nk_false);

        nk_widget_disable_begin(&ctx);
        nk_layout_row_dynamic(&ctx, 26, 1);
        nk_button_label(&ctx, "Disabled Button");
        nk_widget_disable_end(&ctx);

        nk_layout_row_dynamic(&ctx, 150, 1);
        if (nk_group_begin(&ctx, "Items", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            nk_layout_row_dynamic(&ctx, 22, 1);
            for (row = 0; row < 4; ++row) {
                char label[32];
                nk_bool sel = nk_false;
                snprintf(label, sizeof(label), "Row %d", row);
                nk_selectable_label(&ctx, label, NK_TEXT_LEFT, &sel);
            }
            nk_group_end(&ctx);
        }

        nk_layout_row_dynamic(&ctx, 24, 1);
        nk_button_label(&ctx, "Same");
        nk_button_label(&ctx, "Same");
        nk_end(&ctx);
    }
    nk_input_end(&ctx);

    len = nk_a11y_snapshot_dump_json(&ctx, json, (int)sizeof(json));
    if (len <= 0) { nk_free(&ctx); return 4; }
    fwrite(json, 1, (size_t)len, stdout);

    nk_a11y_disable(&ctx);
    nk_free(&ctx);
    return 0;
}
