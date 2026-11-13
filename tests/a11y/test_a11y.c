/*
 * Nuklear accessibility layer test suite.
 *
 * Drives the immediate mode API headlessly (no rendering backend) and
 * verifies the semantic tree: roles/names/values/states, visibility
 * filtering, focus order and modal confinement, dynamic add/remove,
 * per frame allocation behaviour and JSON escaping.
 *
 * Build: make -C tests/a11y
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_IMPLEMENTATION
#include "nuklear.h"

/* ---------------------------------------------------------------- */
/* test scaffolding                                                 */
/* ---------------------------------------------------------------- */
static int g_failures;
static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } } while (0)

/* allocator that counts live blocks and per-phase allocations */
struct counting_alloc {
    long live;
    long total_allocs;
    long phase_allocs;
    int counting;
};

static void*
ca_alloc(nk_handle h, void *old, nk_size size)
{
    struct counting_alloc *ca = (struct counting_alloc*)h.ptr;
    void *p = realloc(old, size);
    if (!old && p) {
        ca->live++;
        ca->total_allocs++;
        if (ca->counting) ca->phase_allocs++;
    }
    if (old && !p && size) ca->live--;
    return p;
}

static void
ca_free(nk_handle h, void *ptr)
{
    struct counting_alloc *ca = (struct counting_alloc*)h.ptr;
    if (ptr) ca->live--;
    free(ptr);
}

/* minimal font: width is estimated from byte length, no glyph baking */
static float
dummy_width(nk_handle h, float hgt, const char *txt, int len)
{
    (void)h; (void)hgt; (void)txt;
    return (float)len * 8.0f;
}

static void
make_font(struct nk_user_font *f)
{
    memset(f, 0, sizeof(*f));
    f->height = 16.0f;
    f->width = dummy_width;
}

static struct nk_context*
make_context(struct nk_user_font *font, struct nk_allocator *a)
{
    static struct nk_context ctx;
    if (!nk_init(&ctx, a, font)) return 0;
    return &ctx;
}

/* one headless frame: nuklear backends call nk_clear every frame */
static void
frame_begin(struct nk_context *ctx)
{
    nk_clear(ctx);
    nk_input_begin(ctx);
}

static void
frame_end(struct nk_context *ctx)
{
    nk_input_end(ctx);
}

static const struct nk_a11y_node*
find_named(const struct nk_context *ctx, const char *name)
{
    struct nk_a11y_snapshot s = nk_a11y_snapshot_get(ctx);
    int i;
    for (i = 1; i < s.count; ++i) {
        if (s.nodes[i] && strcmp(s.nodes[i]->name, name) == 0)
            return s.nodes[i];
    }
    return 0;
}

static int
count_role(const struct nk_context *ctx, enum nk_a11y_role role)
{
    struct nk_a11y_snapshot s = nk_a11y_snapshot_get(ctx);
    int i, n = 0;
    for (i = 1; i < s.count; ++i)
        if (s.nodes[i] && s.nodes[i]->role == role) n++;
    return n;
}

/* A representative application screen rebuilt every frame. */
struct app_state {
    nk_bool show_second;
    int list_rows;
    nk_bool check;
    int opt;
    char edit_buf[64];
    int edit_len;
    float slider;
    nk_size progress;
    int dynamic_buttons;
    nk_bool disable_block;
};

static void
build_ui(struct nk_context *ctx, struct app_state *st)
{
    enum nk_widget_layout_states ws;
    if (nk_begin(ctx, "Main", nk_rect(10, 10, 420, 760),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE))
    {
        nk_layout_row_dynamic(ctx, 28, 2);
        if (nk_button_label(ctx, "OK")) (void)0;
        if (nk_button_label(ctx, "Cancel")) (void)0;

        nk_checkbox_label(ctx, "Agree", &st->check);
        nk_option_label(ctx, "A", st->opt == 0);
        nk_option_label(ctx, "B", st->opt == 1);

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
            st->edit_buf, (int)sizeof(st->edit_buf), nk_filter_default);

        nk_slider_float(ctx, 0.0f, &st->slider, 100.0f, 1.0f);
        nk_progress(ctx, &st->progress, 100, nk_false);

        if (st->disable_block)
            nk_widget_disable_begin(ctx);
        nk_layout_row_dynamic(ctx, 26, 1);
        if (nk_button_label(ctx, "Disabled Button")) (void)0;
        if (st->disable_block)
            nk_widget_disable_end(ctx);

        /* list inside a group with a fixed row height so it always
         * has visible space for at least a few rows */
        nk_layout_row_dynamic(ctx, 150, 1);
        if (nk_group_begin(ctx, "Items", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            int i;
            nk_layout_row_dynamic(ctx, 22, 1);
            for (i = 0; i < st->list_rows; ++i) {
                char label[32];
                nk_bool sel = nk_false;
                snprintf(label, sizeof(label), "Row %d", i);
                nk_selectable_label(ctx, label, NK_TEXT_LEFT, &sel);
            }
            nk_group_end(ctx);
        }

        /* dynamically added/removed widgets with a repeated label */
        {
            int i;
            nk_layout_row_dynamic(ctx, 24, 1);
            for (i = 0; i < st->dynamic_buttons; ++i) {
                if (nk_button_label(ctx, "Same")) (void)0;
            }
        }
        (void)ws;
        nk_end(ctx);
    }

    /* second top-level window (Nuklear does not allow nesting nk_begin) */
    if (st->show_second) {
        if (nk_begin(ctx, "Second", nk_rect(460, 10, 220, 200),
            NK_WINDOW_BORDER | NK_WINDOW_TITLE))
        {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_button_label(ctx, "Nested OK")) (void)0;
            nk_end(ctx);
        }
    }
}

/* ---------------------------------------------------------------- */
/* tests                                                            */
/* ---------------------------------------------------------------- */
static void
test_roles_and_names(struct nk_context *ctx, struct app_state *st)
{
    struct nk_a11y_snapshot snap;
    const struct nk_a11y_node *n;

    printf("[1] roles/names/values/states\n");
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    snap = nk_a11y_snapshot_get(ctx);
    CHECK(snap.count > 5);
    CHECK(snap.nodes[0]->role == NK_A11Y_ROLE_PANE);
    CHECK(find_named(ctx, "Main") != 0);
    CHECK(find_named(ctx, "Main")->role == NK_A11Y_ROLE_WINDOW);

    n = find_named(ctx, "OK");
    CHECK(n && n->role == NK_A11Y_ROLE_BUTTON);
    CHECK(n && (n->state & NK_A11Y_STATE_FOCUSABLE));
    CHECK(n && n->focus_order >= 0);
    CHECK(n && n->parent >= 0);

    n = find_named(ctx, "Agree");
    CHECK(n && n->role == NK_A11Y_ROLE_TOGGLE_BUTTON);
    CHECK(n && !(n->state & NK_A11Y_STATE_CHECKED));
    st->check = nk_true;

    n = find_named(ctx, "A");
    CHECK(n && n->role == NK_A11Y_ROLE_RADIO_BUTTON);
    CHECK(n && (n->state & NK_A11Y_STATE_CHECKED));

    n = find_named(ctx, "Items");
    CHECK(n && n->role == NK_A11Y_ROLE_GROUP);

    n = find_named(ctx, "Row 0");
    CHECK(n && n->role == NK_A11Y_ROLE_LIST_ITEM);

    n = find_named(ctx, "slider");
    CHECK(n && n->role == NK_A11Y_ROLE_SLIDER && n->value != 0);

    n = find_named(ctx, "progress");
    CHECK(n && n->role == NK_A11Y_ROLE_PROGRESS_BAR && n->value != 0);

    /* rebuild with checkbox on */
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    n = find_named(ctx, "Agree");
    CHECK(n && (n->state & NK_A11Y_STATE_CHECKED));
}

static void
test_input_value_and_escaping(struct nk_context *ctx, struct app_state *st)
{
    const struct nk_a11y_node *n;
    char json[8192];
    int len;

    printf("[2] text input value and JSON escaping\n");
    /* quotes, backslash, newline and a control character must escape */
    strcpy(st->edit_buf, "a\"b\\c\nd\x01");
    st->edit_len = (int)strlen(st->edit_buf);

    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    n = find_named(ctx, "edit");
    CHECK(n && n->role == NK_A11Y_ROLE_TEXT_INPUT);
    CHECK(n && strcmp(n->value, st->edit_buf) == 0);

    len = nk_a11y_snapshot_dump_json(ctx, json, (int)sizeof(json));
    CHECK(len > 0);
    CHECK(strstr(json, "a\\\"b\\\\c\\nd\\u0001") != 0);
    CHECK(json[len] == 0);

    /* undersized buffer must report the needed size without writing */
    {
        char tiny[8];
        int need = nk_a11y_snapshot_dump_json(ctx, tiny, (int)sizeof(tiny));
        CHECK(need > (int)sizeof(tiny));
    }
}

static void
test_disabled_hidden_clipped(struct nk_context *ctx, struct app_state *st)
{
    const struct nk_a11y_node *n;

    printf("[3] disabled / hidden / clipping\n");
    st->disable_block = nk_true;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    n = find_named(ctx, "Disabled Button");
    CHECK(n && (n->state & NK_A11Y_STATE_DISABLED));
    CHECK(n && !(n->state & NK_A11Y_STATE_FOCUSABLE));
    CHECK(n && n->focus_order == -1);
    st->disable_block = nk_false;

    /* hidden window: its subtree must not appear at all. nk_begin
     * returns false but still sets ctx->current; nk_end detects the
     * missing layout and resets it, so the pair stays balanced. */
    frame_begin(ctx);
    {
        nk_bool open = nk_begin(ctx, "HiddenWin", nk_rect(0, 0, 100, 100),
            NK_WINDOW_HIDDEN);
        if (open) {
            nk_button_label(ctx, "Ghost");
        }
        nk_end(ctx);
    }
    frame_end(ctx);
    CHECK(find_named(ctx, "Ghost") == 0);
    CHECK(find_named(ctx, "HiddenWin") == 0);

    /* fully clipped widget: place it outside the panel clip region */
    frame_begin(ctx);
    if (nk_begin(ctx, "ClipHost", nk_rect(10, 10, 200, 120),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 26, 1);
        if (nk_button_label(ctx, "Visible")) (void)0;
        /* many rows push the last ones outside the visible clip */
        {
            int i;
            for (i = 0; i < 12; ++i)
                if (nk_button_label(ctx, "ClippedAway")) (void)0;
        }
        nk_end(ctx);
    }
    frame_end(ctx);
    {
        struct nk_a11y_snapshot s = nk_a11y_snapshot_get(ctx);
        int clipped_present = 0, i;
        for (i = 1; i < s.count; ++i)
            if (s.nodes[i] && strcmp(s.nodes[i]->name, "ClippedAway") == 0)
                clipped_present++;
        /* at least one occurrence exists, but never all 12: the rows
         * scrolled past the clip are excluded from the semantic tree */
        CHECK(clipped_present > 0 && clipped_present < 12);
        CHECK(find_named(ctx, "Visible") != 0);
    }
}

static void
test_nested_window(struct nk_context *ctx, struct app_state *st)
{
    const struct nk_a11y_node *main_n, *second_n, *btn;
    struct nk_a11y_snapshot snap;
    int i, found_nested_under_second = 0;

    printf("[4] nested window hierarchy\n");
    st->show_second = nk_true;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    main_n = find_named(ctx, "Main");
    second_n = find_named(ctx, "Second");
    btn = find_named(ctx, "Nested OK");
    CHECK(main_n && second_n && btn);
    CHECK(second_n && second_n->parent >= 0);
    CHECK(btn && btn->role == NK_A11Y_ROLE_BUTTON);

    /* every non-root node must reach the virtual root via parents */
    snap = nk_a11y_snapshot_get(ctx);
    for (i = 1; i < snap.count; ++i) {
        const struct nk_a11y_node *cur = snap.nodes[i];
        int guard = 0;
        while (cur && cur->parent >= 0 && guard++ < snap.count + 1)
            cur = snap.nodes[cur->parent];
        CHECK(cur == snap.nodes[0]);
    }

    {
        const struct nk_a11y_node *cur = btn;
        while (cur && cur->id != second_n->id) cur = snap.nodes[cur->parent];
        if (cur && cur->id == second_n->id) found_nested_under_second = 1;
    }
    CHECK(found_nested_under_second);
    st->show_second = nk_false;
}

static void
test_dynamic_add_remove(struct nk_context *ctx, struct app_state *st)
{
    nk_uint id_first = 0, id_again = 0;
    const struct nk_a11y_node *n0, *n1;

    printf("[5] dynamic add/remove and stable ids\n");
    st->dynamic_buttons = 2;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    CHECK(count_role(ctx, NK_A11Y_ROLE_BUTTON) >= 4);
    n0 = find_named(ctx, "Same");
    CHECK(n0);
    id_first = n0->id;

    /* grow: first occurrence keeps its id */
    st->dynamic_buttons = 5;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    n0 = find_named(ctx, "Same");
    CHECK(n0 && n0->id == id_first);

    /* shrink back: the surviving first node is stable again */
    st->dynamic_buttons = 1;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    n0 = find_named(ctx, "Same");
    CHECK(n0 && n0->id == id_first);
    id_again = n0->id;
    (void)n1;
    CHECK(id_again == id_first);

    st->dynamic_buttons = 0;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
}

static void
test_focus_order(struct nk_context *ctx, struct app_state *st)
{
    struct nk_a11y_snapshot snap;
    int i, last = -1, ordered = 1;
    nk_uint first_id;
    const struct nk_a11y_node *first_n;

    printf("[6] focus order and navigation\n");
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    /* focus_order values are dense, increasing in emission order, and
     * skip disabled nodes */
    snap = nk_a11y_snapshot_get(ctx);
    for (i = 1; i < snap.count; ++i) {
        const struct nk_a11y_node *n = snap.nodes[i];
        if (n->focus_order < 0) continue;
        CHECK(!(n->state & NK_A11Y_STATE_DISABLED));
        if (n->focus_order <= last) ordered = 0;
        last = n->focus_order;
    }
    CHECK(ordered);

    first_n = snap.nodes[1];
    /* find first focusable node */
    for (i = 1; i < snap.count; ++i)
        if (snap.nodes[i]->focus_order == 0) { first_n = snap.nodes[i]; break; }
    first_id = first_n->id;

    CHECK(nk_a11y_focus_first(ctx));
    CHECK(nk_a11y_focused_id(ctx) == first_id);
    {
        const struct nk_a11y_node *f = find_named(ctx, first_n->name);
        CHECK(f && (f->state & NK_A11Y_STATE_FOCUSED));
    }
    CHECK(nk_a11y_focus_next(ctx));
    CHECK(nk_a11y_focused_id(ctx) != first_id);
    CHECK(nk_a11y_focus_prev(ctx));
    CHECK(nk_a11y_focused_id(ctx) == first_id);

    /* focus never lands on a disabled node */
    st->disable_block = nk_true;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    {
        const struct nk_a11y_node *d = find_named(ctx, "Disabled Button");
        CHECK(d);
        CHECK(!nk_a11y_set_focus(ctx, d->id));
        CHECK(nk_a11y_focused_id(ctx) != d->id);
    }
    st->disable_block = nk_false;
}

static void
test_modal_popup(struct nk_context *ctx)
{
    const struct nk_a11y_node *outside, *modal_menu, *item;
    nk_uint outside_id;

    printf("[7] modal popup confines focus and activation\n");
    frame_begin(ctx);
    if (nk_begin(ctx, "ModalHost", nk_rect(20, 20, 300, 300),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Outside")) (void)0;
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "OpenPopup")) (void)0;
        nk_end(ctx);
    }
    frame_end(ctx);

    outside = find_named(ctx, "Outside");
    CHECK(outside);
    outside_id = outside->id;

    /* open a blocking (modal) popup unconditionally for the frame */
    frame_begin(ctx);
    if (nk_begin(ctx, "ModalHost", nk_rect(20, 20, 300, 300),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Outside")) (void)0;
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_popup_begin(ctx, NK_POPUP_STATIC, "Choice", 0,
            nk_rect(40, 40, 160, 120)))
        {
            nk_layout_row_dynamic(ctx, 28, 1);
            if (nk_button_label(ctx, "Choice A")) (void)0;
            if (nk_button_label(ctx, "Choice B")) (void)0;
            nk_popup_end(ctx);
        }
        nk_end(ctx);
    }
    frame_end(ctx);

    modal_menu = find_named(ctx, "Choice");
    CHECK(modal_menu && (modal_menu->state & NK_A11Y_STATE_MODAL));
    item = find_named(ctx, "Choice A");
    CHECK(item);

    /* focus and activation must be refused outside the modal subtree */
    CHECK(!nk_a11y_set_focus(ctx, outside_id));
    CHECK(nk_a11y_set_focus(ctx, item->id));
    CHECK(!nk_a11y_activate(ctx, outside_id));
    CHECK(nk_a11y_activate(ctx, item->id));
}

static void
test_combo_expansion(struct nk_context *ctx)
{
    static const char *items[] = { "Red", "Green", "Blue" };
    const struct nk_a11y_node *combo;

    printf("[8] combo collapsed/expanded state\n");

    /* drain any activation queued by an earlier test so this one starts
     * from an idle input state (one press frame + one release frame) */
    frame_begin(ctx); frame_end(ctx);
    frame_begin(ctx); frame_end(ctx);

    /* build the collapsed combo */
    frame_begin(ctx);
    if (nk_begin(ctx, "ComboHost", nk_rect(10, 10, 300, 200),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_combo(ctx, items, 3, 0, 24, nk_vec2(180, 120));
        nk_end(ctx);
    }
    frame_end(ctx);
    combo = find_named(ctx, "Red");
    CHECK(combo && combo->role == NK_A11Y_ROLE_COMBO_BOX);
    CHECK(combo && (combo->state & NK_A11Y_STATE_COLLAPSED));

    /* open the combo through the accessibility activation action: the
     * synthetic press and release are delivered on the next two frames
     * by the layer itself, so two build passes are required. */
    CHECK(nk_a11y_activate(ctx, combo->id));
    frame_begin(ctx);
    if (nk_begin(ctx, "ComboHost", nk_rect(10, 10, 300, 200),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_combo(ctx, items, 3, 0, 24, nk_vec2(180, 120));
        nk_end(ctx);
    }
    frame_end(ctx);
    /* the combo opens on the press frame */
    combo = find_named(ctx, "Red");
    CHECK(combo && (combo->state & NK_A11Y_STATE_EXPANDED));
    CHECK(find_named(ctx, "Green") != 0);

    /* release frame: the popup stays open and keeps its items */
    frame_begin(ctx);
    if (nk_begin(ctx, "ComboHost", nk_rect(10, 10, 300, 200),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_combo(ctx, items, 3, 0, 24, nk_vec2(180, 120));
        nk_end(ctx);
    }
    frame_end(ctx);
    combo = find_named(ctx, "Red");
    CHECK(combo && (combo->state & NK_A11Y_STATE_EXPANDED));
    CHECK(find_named(ctx, "Green") != 0);
}

static void
test_allocation_reuse(struct nk_context *ctx, struct app_state *st,
    struct counting_alloc *ca)
{
    int frame;
    long peak = 0;

    printf("[9] steady-state frames allocate no heap memory\n");
    /* warm up so every arena reaches its working-set size */
    for (frame = 0; frame < 8; ++frame) {
        st->dynamic_buttons = (frame % 3) + 1;
        st->list_rows = 6 + (frame % 4);
        frame_begin(ctx);
        build_ui(ctx, st);
        frame_end(ctx);
    }
    st->dynamic_buttons = 2;
    st->list_rows = 8;

    /* identical frames must now perform zero allocations */
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);

    ca->counting = 1;
    ca->phase_allocs = 0;
    for (frame = 0; frame < 16; ++frame) {
        frame_begin(ctx);
        build_ui(ctx, st);
        frame_end(ctx);
        if (ca->phase_allocs > peak) peak = ca->phase_allocs;
        CHECK(ca->phase_allocs == 0);
    }
    ca->counting = 0;
    CHECK(peak == 0);

    /* growing the workload is allowed to allocate to grow the arenas and
     * must then return to zero-allocation steady state */
    st->list_rows = 64;
    frame_begin(ctx);
    build_ui(ctx, st);
    frame_end(ctx);
    ca->counting = 1;
    ca->phase_allocs = 0;
    for (frame = 0; frame < 4; ++frame) {
        frame_begin(ctx);
        build_ui(ctx, st);
        frame_end(ctx);
    }
    CHECK(ca->phase_allocs == 0);
    ca->counting = 0;
    st->list_rows = 8;
}

static void
test_activation_input(struct nk_context *ctx)
{
    const struct nk_a11y_node *btn;
    nk_bool pressed = nk_false;

    printf("[10] activation replays through normal input\n");
    frame_begin(ctx);
    if (nk_begin(ctx, "ActHost", nk_rect(10, 10, 200, 120),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "PressMe")) pressed = nk_true;
        nk_end(ctx);
    }
    frame_end(ctx);
    btn = find_named(ctx, "PressMe");
    CHECK(btn);
    CHECK(nk_a11y_activate(ctx, btn->id));

    /* the layer injects the press on the next frame's nk_input_begin
     * and the release on the frame after; the widget reports a click
     * on the release frame */
    frame_begin(ctx);
    if (nk_begin(ctx, "ActHost", nk_rect(10, 10, 200, 120),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "PressMe")) pressed = nk_true;
        nk_end(ctx);
    }
    frame_end(ctx);

    frame_begin(ctx);
    if (nk_begin(ctx, "ActHost", nk_rect(10, 10, 200, 120),
        NK_WINDOW_BORDER | NK_WINDOW_TITLE))
    {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "PressMe")) pressed = nk_true;
        nk_end(ctx);
    }
    frame_end(ctx);
    CHECK(pressed);
}

int
main(void)
{
    static struct nk_user_font font;
    struct counting_alloc ca;
    struct nk_allocator alloc;
    struct app_state st;
    struct nk_context *ctx;
    char json[16384];
    int len;

    memset(&st, 0, sizeof(st));
    strcpy(st.edit_buf, "hello");
    st.list_rows = 5;
    st.slider = 30.0f;
    st.progress = 40;

    memset(&ca, 0, sizeof(ca));
    alloc.userdata.ptr = &ca;
    alloc.alloc = ca_alloc;
    alloc.free = ca_free;

    make_font(&font);
    ctx = make_context(&font, &alloc);
    if (!ctx) { printf("context init failed\n"); return 2; }

    CHECK(nk_a11y_enable(ctx, &alloc));
    CHECK(nk_a11y_is_enabled(ctx));

    test_roles_and_names(ctx, &st);
    test_input_value_and_escaping(ctx, &st);
    test_disabled_hidden_clipped(ctx, &st);
    test_nested_window(ctx, &st);
    test_dynamic_add_remove(ctx, &st);
    test_focus_order(ctx, &st);
    test_modal_popup(ctx);
    test_combo_expansion(ctx);
    test_allocation_reuse(ctx, &st, &ca);
    test_activation_input(ctx);

    /* final frame: exercise the full JSON serializer end to end. The
     * checked-in regression baseline is produced by baseline_gen.c. */
    frame_begin(ctx);
    build_ui(ctx, &st);
    frame_end(ctx);
    len = nk_a11y_snapshot_dump_json(ctx, json, (int)sizeof(json));
    CHECK(len > 0);
    CHECK((int)strlen(json) == len);
    CHECK(json[0] == '{');

    nk_a11y_disable(ctx);
    CHECK(!nk_a11y_is_enabled(ctx));
    nk_free(ctx);
    CHECK(ca.live == 0);

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
