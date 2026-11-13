#include "nuklear.h"
#include "nuklear_internal.h"

/* ===============================================================
 *
 *                          ACCESSIBILITY
 *
 * Optional semantic layer. Instrumented widgets call nk_a11y_emit()
 * while the immediate mode UI is built; the calls produce a tree of
 * nk_a11y_node describing the visible interface of the current frame.
 *
 * All storage is paged/growable and reused across frames:
 *
 *   - node blocks hold NK_A11Y_NODE_PAGE nodes each,
 *   - text blocks hold NK_A11Y_TEXT_PAGE label/value bytes each,
 *   - a flat index array gives O(1) index based access and only
 *     grows (doubling) when the working set grows,
 *
 * A frame reset only rewinds cursors and counts (O(1)); all blocks are
 * kept for the next frame so a steady workload performs zero heap
 * allocations per frame and never allocates per widget.
 *
 * ===============================================================*/
#ifdef NK_INCLUDE_ACCESSIBILITY

struct nk_a11y_node_block {
    struct nk_a11y_node_block *next;
    int count;
    struct nk_a11y_node node[NK_A11Y_NODE_PAGE];
};

struct nk_a11y_text_block {
    struct nk_a11y_text_block *next;
    int used;
    char data[NK_A11Y_TEXT_PAGE];
};

struct nk_a11y_dup_entry {
    nk_hash key;
    int occurrences;
};

struct nk_a11y_dup_block {
    struct nk_a11y_dup_block *next;
    int count;
    struct nk_a11y_dup_entry entry[NK_A11Y_NODE_PAGE];
};

struct nk_a11y_pending_click {
    int x, y;
    nk_bool valid;
};

struct nk_a11y_runtime {
    nk_bool enabled;
    struct nk_allocator alloc;

    /* node storage blocks + flat index (capacity retained across frames) */
    struct nk_a11y_node_block *nodes;
    struct nk_a11y_node **flat;
    int flat_count;
    int flat_cap;

    /* string arena */
    struct nk_a11y_text_block *texts;

    /* per-parent duplicate name bookkeeping (rewound each frame) */
    struct nk_a11y_dup_block *dups;

    /* scope stack: node indices of open windows/groups/popups */
    int scope[NK_A11Y_SCOPE_DEPTH];
    int scope_depth;

    /* modal restriction: scope depth that focus must stay inside */
    int modal_scope;
    int modal_node;

    nk_uint focused_id;      /* persistent across frames, 0 = none */
    int focused_index;       /* resolved during the current frame */
    int focus_counter;       /* tab order assigned this frame */
    unsigned int frame;

    /* synthetic input queued by nk_a11y_activate(); replay is split so
     * the press and release land in different frames */
    struct nk_a11y_pending_click pending;
    nk_bool release_queued;
};

static int a11y_strlen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

/* ----------------------------------------------------------------
 * Arena lifecycle
 * ---------------------------------------------------------------- */
NK_LIB void
nk_a11y_shutdown(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    if (!ctx || !ctx->a11y) return;
    a = ctx->a11y;
    while (a->nodes) {
        struct nk_a11y_node_block *next = a->nodes->next;
        a->alloc.free(a->alloc.userdata, a->nodes);
        a->nodes = next;
    }
    while (a->texts) {
        struct nk_a11y_text_block *next = a->texts->next;
        a->alloc.free(a->alloc.userdata, a->texts);
        a->texts = next;
    }
    while (a->dups) {
        struct nk_a11y_dup_block *next = a->dups->next;
        a->alloc.free(a->alloc.userdata, a->dups);
        a->dups = next;
    }
    if (a->flat) a->alloc.free(a->alloc.userdata, a->flat);
    a->alloc.free(a->alloc.userdata, a);
    ctx->a11y = 0;
}

NK_API nk_bool
nk_a11y_enable(struct nk_context *ctx, struct nk_allocator *alloc)
{
    NK_ASSERT(ctx);
    if (!ctx || !alloc) return nk_false;
    if (ctx->a11y) {
        ctx->a11y->enabled = nk_true;
        return nk_true;
    }
    ctx->a11y = (struct nk_a11y_runtime*)
        alloc->alloc(alloc->userdata, 0, sizeof(struct nk_a11y_runtime));
    if (!ctx->a11y) return nk_false;
    NK_MEMSET(ctx->a11y, 0, sizeof(*ctx->a11y));
    ctx->a11y->alloc = *alloc;
    ctx->a11y->enabled = nk_true;
    ctx->a11y->modal_scope = -1;
    ctx->a11y->modal_node = -1;
    nk_a11y_begin_frame(ctx);
    return nk_true;
}

NK_API void
nk_a11y_disable(struct nk_context *ctx)
{
    if (!ctx || !ctx->a11y) return;
    nk_a11y_shutdown(ctx);
}

NK_API nk_bool
nk_a11y_is_enabled(const struct nk_context *ctx)
{
    if (!ctx || !ctx->a11y) return nk_false;
    return ctx->a11y->enabled;
}

static struct nk_a11y_node_block*
a11y_new_node_block(struct nk_a11y_runtime *a)
{
    struct nk_a11y_node_block *b = (struct nk_a11y_node_block*)
        a->alloc.alloc(a->alloc.userdata, 0, sizeof(*b));
    if (!b) return 0;
    b->next = 0;
    b->count = 0;
    return b;
}

static struct nk_a11y_text_block*
a11y_new_text_block(struct nk_a11y_runtime *a)
{
    struct nk_a11y_text_block *b = (struct nk_a11y_text_block*)
        a->alloc.alloc(a->alloc.userdata, 0, sizeof(*b));
    if (!b) return 0;
    b->next = 0;
    b->used = 0;
    return b;
}

static struct nk_a11y_dup_block*
a11y_new_dup_block(struct nk_a11y_runtime *a)
{
    struct nk_a11y_dup_block *b = (struct nk_a11y_dup_block*)
        a->alloc.alloc(a->alloc.userdata, 0, sizeof(*b));
    if (!b) return 0;
    b->next = 0;
    b->count = 0;
    return b;
}

/* Grow the flat index by doubling; the allocation is retained for the
 * lifetime of the runtime so steady-state frames never allocate. */
static nk_bool
a11y_flat_reserve(struct nk_a11y_runtime *a, int need)
{
    int newcap;
    struct nk_a11y_node **grown;
    if (need <= a->flat_cap) return nk_true;
    newcap = a->flat_cap ? a->flat_cap : 64;
    while (newcap < need) newcap *= 2;
    grown = (struct nk_a11y_node**)a->alloc.alloc(a->alloc.userdata,
        a->flat, (nk_size)newcap * sizeof(struct nk_a11y_node*));
    if (!grown) return nk_false;
    a->flat = grown;
    a->flat_cap = newcap;
    return nk_true;
}

/* Copy `len` bytes into the string arena, always NUL terminated.
 * Truncates at NK_A11Y_NAME_MAX. Empty input returns NULL. */
static const char*
a11y_intern(struct nk_a11y_runtime *a, const char *text, int len)
{
    int copy;
    char *dst;
    struct nk_a11y_text_block *b;

    if (!text || len <= 0) return 0;
    if (len > NK_A11Y_NAME_MAX) len = NK_A11Y_NAME_MAX;
    copy = len;

    b = a->texts;
    if (!b || b->used + copy + 1 > NK_A11Y_TEXT_PAGE) {
        struct nk_a11y_text_block *nb = a11y_new_text_block(a);
        if (!nb) return 0;
        nb->next = a->texts;
        a->texts = nb;
        b = nb;
    }
    dst = b->data + b->used;
    NK_MEMCPY(dst, text, (nk_size)copy);
    dst[copy] = 0;
    b->used += copy + 1;
    return dst;
}

/* FNV-1a 32 bit, deterministic across platforms/compilers. */
static nk_hash
a11y_hash_bytes(const char *data, int len, nk_hash seed)
{
    nk_hash h = seed ? seed : 2166136261u;
    int i;
    for (i = 0; i < len; ++i) {
        h ^= (nk_hash)(unsigned char)data[i];
        h *= 16777619u;
    }
    return h;
}

/* Count same-named children already recorded for `key`, reserve next. */
static int
a11y_dup_reserve(struct nk_a11y_runtime *a, nk_hash key)
{
    struct nk_a11y_dup_block *b;
    struct nk_a11y_dup_block *tail = 0;
    int i;

    b = a->dups;
    while (b) {
        for (i = 0; i < b->count; ++i) {
            if (b->entry[i].key == key)
                return b->entry[i].occurrences++;
        }
        tail = b;
        b = b->next;
    }
    if (!a->dups || a->dups->count >= NK_A11Y_NODE_PAGE) {
        struct nk_a11y_dup_block *nb = a11y_new_dup_block(a);
        if (!nb) return 0;
        nb->next = a->dups;
        a->dups = nb;
        tail = nb;
    }
    if (tail) {
        i = tail->count++;
        tail->entry[i].key = key;
        tail->entry[i].occurrences = 1;
    }
    return 0;
}

/* Stable id from parent id + local name + same-name occurrence. */
static nk_uint
a11y_make_id(nk_uint parent_id, const char *name, int name_len, int occurrence)
{
    nk_hash h = a11y_hash_bytes(name, name_len, parent_id * 2654435761u + 1u);
    h = a11y_hash_bytes((const char*)&occurrence, (int)sizeof(occurrence), h);
    if (h == 0) h = 1;
    return (nk_uint)h;
}

/* ----------------------------------------------------------------
 * Frame lifecycle
 * ---------------------------------------------------------------- */
NK_LIB void
nk_a11y_begin_frame(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    struct nk_a11y_node_block *nb;
    struct nk_a11y_dup_block *db;
    struct nk_a11y_node *root;

    if (!ctx || !ctx->a11y) return;
    a = ctx->a11y;
    if (!a->enabled) return;

    /* rewind arenas in O(1); allocated blocks are kept for reuse */
    nb = a->nodes;
    while (nb) {
        nb->count = 0;
        nb = nb->next;
    }
    if (a->texts) a->texts->used = 0;
    {
        struct nk_a11y_text_block *tb = a->texts ? a->texts->next : 0;
        while (tb) {
            struct nk_a11y_text_block *next = tb->next;
            a->alloc.free(a->alloc.userdata, tb);
            tb = next;
        }
        if (a->texts) a->texts->next = 0;
    }
    db = a->dups;
    while (db) {
        db->count = 0;
        db = db->next;
    }

    a->flat_count = 0;
    a->scope_depth = 0;
    a->modal_scope = -1;
    a->modal_node = -1;
    a->focus_counter = 0;
    a->focused_index = -1;
    a->frame++;

    /* replay queued accessibility input after nk_input_begin cleared the
     * transient click counters. A queued press is delivered this frame
     * and its matching release deferred to the following frame so the
     * immediate mode widget logic observes a real press/release cycle. */
    if (a->release_queued) {
        nk_input_button(ctx, NK_BUTTON_LEFT,
            a->pending.x, a->pending.y, nk_false);
        a->release_queued = nk_false;
        a->pending.valid = nk_false;
    } else if (a->pending.valid) {
        nk_input_motion(ctx, a->pending.x, a->pending.y);
        nk_input_button(ctx, NK_BUTTON_LEFT,
            a->pending.x, a->pending.y, nk_true);
        a->release_queued = nk_true;
    }

    /* index 0 is the virtual document root */
    if (!a->nodes) {
        a->nodes = a11y_new_node_block(a);
        if (!a->nodes) return;
    }
    if (!a11y_flat_reserve(a, 1)) return;
    root = &a->nodes->node[0];
    NK_MEMSET(root, 0, sizeof(*root));
    root->id = 1;
    root->role = NK_A11Y_ROLE_PANE;
    root->parent = -1;
    root->first_child = root->last_child = -1;
    root->next_sibling = root->prev_sibling = -1;
    root->focus_order = -1;
    root->name = "";
    root->value = 0;
    a->nodes->count = 1;
    a->flat[0] = root;
    a->flat_count = 1;
}

NK_API void
nk_a11y_frame_reset(struct nk_context *ctx)
{
    nk_a11y_begin_frame(ctx);
}

static struct nk_a11y_node*
a11y_node_at(struct nk_a11y_runtime *a, int index)
{
    if (index < 0 || index >= a->flat_count) return 0;
    return a->flat[index];
}

/* ----------------------------------------------------------------
 * Visibility
 * ---------------------------------------------------------------- */
static nk_bool
a11y_rect_visible(struct nk_context *ctx, struct nk_rect r)
{
    struct nk_panel *panel;
    struct nk_rect clip;
    if (!ctx->current || !ctx->current->layout) return nk_false;
    panel = ctx->current->layout;
    clip = panel->clip;
    return NK_INTERSECT(clip.x, clip.y, clip.w, clip.h,
        r.x, r.y, r.w, r.h) ? nk_true : nk_false;
}

/* ----------------------------------------------------------------
 * Node emission
 * ---------------------------------------------------------------- */
NK_LIB nk_bool
nk_a11y_rect_is_visible(struct nk_context *ctx, struct nk_rect r)
{
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    return a11y_rect_visible(ctx, r);
}

NK_LIB int
nk_a11y_emit_in(struct nk_context *ctx, int parent, nk_bool visible,
    enum nk_a11y_role role, const char *name, int name_len,
    const char *value, int value_len, struct nk_rect bounds, nk_flags state)
{
    struct nk_a11y_runtime *a;
    struct nk_a11y_node_block *b;
    struct nk_a11y_node *node, *parent_node;
    int index, occurrence, nlen;
    nk_hash name_key, id;
    const char *name_copy, *value_copy;

    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return -1;
    a = ctx->a11y;

    /* Fully clipped / scrolled-away / hidden widgets are excluded:
     * focus can never land on an invisible control. */
    if (!visible) return -1;

    if (parent < 0)
        parent = a->scope_depth ? a->scope[a->scope_depth - 1] : 0;

    name_copy = a11y_intern(a, name, name_len);
    value_copy = a11y_intern(a, value, value_len);
    if (!name_copy) name_copy = "";
    nlen = a11y_strlen(name_copy);

    /* stable identity: parent id + name + per-parent occurrence so
     * repeated labels and dynamic add/remove stay addressable */
    parent_node = a11y_node_at(a, parent);
    name_key = a11y_hash_bytes(name_copy, nlen,
        (nk_hash)(parent + 1) * 2654435761u);
    occurrence = a11y_dup_reserve(a, name_key);
    id = a11y_make_id(parent_node ? parent_node->id : 1,
        name_copy, nlen, occurrence);

    /* allocate a slot, growing the node block chain when full */
    b = a->nodes;
    while (b->next) b = b->next;
    if (b->count >= NK_A11Y_NODE_PAGE) {
        struct nk_a11y_node_block *nb = a11y_new_node_block(a);
        if (!nb) return -1;
        b->next = nb;
        b = nb;
    }
    if (!a11y_flat_reserve(a, a->flat_count + 1)) return -1;
    index = a->flat_count++;
    node = &b->node[b->count++];
    NK_MEMSET(node, 0, sizeof(*node));
    node->id = id;
    node->role = role;
    node->bounds = bounds;
    node->parent = parent;
    node->first_child = node->last_child = -1;
    node->next_sibling = node->prev_sibling = -1;
    node->focus_order = -1;
    node->name = name_copy;
    node->value = value_copy;
    node->state = state;
    a->flat[index] = node;

    /* link into parent's child list */
    NK_ASSERT(parent_node);
    if (parent_node) {
        if (parent_node->first_child < 0) {
            parent_node->first_child = index;
            parent_node->last_child = index;
        } else {
            struct nk_a11y_node *last =
                a11y_node_at(a, parent_node->last_child);
            if (last) {
                last->next_sibling = index;
                node->prev_sibling = parent_node->last_child;
            }
            parent_node->last_child = index;
        }
    }

    /* tab order; disabled controls are never focusable */
    if (!(state & NK_A11Y_STATE_DISABLED)) {
        node->state |= NK_A11Y_STATE_FOCUSABLE;
        node->focus_order = a->focus_counter++;
    }
    if (a->focused_id && id == a->focused_id) {
        node->state |= NK_A11Y_STATE_FOCUSED;
        a->focused_index = index;
    }
    return index;
}

NK_LIB int
nk_a11y_emit(struct nk_context *ctx, enum nk_a11y_role role,
    const char *name, int name_len, const char *value, int value_len,
    struct nk_rect bounds, nk_flags state)
{
    return nk_a11y_emit_in(ctx, -1, a11y_rect_visible(ctx, bounds),
        role, name, name_len, value, value_len, bounds, state);
}

NK_LIB void
nk_a11y_scope_push(struct nk_context *ctx, enum nk_a11y_role role,
    const char *name, int name_len, struct nk_rect bounds, nk_flags state)
{
    struct nk_a11y_runtime *a;
    int idx;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return;
    a = ctx->a11y;
    idx = nk_a11y_emit(ctx, role, name, name_len, 0, 0, bounds, state);
    if (idx < 0) return;
    if (a->scope_depth >= NK_A11Y_SCOPE_DEPTH) {
        NK_ASSERT(!"NK_A11Y_SCOPE_DEPTH exceeded");
        return;
    }
    a->scope[a->scope_depth++] = idx;
}

NK_LIB void
nk_a11y_scope_pop(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return;
    a = ctx->a11y;
    if (a->scope_depth > 0)
        a->scope_depth--;
}

NK_LIB int
nk_a11y_current_scope_idx(const struct nk_context *ctx)
{
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled || !ctx->a11y->scope_depth)
        return -1;
    return ctx->a11y->scope[ctx->a11y->scope_depth - 1];
}

NK_LIB void
nk_a11y_note_popup(struct nk_context *ctx, nk_bool modal, nk_bool open)
{
    struct nk_a11y_runtime *a;
    int scope;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return;
    a = ctx->a11y;
    if (!open) {
        if (a->modal_scope >= 0 && a->scope_depth <= a->modal_scope) {
            a->modal_scope = -1;
            a->modal_node = -1;
        }
        return;
    }
    scope = a->scope_depth - 1;
    if (modal && scope > a->modal_scope) {
        a->modal_scope = scope;
        a->modal_node = scope >= 0 ? a->scope[scope] : -1;
        if (a->modal_node >= 0) {
            struct nk_a11y_node *n = a11y_node_at(a, a->modal_node);
            if (n) n->state |= NK_A11Y_STATE_MODAL;
        }
    }
}

NK_LIB void
nk_a11y_note_edit_focus(struct nk_context *ctx, int node_index)
{
    struct nk_a11y_runtime *a;
    struct nk_a11y_node *n;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return;
    a = ctx->a11y;
    n = a11y_node_at(a, node_index);
    if (!n) return;
    n->state |= NK_A11Y_STATE_FOCUSED;
    a->focused_id = n->id;
    a->focused_index = node_index;
}

/* ----------------------------------------------------------------
 * Snapshot
 * ---------------------------------------------------------------- */
NK_API struct nk_a11y_snapshot
nk_a11y_snapshot_get(const struct nk_context *ctx)
{
    struct nk_a11y_snapshot snap;
    NK_MEMSET(&snap, 0, sizeof(snap));
    snap.root = 0;
    snap.focused = -1;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return snap;
    snap.nodes = (const struct nk_a11y_node *const *)ctx->a11y->flat;
    snap.count = ctx->a11y->flat_count;
    snap.focused = ctx->a11y->focused_index;
    snap.frame = ctx->a11y->frame;
    return snap;
}

static struct nk_a11y_node*
a11y_find(struct nk_a11y_runtime *a, nk_uint id)
{
    int i;
    for (i = 0; i < a->flat_count; ++i)
        if (a->flat[i]->id == id) return a->flat[i];
    return 0;
}

static int
a11y_index_of(struct nk_a11y_runtime *a, const struct nk_a11y_node *target)
{
    int i;
    for (i = 0; i < a->flat_count; ++i)
        if (a->flat[i] == target) return i;
    return -1;
}

NK_API const struct nk_a11y_node*
nk_a11y_find_by_id(const struct nk_context *ctx, nk_uint id)
{
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return 0;
    return a11y_find(ctx->a11y, id);
}

/* ----------------------------------------------------------------
 * Role / state names
 * ---------------------------------------------------------------- */
NK_API const char*
nk_a11y_role_name(enum nk_a11y_role role)
{
    switch (role) {
    case NK_A11Y_ROLE_WINDOW: return "window";
    case NK_A11Y_ROLE_GROUP: return "group";
    case NK_A11Y_ROLE_PANE: return "pane";
    case NK_A11Y_ROLE_BUTTON: return "button";
    case NK_A11Y_ROLE_TOGGLE_BUTTON: return "toggle_button";
    case NK_A11Y_ROLE_RADIO_BUTTON: return "radio_button";
    case NK_A11Y_ROLE_TEXT_INPUT: return "text_input";
    case NK_A11Y_ROLE_COMBO_BOX: return "combo_box";
    case NK_A11Y_ROLE_LIST: return "list";
    case NK_A11Y_ROLE_LIST_ITEM: return "list_item";
    case NK_A11Y_ROLE_MENU: return "menu";
    case NK_A11Y_ROLE_MENU_ITEM: return "menu_item";
    case NK_A11Y_ROLE_TREE_ITEM: return "tree_item";
    case NK_A11Y_ROLE_SLIDER: return "slider";
    case NK_A11Y_ROLE_PROGRESS_BAR: return "progress_bar";
    case NK_A11Y_ROLE_SPIN_BUTTON: return "spin_button";
    case NK_A11Y_ROLE_LABEL: return "label";
    case NK_A11Y_ROLE_LINK: return "link";
    default: return "none";
    }
}

NK_API const char*
nk_a11y_state_name(nk_flags state)
{
    static char buf[160];
    int pos = 0;
    #define A11Y_STATE(f, s) if (state & (f)) { \
        int slen = a11y_strlen(s); \
        if (pos && pos + 1 < (int)sizeof(buf)) buf[pos++] = '|'; \
        if (pos + slen < (int)sizeof(buf)) { NK_MEMCPY(buf + pos, s, (nk_size)slen); pos += slen; } }
    A11Y_STATE(NK_A11Y_STATE_DISABLED, "disabled")
    A11Y_STATE(NK_A11Y_STATE_FOCUSED, "focused")
    A11Y_STATE(NK_A11Y_STATE_CHECKED, "checked")
    A11Y_STATE(NK_A11Y_STATE_EXPANDED, "expanded")
    A11Y_STATE(NK_A11Y_STATE_COLLAPSED, "collapsed")
    A11Y_STATE(NK_A11Y_STATE_MODAL, "modal")
    A11Y_STATE(NK_A11Y_STATE_CLIPPED, "clipped")
    A11Y_STATE(NK_A11Y_STATE_FOCUSABLE, "focusable")
    A11Y_STATE(NK_A11Y_STATE_MULTILINE, "multiline")
    #undef A11Y_STATE
    if (pos >= (int)sizeof(buf)) pos = (int)sizeof(buf) - 1;
    buf[pos] = 0;
    return buf;
}

/* ----------------------------------------------------------------
 * JSON serialisation (bounded, no heap allocation, fully escaped)
 * ---------------------------------------------------------------- */
struct a11y_json_writer {
    char *buf;
    int cap;
    int len;      /* bytes that would be written excluding NUL */
    nk_bool ok;   /* nk_false once the buffer overflows */
};

static void
jw_init(struct a11y_json_writer *w, char *buf, int cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = nk_true;
}

static void
jw_put(struct a11y_json_writer *w, char c)
{
    if (w->len + 1 < w->cap)
        w->buf[w->len] = c;
    else
        w->ok = nk_false;
    w->len++;
}

static void
jw_raw(struct a11y_json_writer *w, const char *s)
{
    while (*s) jw_put(w, *s++);
}

/* bounded integer formatter, avoids a varargs/stdio dependency */
static void
jw_uint(struct a11y_json_writer *w, unsigned int v)
{
    char tmp[16];
    int n = 0;
    if (v == 0) { jw_put(w, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) jw_put(w, tmp[--n]);
}

static void
jw_int(struct a11y_json_writer *w, int v)
{
    if (v < 0) {
        jw_put(w, '-');
        if (v == -2147483647 - 1) { jw_raw(w, "2147483648"); return; }
        v = -v;
    }
    jw_uint(w, (unsigned int)v);
}

/* one decimal place; bounds are layout integers in practice */
static void
jw_float1(struct a11y_json_writer *w, float fv)
{
    int whole, tenth;
    if (fv < 0) { jw_put(w, '-'); fv = -fv; }
    whole = (int)fv;
    tenth = (int)((fv - (float)whole) * 10.0f + 0.5f);
    if (tenth >= 10) { whole++; tenth -= 10; }
    jw_int(w, whole);
    jw_put(w, '.');
    jw_put(w, (char)('0' + tenth));
}

/* JSON string escape with \u00XX encoding for control characters. */
static void
jw_string(struct a11y_json_writer *w, const char *s)
{
    const unsigned char *p = (const unsigned char*)(s ? s : "");
    jw_put(w, '"');
    while (*p) {
        unsigned char c = *p++;
        switch (c) {
        case '"': jw_raw(w, "\\\""); break;
        case '\\': jw_raw(w, "\\\\"); break;
        case '\b': jw_raw(w, "\\b"); break;
        case '\f': jw_raw(w, "\\f"); break;
        case '\n': jw_raw(w, "\\n"); break;
        case '\r': jw_raw(w, "\\r"); break;
        case '\t': jw_raw(w, "\\t"); break;
        default:
            if (c < 0x20) {
                static const char hexd[] = "0123456789abcdef";
                jw_raw(w, "\\u00");
                jw_put(w, hexd[(c >> 4) & 0xF]);
                jw_put(w, hexd[c & 0xF]);
            } else jw_put(w, (char)c);
            break;
        }
    }
    jw_put(w, '"');
}

static void
jw_node(struct a11y_json_writer *w, const struct nk_a11y_runtime *a,
    const struct nk_a11y_node *n, int depth)
{
    int i;
    for (i = 0; i < depth; ++i) jw_raw(w, "  ");
    jw_raw(w, "{\"id\":");
    jw_uint(w, n->id);
    jw_raw(w, ",\"role\":");
    jw_string(w, nk_a11y_role_name(n->role));
    jw_raw(w, ",\"name\":");
    jw_string(w, n->name);
    jw_raw(w, ",\"value\":");
    if (n->value) jw_string(w, n->value); else jw_raw(w, "null");
    jw_raw(w, ",\"state\":");
    jw_uint(w, n->state);
    jw_raw(w, ",\"state_name\":");
    jw_string(w, nk_a11y_state_name(n->state));
    jw_raw(w, ",\"bounds\":[");
    jw_float1(w, n->bounds.x); jw_put(w, ',');
    jw_float1(w, n->bounds.y); jw_put(w, ',');
    jw_float1(w, n->bounds.w); jw_put(w, ',');
    jw_float1(w, n->bounds.h);
    jw_raw(w, "],\"parent\":");
    jw_int(w, n->parent);
    jw_raw(w, ",\"focus_order\":");
    jw_int(w, n->focus_order);
    if (n->first_child >= 0) {
        const struct nk_a11y_node *child = a->flat[n->first_child];
        jw_raw(w, ",\"children\":[");
        jw_put(w, '\n');
        while (child) {
            jw_node(w, a, child, depth + 1);
            if (child->next_sibling >= 0) jw_raw(w, ",");
            jw_put(w, '\n');
            child = child->next_sibling >= 0 ?
                a->flat[child->next_sibling] : 0;
        }
        for (i = 0; i < depth; ++i) jw_raw(w, "  ");
        jw_raw(w, "]}");
    } else {
        jw_raw(w, ",\"children\":[]}");
    }
}

NK_API int
nk_a11y_snapshot_dump_json(const struct nk_context *ctx,
    char *buffer, int capacity)
{
    struct a11y_json_writer w;
    const struct nk_a11y_node *root;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled || capacity <= 0)
        return -1;
    jw_init(&w, buffer, capacity);
    root = ctx->a11y->flat[0];
    jw_raw(&w, "{\n  \"frame\":");
    jw_uint(&w, ctx->a11y->frame);
    jw_raw(&w, ",\n  \"node_count\":");
    jw_int(&w, ctx->a11y->flat_count);
    jw_raw(&w, ",\n  \"focused_id\":");
    jw_uint(&w, ctx->a11y->focused_id);
    jw_raw(&w, ",\n  \"tree\":\n");
    jw_node(&w, ctx->a11y, root, 1);
    jw_raw(&w, "\n}\n");
    if (w.ok && w.len < w.cap) {
        buffer[w.len] = 0;
        return w.len;
    }
    return w.len; /* caller retries with a buffer of len+1 */
}

/* ----------------------------------------------------------------
 * Focus management
 * ---------------------------------------------------------------- */
static nk_bool
a11y_is_focusable(const struct nk_a11y_node *n)
{
    if (!n) return nk_false;
    if (n->role == NK_A11Y_ROLE_NONE) return nk_false;
    if (n->state & NK_A11Y_STATE_DISABLED) return nk_false;
    if (n->focus_order < 0) return nk_false;
    return nk_true;
}

/* When a modal popup is open only its subtree may receive focus. */
static nk_bool
a11y_in_modal(struct nk_a11y_runtime *a, const struct nk_a11y_node *n)
{
    const struct nk_a11y_node *cur = n;
    int guard = 0;
    if (a->modal_node < 0) return nk_true;
    while (cur && guard++ < a->flat_count + 1) {
        if (a11y_index_of(a, cur) == a->modal_node) return nk_true;
        if (cur->parent < 0) return nk_false;
        cur = a11y_node_at(a, cur->parent);
    }
    return nk_false;
}

static struct nk_a11y_node*
a11y_focusable_at_order(struct nk_a11y_runtime *a, int order)
{
    int i;
    for (i = 0; i < a->flat_count; ++i) {
        struct nk_a11y_node *n = a->flat[i];
        if (n->focus_order == order && a11y_in_modal(a, n))
            return n;
    }
    return 0;
}

static void
a11y_clear_focus_flags(struct nk_a11y_runtime *a)
{
    int i;
    for (i = 0; i < a->flat_count; ++i)
        a->flat[i]->state &= ~(nk_flags)NK_A11Y_STATE_FOCUSED;
}

static nk_bool
a11y_apply_focus(struct nk_a11y_runtime *a, struct nk_a11y_node *n)
{
    if (!n || !a11y_is_focusable(n) || !a11y_in_modal(a, n))
        return nk_false;
    a11y_clear_focus_flags(a);
    n->state |= NK_A11Y_STATE_FOCUSED;
    a->focused_id = n->id;
    a->focused_index = a11y_index_of(a, n);
    return nk_true;
}

NK_API nk_bool
nk_a11y_set_focus(struct nk_context *ctx, nk_uint id)
{
    struct nk_a11y_node *n;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    n = a11y_find(ctx->a11y, id);
    return a11y_apply_focus(ctx->a11y, n);
}

NK_API nk_bool
nk_a11y_focus_clear(struct nk_context *ctx)
{
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    a11y_clear_focus_flags(ctx->a11y);
    ctx->a11y->focused_id = 0;
    ctx->a11y->focused_index = -1;
    return nk_true;
}

NK_API nk_uint
nk_a11y_focused_id(const struct nk_context *ctx)
{
    if (!ctx || !ctx->a11y) return 0;
    return ctx->a11y->focused_id;
}

NK_API nk_bool
nk_a11y_focus_first(struct nk_context *ctx)
{
    struct nk_a11y_node *n;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    n = a11y_focusable_at_order(ctx->a11y, 0);
    return a11y_apply_focus(ctx->a11y, n);
}

NK_API nk_bool
nk_a11y_focus_last(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    int order;
    struct nk_a11y_node *n = 0;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    a = ctx->a11y;
    for (order = 0; order < a->focus_counter; ++order) {
        struct nk_a11y_node *cand = a11y_focusable_at_order(a, order);
        if (cand) n = cand;
    }
    return a11y_apply_focus(a, n);
}

NK_API nk_bool
nk_a11y_focus_next(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    int order, start = -1;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    a = ctx->a11y;
    if (a->focused_index >= 0) {
        struct nk_a11y_node *cur = a11y_node_at(a, a->focused_index);
        if (cur) start = cur->focus_order;
    }
    for (order = start + 1; order < a->focus_counter; ++order) {
        struct nk_a11y_node *n = a11y_focusable_at_order(a, order);
        if (n && a11y_apply_focus(a, n)) return nk_true;
    }
    return nk_a11y_focus_first(ctx); /* wrap around */
}

NK_API nk_bool
nk_a11y_focus_prev(struct nk_context *ctx)
{
    struct nk_a11y_runtime *a;
    int order, start = -1;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    a = ctx->a11y;
    if (a->focused_index >= 0) {
        struct nk_a11y_node *cur = a11y_node_at(a, a->focused_index);
        if (cur) start = cur->focus_order;
    }
    for (order = start - 1; order >= 0; --order) {
        struct nk_a11y_node *n = a11y_focusable_at_order(a, order);
        if (n && a11y_apply_focus(a, n)) return nk_true;
    }
    return nk_a11y_focus_last(ctx);
}

/* ----------------------------------------------------------------
 * Activation (replayed as ordinary input)
 * ---------------------------------------------------------------- */
NK_API nk_bool
nk_a11y_activate(struct nk_context *ctx, nk_uint id)
{
    struct nk_a11y_runtime *a;
    struct nk_a11y_node *n;
    if (!ctx || !ctx->a11y || !ctx->a11y->enabled) return nk_false;
    a = ctx->a11y;
    n = a11y_find(a, id);
    if (!n) return nk_false;
    if (n->state & NK_A11Y_STATE_DISABLED) return nk_false;
    if (!a11y_in_modal(a, n)) return nk_false;
    /* queue a synthetic left click at the widget centre; the press and
     * release are injected across the next two frames by
     * nk_a11y_begin_frame (driven from nk_input_begin) so the regular
     * immediate mode widget logic processes them as genuine input */
    a->pending.x = (int)(n->bounds.x + n->bounds.w * 0.5f);
    a->pending.y = (int)(n->bounds.y + n->bounds.h * 0.5f);
    a->pending.valid = nk_true;
    a->release_queued = nk_false;
    a11y_apply_focus(a, n);
    return nk_true;
}

NK_API void
nk_a11y_pump_input(struct nk_context *ctx)
{
    /* Input is delivered from nk_a11y_begin_frame on the following
     * nk_input_begin calls; this hook exists so applications can keep
     * an explicit pump step and remains safe to call repeatedly. */
    NK_UNUSED(ctx);
}

#else /* NK_INCLUDE_ACCESSIBILITY */

/* Stubs keep user code compiling unchanged when the layer is disabled. */
struct nk_a11y_runtime;

NK_API nk_bool nk_a11y_enable(struct nk_context *ctx, struct nk_allocator *alloc)
{ NK_UNUSED(ctx); NK_UNUSED(alloc); return nk_false; }
NK_API void nk_a11y_disable(struct nk_context *ctx) { NK_UNUSED(ctx); }
NK_API nk_bool nk_a11y_is_enabled(const struct nk_context *ctx)
{ NK_UNUSED(ctx); return nk_false; }
NK_API void nk_a11y_frame_reset(struct nk_context *ctx) { NK_UNUSED(ctx); }
NK_API struct nk_a11y_snapshot nk_a11y_snapshot_get(const struct nk_context *ctx)
{ struct nk_a11y_snapshot s; NK_MEMSET(&s, 0, sizeof(s)); s.root = 0; s.focused = -1;
  NK_UNUSED(ctx); return s; }
NK_API const struct nk_a11y_node *
nk_a11y_find_by_id(const struct nk_context *ctx, nk_uint id)
{ NK_UNUSED(ctx); NK_UNUSED(id); return 0; }
NK_API int nk_a11y_snapshot_dump_json(const struct nk_context *ctx,
    char *b, int c) { NK_UNUSED(ctx); NK_UNUSED(b); NK_UNUSED(c); return -1; }
NK_API nk_bool nk_a11y_set_focus(struct nk_context *ctx, nk_uint id)
{ NK_UNUSED(ctx); NK_UNUSED(id); return nk_false; }
NK_API nk_bool nk_a11y_focus_next(struct nk_context *ctx) { NK_UNUSED(ctx); return nk_false; }
NK_API nk_bool nk_a11y_focus_prev(struct nk_context *ctx) { NK_UNUSED(ctx); return nk_false; }
NK_API nk_bool nk_a11y_focus_first(struct nk_context *ctx) { NK_UNUSED(ctx); return nk_false; }
NK_API nk_bool nk_a11y_focus_last(struct nk_context *ctx) { NK_UNUSED(ctx); return nk_false; }
NK_API nk_bool nk_a11y_focus_clear(struct nk_context *ctx) { NK_UNUSED(ctx); return nk_false; }
NK_API nk_uint nk_a11y_focused_id(const struct nk_context *ctx)
{ NK_UNUSED(ctx); return 0; }
NK_API nk_bool nk_a11y_activate(struct nk_context *ctx, nk_uint id)
{ NK_UNUSED(ctx); NK_UNUSED(id); return nk_false; }
NK_API void nk_a11y_pump_input(struct nk_context *ctx) { NK_UNUSED(ctx); }
NK_API const char *nk_a11y_role_name(enum nk_a11y_role role)
{ NK_UNUSED(role); return "none"; }
NK_API const char *nk_a11y_state_name(nk_flags state)
{ NK_UNUSED(state); return ""; }

#endif /* NK_INCLUDE_ACCESSIBILITY */
