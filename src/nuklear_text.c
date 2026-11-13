#include "nuklear.h"
#include "nuklear_internal.h"

/* ===============================================================
 *
 *                              TEXT
 *
 * ===============================================================*/
NK_LIB void
nk_widget_text(struct nk_command_buffer *o, struct nk_rect b,
    const char *string, int len, const struct nk_text *t,
    nk_flags a, const struct nk_user_font *f)
{
    struct nk_rect label;
    float text_width;
    const char *draw_string = string;
    int draw_len = len;
    nk_bool rtl = nk_false;

    NK_ASSERT(o);
    NK_ASSERT(t);
    if (!o || !t) return;

    b.h = NK_MAX(b.h, 2 * t->padding.y);

#ifdef NK_INCLUDE_BIDI
    /* reorder mixed RTL/LTR paragraphs into visual order; falls back to
     * the identity transform when bidi is disabled or unsupported */
    if (nk_bidi_is_enabled()) {
        draw_string = nk_bidi_visual(string, len, f, &draw_len, &rtl);
    }
#endif
    text_width = f->width(f->userdata, f->height,
        (const char*)draw_string, draw_len);
    text_width += (2.0f * t->padding.x);

    /* use top-left alignment by default */
    if (!(a & (NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_CENTERED | NK_TEXT_ALIGN_RIGHT)))
        a |= NK_TEXT_ALIGN_LEFT;
    if (!(a & (NK_TEXT_ALIGN_TOP | NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_BOTTOM)))
        a |= NK_TEXT_ALIGN_TOP;

    /* align in x-axis. For RTL paragraphs logical LEFT/RIGHT alignment is
     * mirrored so the text visually starts at the right edge, while the
     * unaligned default keeps its original position. */
    if (rtl && (a & NK_TEXT_ALIGN_LEFT))
        a = (nk_flags)((a & ~NK_TEXT_ALIGN_LEFT) | NK_TEXT_ALIGN_RIGHT);
    else if (rtl && (a & NK_TEXT_ALIGN_RIGHT))
        a = (nk_flags)((a & ~NK_TEXT_ALIGN_RIGHT) | NK_TEXT_ALIGN_LEFT);
    if (a & NK_TEXT_ALIGN_LEFT) {
        label.x = b.x + t->padding.x;
        label.w = NK_MAX(0, b.w - 2 * t->padding.x);
    } else if (a & NK_TEXT_ALIGN_CENTERED) {
        label.w = NK_MAX(1, 2 * t->padding.x + (float)text_width);
        label.x = (b.x + t->padding.x + ((b.w - 2 * t->padding.x) - label.w) / 2);
        label.x = NK_MAX(b.x + t->padding.x, label.x);
        label.w = NK_MIN(b.x + b.w, label.x + label.w);
        if (label.w >= label.x) label.w -= label.x;
    } else if (a & NK_TEXT_ALIGN_RIGHT) {
        label.x = NK_MAX(b.x + t->padding.x, (b.x + b.w) - (2 * t->padding.x + (float)text_width));
        label.w = (float)text_width + 2 * t->padding.x;
    }

    /* align in y-axis */
    if (a & NK_TEXT_ALIGN_TOP) {
        label.y = b.y + t->padding.y;
        label.h = NK_MIN(f->height, b.h - 2 * t->padding.y);
    } else if (a & NK_TEXT_ALIGN_MIDDLE) {
        label.y = b.y + b.h/2.0f - (float)f->height/2.0f;
        label.h = NK_MAX(b.h/2.0f, b.h - (b.h/2.0f + f->height/2.0f));
    } else if (a & NK_TEXT_ALIGN_BOTTOM) {
        label.y = b.y + b.h - f->height;
        label.h = f->height;
    }

    nk_draw_text(o, label, (const char*)draw_string, draw_len, f,
        t->background, t->text);
}
NK_LIB void
nk_widget_text_wrap(struct nk_command_buffer *o, struct nk_rect b,
    const char *string, int len, const struct nk_text *t,
    const struct nk_user_font *f)
{
    float width;
    int glyphs = 0;
    int fitting = 0;
    int done = 0;
    struct nk_rect line;
    struct nk_text text;
    NK_INTERN nk_rune seperator[] = {' '};

    NK_ASSERT(o);
    NK_ASSERT(t);
    if (!o || !t) return;

    text.padding = nk_vec2(0,0);
    text.background = t->background;
    text.text = t->text;

    b.w = NK_MAX(b.w, 2 * t->padding.x);
    b.h = NK_MAX(b.h, 2 * t->padding.y);
    b.h = b.h - 2 * t->padding.y;

    line.x = b.x + t->padding.x;
    line.y = b.y + t->padding.y;
    line.w = b.w - 2 * t->padding.x;
    line.h = 2 * t->padding.y + f->height;

#ifdef NK_INCLUDE_BIDI
    if (nk_bidi_is_enabled()) {
        /* bidi-aware word wrap: break into byte ranges first, then
         * reorder each line independently into visual order. The text is
         * streamed in chunks so arbitrarily long content never depends on
         * a fixed stack buffer; clipping by height matches the LTR path. */
        struct nk_bidi_wrap_line wraps[32];
        int consumed = 0;
        for (;;) {
            int nlines;
            int li;
            if (consumed >= len || line.y + line.h >= (b.y + b.h))
                break;
            nlines = nk_bidi_wrap_lines(string + consumed, len - consumed,
                line.w, f, wraps, NK_LEN(wraps));
            if (nlines <= 0) break;
            for (li = 0; li < nlines; ++li) {
                const char *lp;
                int llen;
                nk_bool rtl;
                if (line.y + line.h >= (b.y + b.h)) break;
                lp = string + consumed + wraps[li].byte_offset;
                llen = wraps[li].byte_length;
                /* strip the line break itself from drawing */
                if (llen > 0 && lp[llen - 1] == '\n') llen--;
                if (llen > 0)
                    lp = nk_bidi_visual(lp, llen, f, &llen, &rtl);
                if (llen > 0)
                    nk_widget_text(o, line, lp, llen, &text,
                        NK_TEXT_LEFT, f);
                line.y += f->height + 2 * t->padding.y;
            }
            consumed += wraps[nlines - 1].byte_offset +
                        wraps[nlines - 1].byte_length;
        }
        return;
    }
#endif

    fitting = nk_text_clamp(f, string, len, line.w, &glyphs, &width, seperator,NK_LEN(seperator));
    while (done < len) {
        if (!fitting || line.y + line.h >= (b.y + b.h)) break;
        nk_widget_text(o, line, &string[done], fitting, &text, NK_TEXT_LEFT, f);
        done += fitting;
        line.y += f->height + 2 * t->padding.y;
        fitting = nk_text_clamp(f, &string[done], len - done, line.w, &glyphs, &width, seperator,NK_LEN(seperator));
    }
}
NK_API void
nk_text_colored(struct nk_context *ctx, const char *str, int len,
    nk_flags alignment, struct nk_color color)
{
    struct nk_window *win;
    const struct nk_style *style;

    struct nk_vec2 item_padding;
    struct nk_rect bounds;
    struct nk_text text;

    NK_ASSERT(ctx);
    NK_ASSERT(ctx->current);
    NK_ASSERT(ctx->current->layout);
    if (!ctx || !ctx->current || !ctx->current->layout) return;

    win = ctx->current;
    style = &ctx->style;
    nk_panel_alloc_space(&bounds, ctx);
    item_padding = style->text.padding;

    text.padding.x = item_padding.x;
    text.padding.y = item_padding.y;
    text.background = style->window.background;
    text.text = nk_rgb_factor(color, style->text.color_factor);
    nk_widget_text(&win->buffer, bounds, str, len, &text, alignment, style->font);
}
NK_API void
nk_text_wrap_colored(struct nk_context *ctx, const char *str,
    int len, struct nk_color color)
{
    struct nk_window *win;
    const struct nk_style *style;

    struct nk_vec2 item_padding;
    struct nk_rect bounds;
    struct nk_text text;

    NK_ASSERT(ctx);
    NK_ASSERT(ctx->current);
    NK_ASSERT(ctx->current->layout);
    if (!ctx || !ctx->current || !ctx->current->layout) return;

    win = ctx->current;
    style = &ctx->style;
    nk_panel_alloc_space(&bounds, ctx);
    item_padding = style->text.padding;

    text.padding.x = item_padding.x;
    text.padding.y = item_padding.y;
    text.background = style->window.background;
    text.text = nk_rgb_factor(color, style->text.color_factor);
    nk_widget_text_wrap(&win->buffer, bounds, str, len, &text, style->font);
}
#ifdef NK_INCLUDE_STANDARD_VARARGS
NK_API void
nk_labelf_colored(struct nk_context *ctx, nk_flags flags,
    struct nk_color color, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    nk_labelfv_colored(ctx, flags, color, fmt, args);
    va_end(args);
}
NK_API void
nk_labelf_colored_wrap(struct nk_context *ctx, struct nk_color color,
    const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    nk_labelfv_colored_wrap(ctx, color, fmt, args);
    va_end(args);
}
NK_API void
nk_labelf(struct nk_context *ctx, nk_flags flags, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    nk_labelfv(ctx, flags, fmt, args);
    va_end(args);
}
NK_API void
nk_labelf_wrap(struct nk_context *ctx, const char *fmt,...)
{
    va_list args;
    va_start(args, fmt);
    nk_labelfv_wrap(ctx, fmt, args);
    va_end(args);
}
NK_API void
nk_labelfv_colored(struct nk_context *ctx, nk_flags flags,
    struct nk_color color, const char *fmt, va_list args)
{
    char buf[256];
    nk_strfmt(buf, NK_LEN(buf), fmt, args);
    nk_label_colored(ctx, buf, flags, color);
}

NK_API void
nk_labelfv_colored_wrap(struct nk_context *ctx, struct nk_color color,
    const char *fmt, va_list args)
{
    char buf[256];
    nk_strfmt(buf, NK_LEN(buf), fmt, args);
    nk_label_colored_wrap(ctx, buf, color);
}

NK_API void
nk_labelfv(struct nk_context *ctx, nk_flags flags, const char *fmt, va_list args)
{
    char buf[256];
    nk_strfmt(buf, NK_LEN(buf), fmt, args);
    nk_label(ctx, buf, flags);
}

NK_API void
nk_labelfv_wrap(struct nk_context *ctx, const char *fmt, va_list args)
{
    char buf[256];
    nk_strfmt(buf, NK_LEN(buf), fmt, args);
    nk_label_wrap(ctx, buf);
}

NK_API void
nk_value_bool(struct nk_context *ctx, const char *prefix, int value)
{
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: %s", prefix, ((value) ? "true": "false"));
}
NK_API void
nk_value_int(struct nk_context *ctx, const char *prefix, int value)
{
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: %d", prefix, value);
}
NK_API void
nk_value_uint(struct nk_context *ctx, const char *prefix, unsigned int value)
{
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: %u", prefix, value);
}
NK_API void
nk_value_float(struct nk_context *ctx, const char *prefix, float value)
{
    double double_value = (double)value;
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: %.3f", prefix, double_value);
}
NK_API void
nk_value_color_byte(struct nk_context *ctx, const char *p, struct nk_color c)
{
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: (%d, %d, %d, %d)", p, c.r, c.g, c.b, c.a);
}
NK_API void
nk_value_color_float(struct nk_context *ctx, const char *p, struct nk_color color)
{
    double c[4]; nk_color_dv(c, color);
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: (%.2f, %.2f, %.2f, %.2f)",
        p, c[0], c[1], c[2], c[3]);
}
NK_API void
nk_value_color_hex(struct nk_context *ctx, const char *prefix, struct nk_color color)
{
    char hex[16];
    nk_color_hex_rgba(hex, color);
    nk_labelf(ctx, NK_TEXT_LEFT, "%s: %s", prefix, hex);
}
#endif
NK_API void
nk_text(struct nk_context *ctx, const char *str, int len, nk_flags alignment)
{
    NK_ASSERT(ctx);
    if (!ctx) return;
    nk_text_colored(ctx, str, len, alignment, ctx->style.text.color);
}
NK_API void
nk_text_wrap(struct nk_context *ctx, const char *str, int len)
{
    NK_ASSERT(ctx);
    if (!ctx) return;
    nk_text_wrap_colored(ctx, str, len, ctx->style.text.color);
}
NK_API void
nk_label(struct nk_context *ctx, const char *str, nk_flags alignment)
{
    nk_text(ctx, str, nk_strlen(str), alignment);
}
NK_API void
nk_label_colored(struct nk_context *ctx, const char *str, nk_flags align,
    struct nk_color color)
{
    nk_text_colored(ctx, str, nk_strlen(str), align, color);
}
NK_API void
nk_label_wrap(struct nk_context *ctx, const char *str)
{
    nk_text_wrap(ctx, str, nk_strlen(str));
}
NK_API void
nk_label_colored_wrap(struct nk_context *ctx, const char *str, struct nk_color color)
{
    nk_text_wrap_colored(ctx, str, nk_strlen(str), color);
}

