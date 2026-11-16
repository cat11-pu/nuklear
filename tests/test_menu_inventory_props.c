/*
 * Property test for game menu/inventory UIs built with Nuklear.
 *
 * The test generates a deterministic scene tree for every case:
 *   - overlapping top-level menu/inventory windows
 *   - nested popup windows and nested groups
 *   - clipped groups with scrollbars
 *   - alternating hidden controls and minimized/hidden windows
 *   - per-frame style changes with balanced push/pop stacks
 *
 * Every frame is rendered with two independent contexts. Their complete
 * command streams and layout rectangles must match. A tracking allocator
 * verifies that nk_free returns every allocation. Failing scenes are reduced
 * by deleting subtrees and the smallest failing tree is printed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <errno.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_IMPLEMENTATION
#include "../nuklear.h"

#define VIEW_W 1280
#define VIEW_H 720
#define MAX_NODES 48
#define MAX_FRAMES 32
#define MAX_LAYOUTS 512
#define MAX_COMMANDS 8192
#define NAME_LEN 32
#define LABEL_LEN 64

enum node_kind {
    NODE_MENU_WINDOW,
    NODE_INVENTORY_WINDOW,
    NODE_POPUP,
    NODE_GROUP,
    NODE_BUTTON,
    NODE_LABEL,
    NODE_SLIDER,
    NODE_CHECKBOX,
    NODE_PROGRESS
};

struct scene_node {
    enum node_kind kind;
    int parent;
    int alive;
    char id[NAME_LEN];
    char label[LABEL_LEN];
    int style_id;
    int hidden_phase;       /* -1 = always visible; otherwise alternating */
    int minimized_phase;    /* -1 = never minimized; otherwise periodic */
    int window_flags;
    int clip_mode;
    float row_height;
    struct nk_rect bounds;
};

struct scenario {
    uint64_t seed;
    int case_no;
    int node_count;
    struct scene_node nodes[MAX_NODES];
};

struct layout_record {
    char id[NAME_LEN];
    char kind[20];
    struct nk_rect rect;
    struct nk_rect clip;
    int has_clip;
};

struct command_record {
    enum nk_command_type type;
    struct nk_rect rect;
    struct nk_color color;
    char text[40];
};

struct hidden_record {
    char id[NAME_LEN];
    char kind[20];
};

struct frame_record {
    int frame;
    int command_count;
    int layout_count;
    int hidden_count;
    int active_container_count;
    int visible_windows;
    int overflow;
    int bad_geometry;
    int bad_clip;
    int bad_style;
    struct nk_rect bad_clip_rect;
    uint64_t command_hash;
    uint64_t layout_hash;
    uint64_t hidden_hash;
    uint64_t active_hash;
    struct command_record commands[MAX_COMMANDS];
    struct layout_record layouts[MAX_LAYOUTS];
    struct hidden_record hidden[MAX_NODES];
    char active_containers[MAX_NODES][NAME_LEN];
};

struct ui_state {
    float slider[MAX_NODES];
    int checkbox[MAX_NODES];
    nk_size progress[MAX_NODES];
};

enum failure_kind {
    FAIL_NONE = 0,
    FAIL_ALLOCATOR,
    FAIL_LEAK,
    FAIL_LAYOUT_DETERMINISM,
    FAIL_COMMAND_DETERMINISM,
    FAIL_HIDDEN_DETERMINISM,
    FAIL_LAYOUT_GEOMETRY,
    FAIL_CLIP_GEOMETRY,
    FAIL_STYLE_STACK,
    FAIL_RECORD_OVERFLOW,
    FAIL_NO_COMMANDS
};

struct failure {
    enum failure_kind kind;
    int frame;
    char message[320];
};

struct stats {
    int cases;
    int frames;
    int failures;
    int skipped;
    int commands;
    int layouts;
    int hidden;
    int min_commands;
    int max_commands;
    int min_layouts;
    int max_layouts;
    int command_type_counts[NK_COMMAND_CUSTOM + 1];
    int failure_kind_counts[FAIL_NO_COMMANDS + 1];
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t realloc_calls;
    uint64_t bad_alloc_ops;
    size_t peak_live_bytes;
};

struct frame_output {
    FILE *file;
};

struct global_config {
    int cases;
    int frames;
    uint64_t seed;
    FILE *frames_file;
    FILE *summary_file;
    FILE *failure_file;
};

static struct stats g_stats;
static struct global_config g_config;

static const char *const g_command_names[] = {
    "nop", "scissor", "line", "curve", "rect", "rect_filled",
    "rect_multi_color", "circle", "circle_filled", "arc", "arc_filled",
    "triangle", "triangle_filled", "polygon", "polygon_filled", "polyline",
    "text", "image", "custom"
};

static const char *const g_node_names[] = {
    "menu_window", "inventory_window", "popup", "group", "button",
    "label", "slider", "checkbox", "progress"
};

static uint64_t
mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static uint64_t
rng_next(uint64_t *state)
{
    *state += UINT64_C(0x9e3779b97f4a7c15);
    return mix64(*state);
}

static int
rng_range(uint64_t *state, int min_value, int max_value)
{
    uint64_t span = (uint64_t)(max_value - min_value + 1);
    return min_value + (int)(rng_next(state) % span);
}

static void
copy_string(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int
add_node(struct scenario *sc, enum node_kind kind, int parent,
    const char *id, const char *label)
{
    int index;
    struct scene_node *node;
    if (sc->node_count >= MAX_NODES)
        return -1;
    index = sc->node_count++;
    node = &sc->nodes[index];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->parent = parent;
    node->alive = 1;
    node->hidden_phase = -1;
    node->minimized_phase = -1;
    node->style_id = 0;
    node->clip_mode = 0;
    node->row_height = 28.0f;
    copy_string(node->id, sizeof(node->id), id);
    copy_string(node->label, sizeof(node->label), label);
    return index;
}

static int
is_container(enum node_kind kind)
{
    return kind == NODE_MENU_WINDOW || kind == NODE_INVENTORY_WINDOW ||
        kind == NODE_POPUP || kind == NODE_GROUP;
}

static int
is_window(enum node_kind kind)
{
    return kind == NODE_MENU_WINDOW || kind == NODE_INVENTORY_WINDOW;
}

static int
is_control(enum node_kind kind)
{
    return kind == NODE_BUTTON || kind == NODE_LABEL || kind == NODE_SLIDER ||
        kind == NODE_CHECKBOX || kind == NODE_PROGRESS;
}

static int
node_visible(const struct scene_node *node, int frame)
{
    if (node->hidden_phase < 0)
        return 1;
    if (is_window(node->kind))
        return ((frame + node->hidden_phase) % 5) != 0;
    if (node->kind == NODE_POPUP)
        return ((frame + node->hidden_phase) % 3) != 0;
    return ((frame + node->hidden_phase) & 1) == 0;
}

static int
node_minimized(const struct scene_node *node, int frame)
{
    if (node->minimized_phase < 0)
        return 0;
    return ((frame + node->minimized_phase) % 7) == 0;
}

static int
node_effectively_visible(const struct scenario *sc, int node_index, int frame)
{
    int current = node_index;
    while (current >= 0) {
        const struct scene_node *node = &sc->nodes[current];
        if (!node->alive || !node_visible(node, frame))
            return 0;
        if (is_window(node->kind) && node_minimized(node, frame))
            return 0;
        current = node->parent;
    }
    return 1;
}

static void
mark_subtree_dead(struct scenario *sc, int node_index)
{
    int i;
    int changed;
    if (node_index < 0 || node_index >= sc->node_count)
        return;
    sc->nodes[node_index].alive = 0;
    do {
        changed = 0;
        for (i = 0; i < sc->node_count; ++i) {
            int parent = sc->nodes[i].parent;
            if (!sc->nodes[i].alive || parent < 0)
                continue;
            if (!sc->nodes[parent].alive) {
                sc->nodes[i].alive = 0;
                changed = 1;
            }
        }
    } while (changed);
}

static int
count_alive(const struct scenario *sc)
{
    int i;
    int count = 0;
    for (i = 0; i < sc->node_count; ++i)
        count += sc->nodes[i].alive != 0;
    return count;
}

static int
generate_children(struct scenario *sc, uint64_t *rng, int parent, int depth,
    int max_depth, const char *prefix, int *serial)
{
    int child_count;
    int i;
    int popup_added = 0;
    child_count = rng_range(rng, 2, depth == 0 ? 6 : 4);
    for (i = 0; i < child_count; ++i) {
        int choice = rng_range(rng, 0, 99);
        char id[NAME_LEN];
        char label[LABEL_LEN];
        int index;

        snprintf(id, sizeof(id), "%s_%d_%d", prefix, *serial, i);
        ++*serial;

        if (depth < max_depth && choice < 42) {
            struct scene_node *node;
            index = add_node(sc, NODE_GROUP, parent, id, "Section");
            if (index < 0) return 0;
            node = &sc->nodes[index];
            node->style_id = rng_range(rng, 0, 3);
            node->hidden_phase = rng_range(rng, 0, 1);
            node->clip_mode = rng_range(rng, 0, 2);
            node->row_height = (float)rng_range(rng, 64, 154);
            snprintf(node->label, sizeof(node->label), "Group %d", *serial);
            generate_children(sc, rng, index, depth + 1, max_depth,
                "g", serial);
        } else if (choice < 86) {
            enum node_kind kind;
            int pick = rng_range(rng, 0, 4);
            kind = (enum node_kind)(NODE_BUTTON + pick);
            snprintf(label, sizeof(label),
                (kind == NODE_BUTTON) ? "Use / Equip %d" :
                (kind == NODE_LABEL) ? "Description row %d" :
                (kind == NODE_SLIDER) ? "Volume / Weight %d" :
                (kind == NODE_CHECKBOX) ? "Show advanced stats %d" :
                "Durability %d", *serial);
            index = add_node(sc, kind, parent, id, label);
            if (index < 0) return 0;
            sc->nodes[index].style_id = rng_range(rng, 0, 3);
            sc->nodes[index].hidden_phase = rng_range(rng, 0, 1);
            sc->nodes[index].row_height = (float)rng_range(rng, 22, 32);
        } else if (!popup_added && depth == 0) {
            struct scene_node *node;
            index = add_node(sc, NODE_POPUP, parent, id, "Item actions");
            if (index < 0) return 0;
            popup_added = 1;
            node = &sc->nodes[index];
            node->style_id = rng_range(rng, 0, 3);
            node->hidden_phase = rng_range(rng, 0, 2);
            node->clip_mode = rng_range(rng, 0, 2);
            node->bounds = nk_rect((float)rng_range(rng, 8, 80),
                (float)rng_range(rng, 36, 110),
                (float)rng_range(rng, 180, 320),
                (float)rng_range(rng, 160, 320));
            generate_children(sc, rng, index, depth + 1, max_depth,
                "p", serial);
        } else {
            index = add_node(sc, NODE_LABEL, parent, id, "Divider");
            if (index < 0) return 0;
            sc->nodes[index].hidden_phase = rng_range(rng, 0, 1);
            sc->nodes[index].row_height = 20.0f;
        }
    }
    return 1;
}

static void
generate_scenario(struct scenario *sc, int case_no, uint64_t base_seed)
{
    uint64_t rng;
    int serial = 0;
    int menu;
    int inventory;
    struct scene_node *node;

    memset(sc, 0, sizeof(*sc));
    sc->case_no = case_no;
    sc->seed = mix64(base_seed ^ (UINT64_C(0x9e3779b97f4a7c15) *
        (uint64_t)(case_no + 1)));
    rng = sc->seed;

    menu = add_node(sc, NODE_MENU_WINDOW, -1, "menu", "Pause Menu");
    node = &sc->nodes[menu];
    node->style_id = rng_range(&rng, 0, 3);
    node->hidden_phase = rng_range(&rng, 0, 4);
    node->minimized_phase = (rng_range(&rng, 0, 2) == 0) ? -1 :
        rng_range(&rng, 0, 6);
    node->window_flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE |
        NK_WINDOW_MOVABLE | NK_WINDOW_MINIMIZABLE;
    if (rng_range(&rng, 0, 3) == 0)
        node->window_flags |= NK_WINDOW_NO_SCROLLBAR;
    if (rng_range(&rng, 0, 3) == 0)
        node->window_flags |= NK_WINDOW_CLOSABLE;
    node->bounds = nk_rect((float)rng_range(&rng, 20, 100),
        (float)rng_range(&rng, 24, 80),
        (float)rng_range(&rng, 360, 500),
        (float)rng_range(&rng, 340, 580));

    inventory = add_node(sc, NODE_INVENTORY_WINDOW, -1,
        "inventory", "Inventory");
    node = &sc->nodes[inventory];
    node->style_id = rng_range(&rng, 0, 3);
    node->hidden_phase = rng_range(&rng, 0, 4);
    node->minimized_phase = (rng_range(&rng, 0, 2) == 0) ? -1 :
        rng_range(&rng, 0, 6);
    node->window_flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE |
        NK_WINDOW_MOVABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_SCALABLE;
    if (rng_range(&rng, 0, 3) == 0)
        node->window_flags |= NK_WINDOW_SCROLL_AUTO_HIDE;
    node->bounds = nk_rect((float)rng_range(&rng, 470, 590),
        (float)rng_range(&rng, 65, 140),
        (float)rng_range(&rng, 470, 620),
        (float)rng_range(&rng, 360, 540));

    generate_children(sc, &rng, menu, 0, 3, "m", &serial);
    generate_children(sc, &rng, inventory, 0, 3, "i", &serial);
}

/* =========================================================================
 * Tracking allocator
 * ========================================================================= */

#define ALLOC_MAGIC UINT64_C(0x6e6b616c6c6f6331)
#define ALLOC_HEADER_SIZE ((sizeof(struct alloc_block) + 15u) & ~(size_t)15u)

struct alloc_block {
    uint64_t magic;
    size_t size;
    struct alloc_block *next;
    struct alloc_block *prev;
};

struct allocator_state {
    struct alloc_block *head;
    size_t live_bytes;
    size_t peak_bytes;
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t realloc_calls;
    uint64_t bad_ops;
};

static void
alloc_link(struct allocator_state *state, struct alloc_block *block)
{
    block->prev = NULL;
    block->next = state->head;
    if (state->head)
        state->head->prev = block;
    state->head = block;
    state->live_bytes += block->size;
    if (state->live_bytes > state->peak_bytes)
        state->peak_bytes = state->live_bytes;
    if (state->peak_bytes > g_stats.peak_live_bytes)
        g_stats.peak_live_bytes = state->peak_bytes;
}

static void
alloc_unlink(struct allocator_state *state, struct alloc_block *block)
{
    if (block->prev)
        block->prev->next = block->next;
    else
        state->head = block->next;
    if (block->next)
        block->next->prev = block->prev;
    state->live_bytes -= block->size;
}

static void *
tracked_alloc(nk_handle handle, void *old_ptr, nk_size size)
{
    struct allocator_state *state = (struct allocator_state*)handle.ptr;
    struct alloc_block *block;
    void *result;

    if (!state) return NULL;
    if (old_ptr) {
        struct alloc_block *old_block = (struct alloc_block*)
            ((char*)old_ptr - ALLOC_HEADER_SIZE);
        size_t copy_size;
        state->realloc_calls++;
        ++g_stats.realloc_calls;
        if (old_block->magic != ALLOC_MAGIC) {
            state->bad_ops++;
            ++g_stats.bad_alloc_ops;
            return NULL;
        }
        if (size == 0) {
            alloc_unlink(state, old_block);
            old_block->magic = 0;
            free(old_block);
            state->free_calls++;
            ++g_stats.free_calls;
            return NULL;
        }
        block = (struct alloc_block*)malloc(ALLOC_HEADER_SIZE + size);
        if (!block) {
            state->bad_ops++;
            ++g_stats.bad_alloc_ops;
            return NULL;
        }
        copy_size = old_block->size < size ? old_block->size : size;
        result = (char*)block + ALLOC_HEADER_SIZE;
        if (copy_size)
            memcpy(result, old_ptr, copy_size);
        alloc_unlink(state, old_block);
        old_block->magic = 0;
        free(old_block);
        state->free_calls++;
        ++g_stats.free_calls;
        block->magic = ALLOC_MAGIC;
        block->size = (size_t)size;
        alloc_link(state, block);
        state->alloc_calls++;
        ++g_stats.alloc_calls;
        return result;
    }

    if (size == 0)
        return NULL;
    block = (struct alloc_block*)malloc(ALLOC_HEADER_SIZE + size);
    if (!block) {
        state->bad_ops++;
        ++g_stats.bad_alloc_ops;
        return NULL;
    }
    block->magic = ALLOC_MAGIC;
    block->size = (size_t)size;
    alloc_link(state, block);
    state->alloc_calls++;
    ++g_stats.alloc_calls;
    return (char*)block + ALLOC_HEADER_SIZE;
}

static void
tracked_free(nk_handle handle, void *old_ptr)
{
    struct allocator_state *state = (struct allocator_state*)handle.ptr;
    struct alloc_block *block;
    if (!state || !old_ptr)
        return;
    block = (struct alloc_block*)((char*)old_ptr - ALLOC_HEADER_SIZE);
    if (block->magic != ALLOC_MAGIC) {
        state->bad_ops++;
        ++g_stats.bad_alloc_ops;
        return;
    }
    alloc_unlink(state, block);
    block->magic = 0;
    free(block);
    state->free_calls++;
    ++g_stats.free_calls;
}

static void
allocator_force_release(struct allocator_state *state)
{
    struct alloc_block *iter = state->head;
    while (iter) {
        struct alloc_block *next = iter->next;
        iter->magic = 0;
        free(iter);
        iter = next;
    }
    state->head = NULL;
    state->live_bytes = 0;
}

/* =========================================================================
 * Hashing and fake font
 * ========================================================================= */

static void
hash_u64(uint64_t *hash, uint64_t value)
{
    int i;
    for (i = 0; i < 8; ++i) {
        *hash ^= (uint8_t)(value >> (i * 8));
        *hash *= UINT64_C(1099511628211);
    }
}

static void
hash_u32(uint64_t *hash, uint32_t value)
{
    int i;
    for (i = 0; i < 4; ++i) {
        *hash ^= (uint8_t)(value >> (i * 8));
        *hash *= UINT64_C(1099511628211);
    }
}

static void
hash_float(uint64_t *hash, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    if (bits == UINT32_C(0x80000000))
        bits = 0;
    hash_u32(hash, bits);
}

static void
hash_rect(uint64_t *hash, struct nk_rect rect)
{
    hash_float(hash, rect.x);
    hash_float(hash, rect.y);
    hash_float(hash, rect.w);
    hash_float(hash, rect.h);
}

static void
hash_color(uint64_t *hash, struct nk_color color)
{
    hash_u32(hash, ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
        ((uint32_t)color.b << 8) | color.a);
}

static void
hash_string(uint64_t *hash, const char *text)
{
    const unsigned char *iter = (const unsigned char*)text;
    while (*iter)
        hash_u32(hash, *iter++);
    hash_u32(hash, 0);
}

static float
fake_font_width(nk_handle handle, float height, const char *text, int len)
{
    (void)handle;
    if (!text || len <= 0)
        return 0.0f;
    return (float)len * (7.0f * height / 14.0f);
}

static struct nk_user_font g_font;

static void
font_init(void)
{
    memset(&g_font, 0, sizeof(g_font));
    g_font.height = 14.0f;
    g_font.width = fake_font_width;
}

/* =========================================================================
 * Style switching
 * ========================================================================= */

struct palette {
    struct nk_color background;
    struct nk_color border;
    struct nk_color text;
    struct nk_color button;
    struct nk_color button_hover;
    struct nk_color button_active;
    float rounding;
    struct nk_vec2 spacing;
};

static const struct palette g_palettes[4] = {
    {{28, 32, 42, 255}, {70, 82, 104, 255}, {224, 230, 240, 255},
     {48, 58, 78, 255}, {66, 82, 112, 255}, {36, 46, 64, 255},
     3.0f, {4.0f, 4.0f}},
    {{38, 28, 28, 255}, {112, 62, 54, 255}, {250, 224, 212, 255},
     {92, 44, 38, 255}, {132, 62, 52, 255}, {72, 32, 28, 255},
     8.0f, {6.0f, 3.0f}},
    {{24, 40, 32, 255}, {52, 100, 72, 255}, {220, 248, 226, 255},
     {36, 78, 54, 255}, {52, 112, 76, 255}, {28, 62, 42, 255},
     1.0f, {3.0f, 6.0f}},
    {{42, 38, 22, 255}, {118, 98, 44, 255}, {255, 244, 196, 255},
     {88, 74, 30, 255}, {128, 104, 44, 255}, {68, 56, 22, 255},
     12.0f, {2.0f, 2.0f}}
};

struct style_probe {
    struct nk_color text;
    struct nk_style_item normal;
    float rounding;
    struct nk_vec2 spacing;
};

static struct style_probe
style_probe_capture(const struct nk_context *ctx)
{
    struct style_probe probe;
    probe.text = ctx->style.text.color;
    probe.normal = ctx->style.button.normal;
    probe.rounding = ctx->style.button.rounding;
    probe.spacing = ctx->style.window.spacing;
    return probe;
}

static int
style_probe_equal(struct style_probe a, struct style_probe b)
{
    int color_equal = a.text.r == b.text.r && a.text.g == b.text.g &&
        a.text.b == b.text.b && a.text.a == b.text.a;
    int item_equal = a.normal.type == b.normal.type;
    if (item_equal && a.normal.type == NK_STYLE_ITEM_COLOR) {
        item_equal = a.normal.data.color.r == b.normal.data.color.r &&
            a.normal.data.color.g == b.normal.data.color.g &&
            a.normal.data.color.b == b.normal.data.color.b &&
            a.normal.data.color.a == b.normal.data.color.a;
    }
    return color_equal && item_equal && a.rounding == b.rounding &&
        a.spacing.x == b.spacing.x && a.spacing.y == b.spacing.y;
}

static int
style_stacks_balanced(const struct nk_context *ctx)
{
    return ctx->stacks.style_items.head == 0 &&
        ctx->stacks.floats.head == 0 &&
        ctx->stacks.vectors.head == 0 &&
        ctx->stacks.flags.head == 0 &&
        ctx->stacks.colors.head == 0 &&
        ctx->stacks.fonts.head == 0 &&
        ctx->stacks.button_behaviors.head == 0;
}

struct render_state {
    const struct scenario *sc;
    struct frame_record *record;
    struct ui_state ui;
    int frame;
    int style_depth;
};

static void
push_palette(struct nk_context *ctx, struct render_state *state, int style_id)
{
    const struct palette *palette = &g_palettes[style_id & 3];
    nk_style_push_style_item(ctx, &ctx->style.button.normal,
        nk_style_item_color(palette->button));
    nk_style_push_style_item(ctx, &ctx->style.button.hover,
        nk_style_item_color(palette->button_hover));
    nk_style_push_style_item(ctx, &ctx->style.button.active,
        nk_style_item_color(palette->button_active));
    nk_style_push_color(ctx, &ctx->style.text.color, palette->text);
    nk_style_push_float(ctx, &ctx->style.button.rounding, palette->rounding);
    nk_style_push_vec2(ctx, &ctx->style.window.spacing, palette->spacing);
    state->style_depth += 6;
}

static void
pop_palette(struct nk_context *ctx, struct render_state *state)
{
    nk_style_pop_vec2(ctx);
    nk_style_pop_float(ctx);
    nk_style_pop_color(ctx);
    nk_style_pop_style_item(ctx);
    nk_style_pop_style_item(ctx);
    nk_style_pop_style_item(ctx);
    state->style_depth -= 6;
}

/* =========================================================================
 * Recording
 * ========================================================================= */

static void
add_layout(struct frame_record *record, const char *id, const char *kind,
    struct nk_rect rect, const struct nk_rect *clip)
{
    struct layout_record *entry;
    if (record->layout_count >= MAX_LAYOUTS) {
        record->overflow = 1;
        return;
    }
    entry = &record->layouts[record->layout_count++];
    memset(entry, 0, sizeof(*entry));
    copy_string(entry->id, sizeof(entry->id), id);
    copy_string(entry->kind, sizeof(entry->kind), kind);
    entry->rect = rect;
    if (clip) {
        entry->clip = *clip;
        entry->has_clip = 1;
    }
    record->layout_hash ^= UINT64_C(1469598103934665603);
    hash_string(&record->layout_hash, kind);
    hash_string(&record->layout_hash, id);
    hash_rect(&record->layout_hash, rect);
    if (clip) {
        hash_rect(&record->layout_hash, *clip);
        hash_u32(&record->layout_hash, 1);
    } else {
        hash_u32(&record->layout_hash, 0);
    }
    if (!isfinite(rect.x) || !isfinite(rect.y) ||
        !isfinite(rect.w) || !isfinite(rect.h) ||
        rect.w < 0.0f || rect.h < 0.0f || rect.w > 100000.0f ||
        rect.h > 100000.0f) {
        record->bad_geometry = 1;
    }
    if (clip && (!isfinite(clip->x) || !isfinite(clip->y) ||
        !isfinite(clip->w) || !isfinite(clip->h) ||
        clip->w < 0.0f || clip->h < 0.0f)) {
        record->bad_geometry = 1;
    }
}

static void
add_hidden(struct frame_record *record, const char *id, const char *kind)
{
    struct hidden_record *entry;
    if (record->hidden_count >= MAX_NODES) {
        record->overflow = 1;
        return;
    }
    entry = &record->hidden[record->hidden_count++];
    memset(entry, 0, sizeof(*entry));
    copy_string(entry->id, sizeof(entry->id), id);
    copy_string(entry->kind, sizeof(entry->kind), kind);
    hash_string(&record->hidden_hash, kind);
    hash_string(&record->hidden_hash, id);
}

static void
add_active_container(struct frame_record *record, const char *id)
{
    if (record->active_container_count >= MAX_NODES) {
        record->overflow = 1;
        return;
    }
    copy_string(record->active_containers[record->active_container_count++],
        NAME_LEN, id);
    hash_string(&record->active_hash, id);
}

static int
container_is_active(const struct frame_record *record, const char *id)
{
    int i;
    for (i = 0; i < record->active_container_count; ++i) {
        if (strcmp(record->active_containers[i], id) == 0)
            return 1;
    }
    return 0;
}

static void
add_command(struct frame_record *record, enum nk_command_type type,
    struct nk_rect rect, struct nk_color color, const char *text)
{
    struct command_record *entry;
    if (record->command_count >= MAX_COMMANDS) {
        record->overflow = 1;
        return;
    }
    entry = &record->commands[record->command_count++];
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    entry->rect = rect;
    entry->color = color;
    if (text)
        copy_string(entry->text, sizeof(entry->text), text);
    hash_u64(&record->command_hash, (uint64_t)type);
    hash_rect(&record->command_hash, rect);
    hash_color(&record->command_hash, color);
    if (text) {
        hash_u64(&record->command_hash, 1);
        hash_string(&record->command_hash, text);
    } else {
        hash_u64(&record->command_hash, 0);
    }
    if (!isfinite(rect.x) || !isfinite(rect.y) ||
        !isfinite(rect.w) || !isfinite(rect.h) ||
        rect.w < 0.0f || rect.h < 0.0f) {
        record->bad_geometry = 1;
    }
}

static struct nk_rect
bbox_points(const struct nk_vec2i *points, int count)
{
    struct nk_rect rect;
    int i;
    float min_x, min_y, max_x, max_y;
    if (count <= 0)
        return nk_rect(0, 0, 0, 0);
    min_x = max_x = (float)points[0].x;
    min_y = max_y = (float)points[0].y;
    for (i = 1; i < count; ++i) {
        float x = (float)points[i].x;
        float y = (float)points[i].y;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    rect = nk_rect(min_x, min_y, max_x - min_x, max_y - min_y);
    return rect;
}

static void
capture_commands(struct nk_context *ctx, struct frame_record *record)
{
    const struct nk_command *command;
    for (command = nk__begin(ctx); command; command = nk__next(ctx, command)) {
        struct nk_color zero_color = {0, 0, 0, 0};
        switch (command->type) {
        case NK_COMMAND_SCISSOR: {
            const struct nk_command_scissor *cmd =
                (const struct nk_command_scissor*)command;
            struct nk_rect rect = nk_rect((float)cmd->x, (float)cmd->y,
                (float)cmd->w, (float)cmd->h);
            add_command(record, command->type, rect, zero_color, NULL);
            if (rect.x == -8192.0f && rect.y == -8192.0f &&
                rect.w == 16384.0f && rect.h == 16384.0f) {
                /* Nuklear's explicit "no clipping" sentinel. */
            } else if (rect.x < 0.0f || rect.y < 0.0f ||
                rect.x + rect.w > (float)VIEW_W + 0.5f ||
                rect.y + rect.h > (float)VIEW_H + 0.5f) {
                if (!record->bad_clip)
                    record->bad_clip_rect = rect;
                record->bad_clip = 1;
            }
            break;
        }
        case NK_COMMAND_LINE: {
            const struct nk_command_line *cmd =
                (const struct nk_command_line*)command;
            float x0 = (float)cmd->begin.x, y0 = (float)cmd->begin.y;
            float x1 = (float)cmd->end.x, y1 = (float)cmd->end.y;
            struct nk_rect rect = nk_rect(x0 < x1 ? x0 : x1,
                y0 < y1 ? y0 : y1, fabsf(x1 - x0), fabsf(y1 - y0));
            add_command(record, command->type, rect, cmd->color, NULL);
            hash_float(&record->command_hash, x0);
            hash_float(&record->command_hash, y0);
            hash_float(&record->command_hash, x1);
            hash_float(&record->command_hash, y1);
            hash_u32(&record->command_hash, cmd->line_thickness);
            break;
        }
        case NK_COMMAND_CURVE: {
            const struct nk_command_curve *cmd =
                (const struct nk_command_curve*)command;
            float xs[4] = {(float)cmd->begin.x, (float)cmd->end.x,
                (float)cmd->ctrl[0].x, (float)cmd->ctrl[1].x};
            float ys[4] = {(float)cmd->begin.y, (float)cmd->end.y,
                (float)cmd->ctrl[0].y, (float)cmd->ctrl[1].y};
            float minx = xs[0], miny = ys[0], maxx = xs[0], maxy = ys[0];
            int i;
            for (i = 1; i < 4; ++i) {
                if (xs[i] < minx) minx = xs[i];
                if (ys[i] < miny) miny = ys[i];
                if (xs[i] > maxx) maxx = xs[i];
                if (ys[i] > maxy) maxy = ys[i];
            }
            add_command(record, command->type,
                nk_rect(minx, miny, maxx - minx, maxy - miny),
                cmd->color, NULL);
            for (i = 0; i < 4; ++i) {
                hash_float(&record->command_hash, xs[i]);
                hash_float(&record->command_hash, ys[i]);
            }
            hash_u32(&record->command_hash, cmd->line_thickness);
            break;
        }
        case NK_COMMAND_RECT: {
            const struct nk_command_rect *cmd =
                (const struct nk_command_rect*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->color, NULL);
            hash_u32(&record->command_hash, cmd->rounding);
            hash_u32(&record->command_hash, cmd->line_thickness);
            break;
        }
        case NK_COMMAND_RECT_FILLED: {
            const struct nk_command_rect_filled *cmd =
                (const struct nk_command_rect_filled*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->color, NULL);
            hash_u32(&record->command_hash, cmd->rounding);
            break;
        }
        case NK_COMMAND_RECT_MULTI_COLOR: {
            const struct nk_command_rect_multi_color *cmd =
                (const struct nk_command_rect_multi_color*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->left, NULL);
            hash_color(&record->command_hash, cmd->top);
            hash_color(&record->command_hash, cmd->bottom);
            hash_color(&record->command_hash, cmd->right);
            break;
        }
        case NK_COMMAND_CIRCLE: {
            const struct nk_command_circle *cmd =
                (const struct nk_command_circle*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->color, NULL);
            hash_u32(&record->command_hash, cmd->line_thickness);
            break;
        }
        case NK_COMMAND_CIRCLE_FILLED: {
            const struct nk_command_circle_filled *cmd =
                (const struct nk_command_circle_filled*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->color, NULL);
            break;
        }
        case NK_COMMAND_ARC:
        case NK_COMMAND_ARC_FILLED: {
            float cx, cy, radius;
            struct nk_color color;
            if (command->type == NK_COMMAND_ARC) {
                const struct nk_command_arc *cmd =
                    (const struct nk_command_arc*)command;
                cx = (float)cmd->cx;
                cy = (float)cmd->cy;
                radius = (float)cmd->r;
                color = cmd->color;
                hash_u32(&record->command_hash, cmd->line_thickness);
                hash_float(&record->command_hash, cmd->a[0]);
                hash_float(&record->command_hash, cmd->a[1]);
            } else {
                const struct nk_command_arc_filled *cmd =
                    (const struct nk_command_arc_filled*)command;
                cx = (float)cmd->cx;
                cy = (float)cmd->cy;
                radius = (float)cmd->r;
                color = cmd->color;
                hash_float(&record->command_hash, cmd->a[0]);
                hash_float(&record->command_hash, cmd->a[1]);
            }
            add_command(record, command->type,
                nk_rect(cx - radius, cy - radius, radius * 2.0f,
                    radius * 2.0f), color, NULL);
            break;
        }
        case NK_COMMAND_TRIANGLE: {
            const struct nk_command_triangle *cmd =
                (const struct nk_command_triangle*)command;
            struct nk_vec2i points[3] = {cmd->a, cmd->b, cmd->c};
            add_command(record, command->type, bbox_points(points, 3),
                cmd->color, NULL);
            hash_u32(&record->command_hash, cmd->line_thickness);
            break;
        }
        case NK_COMMAND_TRIANGLE_FILLED: {
            const struct nk_command_triangle_filled *cmd =
                (const struct nk_command_triangle_filled*)command;
            struct nk_vec2i points[3] = {cmd->a, cmd->b, cmd->c};
            add_command(record, command->type, bbox_points(points, 3),
                cmd->color, NULL);
            break;
        }
        case NK_COMMAND_POLYGON:
        case NK_COMMAND_POLYGON_FILLED:
        case NK_COMMAND_POLYLINE: {
            int count = 0;
            int i;
            struct nk_vec2i *points = NULL;
            struct nk_color color = zero_color;
            if (command->type == NK_COMMAND_POLYGON) {
                const struct nk_command_polygon *cmd =
                    (const struct nk_command_polygon*)command;
                count = cmd->point_count;
                points = (struct nk_vec2i*)cmd->points;
                color = cmd->color;
                hash_u32(&record->command_hash, cmd->line_thickness);
            } else if (command->type == NK_COMMAND_POLYGON_FILLED) {
                const struct nk_command_polygon_filled *cmd =
                    (const struct nk_command_polygon_filled*)command;
                count = cmd->point_count;
                points = (struct nk_vec2i*)cmd->points;
                color = cmd->color;
            } else {
                const struct nk_command_polyline *cmd =
                    (const struct nk_command_polyline*)command;
                count = cmd->point_count;
                points = (struct nk_vec2i*)cmd->points;
                color = cmd->color;
                hash_u32(&record->command_hash, cmd->line_thickness);
            }
            add_command(record, command->type, bbox_points(points, count),
                color, NULL);
            hash_u32(&record->command_hash, (uint32_t)count);
            for (i = 0; i < count; ++i) {
                hash_u32(&record->command_hash, (uint32_t)points[i].x);
                hash_u32(&record->command_hash, (uint32_t)points[i].y);
            }
            break;
        }
        case NK_COMMAND_TEXT: {
            const struct nk_command_text *cmd =
                (const struct nk_command_text*)command;
            char text[40];
            int length = cmd->length;
            if (length < 0) length = 0;
            if (length > (int)sizeof(text) - 1) length = sizeof(text) - 1;
            memcpy(text, cmd->string, (size_t)length);
            text[length] = '\0';
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->foreground, text);
            hash_color(&record->command_hash, cmd->background);
            hash_float(&record->command_hash, cmd->height);
            hash_u32(&record->command_hash, (uint32_t)cmd->length);
            break;
        }
        case NK_COMMAND_IMAGE: {
            const struct nk_command_image *cmd =
                (const struct nk_command_image*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                cmd->col, NULL);
            break;
        }
        case NK_COMMAND_CUSTOM: {
            const struct nk_command_custom *cmd =
                (const struct nk_command_custom*)command;
            add_command(record, command->type,
                nk_rect((float)cmd->x, (float)cmd->y,
                    (float)cmd->w, (float)cmd->h),
                zero_color, NULL);
            break;
        }
        case NK_COMMAND_NOP:
        default:
            add_command(record, command->type, nk_rect(0, 0, 0, 0),
                zero_color, NULL);
            break;
        }
    }
}

/* =========================================================================
 * Rendering
 * ========================================================================= */

static void render_node(struct nk_context *ctx, struct render_state *state,
    int node_index);

static void
render_children(struct nk_context *ctx, struct render_state *state,
    int parent_index)
{
    int i;
    for (i = 0; i < state->sc->node_count; ++i) {
        if (state->sc->nodes[i].alive &&
            state->sc->nodes[i].parent == parent_index)
            render_node(ctx, state, i);
    }
}

static void
render_window(struct nk_context *ctx, struct render_state *state, int node_index)
{
    const struct scene_node *node = &state->sc->nodes[node_index];
    struct frame_record *record = state->record;
    struct nk_rect window_rect;
    struct nk_rect clip;
    nk_flags flags = (nk_flags)node->window_flags;
    int exists;
    int is_visible;
    int is_minimized;
    int visible;

    is_visible = node_visible(node, state->frame);
    is_minimized = node_minimized(node, state->frame);
    if (!is_visible)
        add_hidden(record, node->id, g_node_names[node->kind]);

    exists = nk_window_find(ctx, node->id) != NULL;
    if (exists) {
        nk_window_show(ctx, node->id,
            is_visible ? NK_SHOWN : NK_HIDDEN);
        nk_window_collapse(ctx, node->id,
            is_minimized ? NK_MINIMIZED : NK_MAXIMIZED);
    } else {
        if (!is_visible)
            flags |= NK_WINDOW_HIDDEN;
        if (is_minimized)
            flags |= NK_WINDOW_MINIMIZED;
    }
    push_palette(ctx, state, node->style_id + state->frame + state->sc->case_no);
    visible = nk_begin_titled(ctx, node->id, node->label, node->bounds, flags);
    if (visible) {
        window_rect = nk_window_get_bounds(ctx);
        clip = nk_window_get_content_region(ctx);
        add_layout(record, node->id, "window", window_rect, &clip);
        add_layout(record, node->id, "window_content", clip, &clip);
        add_active_container(record, node->id);
        ++record->visible_windows;
        render_children(ctx, state, node_index);
    }
    nk_end(ctx);
    pop_palette(ctx, state);
}

static void
render_popup(struct nk_context *ctx, struct render_state *state, int node_index)
{
    const struct scene_node *node = &state->sc->nodes[node_index];
    struct frame_record *record = state->record;
    struct nk_rect before_clip = nk_window_get_content_region(ctx);
    nk_flags flags = NK_WINDOW_BORDER;
    int active;

    if (!node_visible(node, state->frame)) {
        add_hidden(record, node->id, "popup");
        return;
    }
    if (node->clip_mode == 0)
        flags |= NK_WINDOW_NO_SCROLLBAR;
    else if (node->clip_mode == 2)
        flags |= NK_WINDOW_SCROLL_AUTO_HIDE;

    push_palette(ctx, state, node->style_id + state->frame + 1);
    active = nk_popup_begin(ctx, NK_POPUP_STATIC, node->id, flags,
        node->bounds);
    if (active) {
        struct nk_rect bounds = nk_window_get_bounds(ctx);
        struct nk_rect clip = nk_window_get_content_region(ctx);
        add_layout(record, node->id, "popup", bounds, &before_clip);
        add_layout(record, node->id, "popup_content", clip, &clip);
        add_active_container(record, node->id);
        render_children(ctx, state, node_index);
        nk_popup_end(ctx);
    } else {
        add_hidden(record, node->id, "popup_closed");
    }
    pop_palette(ctx, state);
}

static void
render_group(struct nk_context *ctx, struct render_state *state, int node_index)
{
    const struct scene_node *node = &state->sc->nodes[node_index];
    struct frame_record *record = state->record;
    struct nk_rect clip = nk_window_get_content_region(ctx);
    struct nk_rect slot;
    nk_flags flags = NK_WINDOW_BORDER;
    int active;

    if (!node_visible(node, state->frame)) {
        add_hidden(record, node->id, "group");
        return;
    }
    if (node->clip_mode == 0)
        flags |= NK_WINDOW_NO_SCROLLBAR;
    else if (node->clip_mode == 2)
        flags |= NK_WINDOW_SCROLL_AUTO_HIDE;

    push_palette(ctx, state, node->style_id + state->frame);
    nk_layout_row_dynamic(ctx, node->row_height, 1);
    slot = nk_widget_bounds(ctx);
    add_layout(record, node->id, "group_slot", slot, &clip);
    active = nk_group_begin_titled(ctx, node->id, node->label, flags);
    if (active) {
        struct nk_rect group_clip = nk_window_get_content_region(ctx);
        add_layout(record, node->id, "group_content", group_clip, &group_clip);
        add_active_container(record, node->id);
        render_children(ctx, state, node_index);
        nk_group_end(ctx);
    }
    pop_palette(ctx, state);
}

static void
render_control(struct nk_context *ctx, struct render_state *state, int node_index)
{
    const struct scene_node *node = &state->sc->nodes[node_index];
    struct frame_record *record = state->record;
    struct nk_rect clip = nk_window_get_content_region(ctx);
    struct nk_rect widget_rect;

    if (!node_visible(node, state->frame)) {
        add_hidden(record, node->id, g_node_names[node->kind]);
        return;
    }
    push_palette(ctx, state, node->style_id + state->frame);
    nk_layout_row_dynamic(ctx, node->row_height, 1);
    widget_rect = nk_widget_bounds(ctx);
    add_layout(record, node->id, g_node_names[node->kind], widget_rect, &clip);
    switch (node->kind) {
    case NODE_BUTTON:
        nk_button_label(ctx, node->label);
        break;
    case NODE_LABEL:
        nk_label(ctx, node->label, NK_TEXT_LEFT);
        break;
    case NODE_SLIDER:
        nk_slider_float(ctx, 0.0f, &state->ui.slider[node_index], 1.0f, 0.05f);
        break;
    case NODE_CHECKBOX:
        nk_checkbox_label(ctx, node->label, &state->ui.checkbox[node_index]);
        break;
    case NODE_PROGRESS:
        nk_progress(ctx, &state->ui.progress[node_index], 100, nk_false);
        break;
    default:
        break;
    }
    pop_palette(ctx, state);
}

static void
render_node(struct nk_context *ctx, struct render_state *state, int node_index)
{
    const struct scene_node *node = &state->sc->nodes[node_index];
    if (!node->alive)
        return;
    switch (node->kind) {
    case NODE_MENU_WINDOW:
    case NODE_INVENTORY_WINDOW:
        render_window(ctx, state, node_index);
        break;
    case NODE_POPUP:
        render_popup(ctx, state, node_index);
        break;
    case NODE_GROUP:
        render_group(ctx, state, node_index);
        break;
    default:
        if (is_control(node->kind))
            render_control(ctx, state, node_index);
        break;
    }
}

static void
init_ui_state(struct ui_state *ui, uint64_t seed)
{
    int i;
    memset(ui, 0, sizeof(*ui));
    for (i = 0; i < MAX_NODES; ++i) {
        uint64_t value = mix64(seed ^ (uint64_t)(i + 1));
        ui->slider[i] = 0.05f + (float)(value % 90) / 100.0f;
        ui->checkbox[i] = (int)(value & 1);
        ui->progress[i] = (nk_size)(value % 101);
    }
}

static void
render_frame(struct nk_context *ctx, const struct scenario *sc,
    struct frame_record *record, int frame, uint64_t seed)
{
    struct render_state state;
    struct style_probe probe_before;
    int i;
    memset(&state, 0, sizeof(state));
    memset(record, 0, sizeof(*record));
    state.sc = sc;
    state.record = record;
    state.frame = frame;
    init_ui_state(&state.ui, seed ^ (uint64_t)(frame * 131));
    record->frame = frame;
    probe_before = style_probe_capture(ctx);

    nk_input_begin(ctx);
    nk_input_motion(ctx, (int)((sc->seed + (uint64_t)frame * 97u) % VIEW_W),
        (int)((sc->seed >> 16) + (uint64_t)frame * 53u) % VIEW_H);
    nk_input_end(ctx);

    for (i = 0; i < sc->node_count; ++i) {
        if (sc->nodes[i].alive && sc->nodes[i].parent < 0)
            render_node(ctx, &state, i);
    }
    capture_commands(ctx, record);
    if (state.style_depth != 0 || !style_stacks_balanced(ctx) ||
        !style_probe_equal(probe_before, style_probe_capture(ctx)))
        record->bad_style = 1;
}

/* =========================================================================
 * Running and verification
 * ========================================================================= */

static int
record_equal(const struct frame_record *a, const struct frame_record *b)
{
    int i;
    if (a->command_count != b->command_count ||
        a->layout_count != b->layout_count ||
        a->hidden_count != b->hidden_count ||
        a->active_container_count != b->active_container_count ||
        a->visible_windows != b->visible_windows ||
        a->command_hash != b->command_hash ||
        a->layout_hash != b->layout_hash ||
        a->hidden_hash != b->hidden_hash ||
        a->active_hash != b->active_hash)
        return 0;
    for (i = 0; i < a->layout_count; ++i) {
        const struct layout_record *la = &a->layouts[i];
        const struct layout_record *lb = &b->layouts[i];
        if (strcmp(la->id, lb->id) != 0 || strcmp(la->kind, lb->kind) != 0 ||
            memcmp(&la->rect, &lb->rect, sizeof(la->rect)) != 0 ||
            la->has_clip != lb->has_clip ||
            (la->has_clip &&
                memcmp(&la->clip, &lb->clip, sizeof(la->clip)) != 0))
            return 0;
    }
    for (i = 0; i < a->hidden_count; ++i) {
        if (strcmp(a->hidden[i].id, b->hidden[i].id) != 0 ||
            strcmp(a->hidden[i].kind, b->hidden[i].kind) != 0)
            return 0;
    }
    for (i = 0; i < a->active_container_count; ++i) {
        if (strcmp(a->active_containers[i], b->active_containers[i]) != 0)
            return 0;
    }
    return 1;
}

static int
all_container_ancestors_active(const struct scenario *sc, int node_index,
    const struct frame_record *record)
{
    int current = sc->nodes[node_index].parent;
    while (current >= 0) {
        const struct scene_node *node = &sc->nodes[current];
        if (is_container(node->kind) &&
            !container_is_active(record, node->id))
            return 0;
        current = node->parent;
    }
    return 1;
}

static int
visible_controls_have_layouts(const struct scenario *sc,
    const struct frame_record *record, int frame, char *missing,
    size_t missing_cap)
{
    int i, j;
    for (i = 0; i < sc->node_count; ++i) {
        const struct scene_node *node = &sc->nodes[i];
        int found = 0;
        if (!node->alive || !is_control(node->kind) ||
            !node_effectively_visible(sc, i, frame) ||
            !all_container_ancestors_active(sc, i, record))
            continue;
        for (j = 0; j < record->layout_count; ++j) {
            if (strcmp(record->layouts[j].id, node->id) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
        {
            if (missing && missing_cap)
                copy_string(missing, missing_cap, node->id);
            return 0;
        }
    }
    return 1;
}

static int
hidden_controls_have_no_layout(const struct scenario *sc,
    const struct frame_record *record, int frame)
{
    int i, j;
    for (i = 0; i < sc->node_count; ++i) {
        const struct scene_node *node = &sc->nodes[i];
        if (!node->alive || !is_control(node->kind) ||
            node_effectively_visible(sc, i, frame))
            continue;
        for (j = 0; j < record->layout_count; ++j) {
            if (strcmp(record->layouts[j].id, node->id) == 0)
                return 0;
        }
    }
    return 1;
}

static int
verify_record(const struct scenario *sc, const struct frame_record *record,
    int frame, struct failure *failure)
{
    char missing[NAME_LEN];
    if (record->overflow) {
        failure->kind = FAIL_RECORD_OVERFLOW;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "frame record overflow: commands=%d layouts=%d hidden=%d",
            record->command_count, record->layout_count,
            record->hidden_count);
        return 0;
    }
    if (record->bad_style) {
        failure->kind = FAIL_STYLE_STACK;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "style stack did not return to its baseline");
        return 0;
    }
    if (record->bad_geometry) {
        failure->kind = FAIL_LAYOUT_GEOMETRY;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "invalid layout rectangle at frame %d", frame);
        return 0;
    }
    if (record->bad_clip) {
        failure->kind = FAIL_CLIP_GEOMETRY;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "scissor (%.1f,%.1f,%.1f,%.1f) escaped the %dx%d viewport "
            "at frame %d",
            record->bad_clip_rect.x, record->bad_clip_rect.y,
            record->bad_clip_rect.w, record->bad_clip_rect.h,
            VIEW_W, VIEW_H, frame);
        return 0;
    }
    if (record->visible_windows > 0 && record->command_count == 0) {
        failure->kind = FAIL_NO_COMMANDS;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "visible window produced no draw commands at frame %d", frame);
        return 0;
    }
    memset(missing, 0, sizeof(missing));
    if (!visible_controls_have_layouts(sc, record, frame, missing,
        sizeof(missing))) {
        int i;
        size_t offset = 0;
        char ids[128];
        ids[0] = '\0';
        for (i = 0; i < record->layout_count && offset + 20 < sizeof(ids);
            ++i) {
            int written = snprintf(ids + offset, sizeof(ids) - offset,
                "%s%s", i ? "," : "", record->layouts[i].id);
            if (written < 0) break;
            offset += (size_t)written;
        }
        failure->kind = FAIL_LAYOUT_GEOMETRY;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "visible control %s has no layout record at frame %d "
            "(windows=%d layouts=%d [%s])", missing, frame,
            record->visible_windows, record->layout_count, ids);
        return 0;
    }
    if (!hidden_controls_have_no_layout(sc, record, frame)) {
        failure->kind = FAIL_HIDDEN_DETERMINISM;
        failure->frame = frame;
        snprintf(failure->message, sizeof(failure->message),
            "a hidden control still received a layout slot at frame %d",
            frame);
        return 0;
    }
    return 1;
}

static void
accumulate_stats(const struct frame_record *record)
{
    int i;
    ++g_stats.frames;
    g_stats.commands += record->command_count;
    g_stats.layouts += record->layout_count;
    g_stats.hidden += record->hidden_count;
    if (g_stats.frames == 1 || record->command_count < g_stats.min_commands)
        g_stats.min_commands = record->command_count;
    if (record->command_count > g_stats.max_commands)
        g_stats.max_commands = record->command_count;
    if (g_stats.frames == 1 || record->layout_count < g_stats.min_layouts)
        g_stats.min_layouts = record->layout_count;
    if (record->layout_count > g_stats.max_layouts)
        g_stats.max_layouts = record->layout_count;
    for (i = 0; i < record->command_count; ++i) {
        int type = (int)record->commands[i].type;
        if (type >= 0 && type <= NK_COMMAND_CUSTOM)
            ++g_stats.command_type_counts[type];
    }
}

static void
json_rect(FILE *file, struct nk_rect rect)
{
    fprintf(file, "[%.3f,%.3f,%.3f,%.3f]",
        rect.x, rect.y, rect.w, rect.h);
}

static void
json_escape(FILE *file, const char *text)
{
    const unsigned char *iter = (const unsigned char*)text;
    fputc('"', file);
    while (*iter) {
        if (*iter == '"' || *iter == '\\') {
            fputc('\\', file);
            fputc(*iter, file);
        } else if (*iter < 32) {
            fprintf(file, "\\u%04x", *iter);
        } else {
            fputc(*iter, file);
        }
        ++iter;
    }
    fputc('"', file);
}

static void
emit_frame(FILE *file, int case_no, const struct frame_record *record)
{
    int i;
    fprintf(file, "{\"case\":%d,\"frame\":%d,\"command_hash\":\"%016llx\","
        "\"layout_hash\":\"%016llx\",\"visible_windows\":%d,\"commands\":[",
        case_no, record->frame,
        (unsigned long long)record->command_hash,
        (unsigned long long)record->layout_hash,
        record->visible_windows);
    for (i = 0; i < record->command_count; ++i) {
        const struct command_record *cmd = &record->commands[i];
        if (i) fputc(',', file);
        fputs("{\"type\":\"", file);
        fputs(g_command_names[(int)cmd->type], file);
        fputs("\",\"r\":", file);
        json_rect(file, cmd->rect);
        fprintf(file, ",\"color\":[%u,%u,%u,%u]",
            cmd->color.r, cmd->color.g, cmd->color.b, cmd->color.a);
        if (cmd->text[0]) {
            fputs(",\"text\":", file);
            json_escape(file, cmd->text);
        }
        fputc('}', file);
    }
    fputs("],\"layouts\":[", file);
    for (i = 0; i < record->layout_count; ++i) {
        const struct layout_record *layout = &record->layouts[i];
        if (i) fputc(',', file);
        fputc('{', file);
        fputs("\"id\":", file);
        json_escape(file, layout->id);
        fputs(",\"kind\":", file);
        json_escape(file, layout->kind);
        fputs(",\"r\":", file);
        json_rect(file, layout->rect);
        if (layout->has_clip) {
            fputs(",\"clip\":", file);
            json_rect(file, layout->clip);
        }
        fputc('}', file);
    }
    fputs("],\"hidden\":[", file);
    for (i = 0; i < record->hidden_count; ++i) {
        if (i) fputc(',', file);
        fputc('{', file);
        fputs("\"id\":", file);
        json_escape(file, record->hidden[i].id);
        fputs(",\"kind\":", file);
        json_escape(file, record->hidden[i].kind);
        fputc('}', file);
    }
    fputs("],\"active_containers\":[", file);
    for (i = 0; i < record->active_container_count; ++i) {
        if (i) fputc(',', file);
        json_escape(file, record->active_containers[i]);
    }
    fputs("]}\n", file);
}

struct run_result {
    int pass;
    struct failure failure;
};

static struct run_result
run_scenario(const struct scenario *sc, int frames, FILE *frames_file,
    int collect_stats)
{
    struct run_result result;
    struct nk_context *ctx_a;
    struct nk_context *ctx_b;
    struct allocator_state *alloc_a;
    struct allocator_state *alloc_b;
    struct nk_allocator allocator_a;
    struct nk_allocator allocator_b;
    struct frame_record *record_a;
    struct frame_record *record_b;
    int frame;

    memset(&result, 0, sizeof(result));
    result.pass = 1;
    if (frames > MAX_FRAMES)
        frames = MAX_FRAMES;

    ctx_a = (struct nk_context*)calloc(1, sizeof(*ctx_a));
    ctx_b = (struct nk_context*)calloc(1, sizeof(*ctx_b));
    alloc_a = (struct allocator_state*)calloc(1, sizeof(*alloc_a));
    alloc_b = (struct allocator_state*)calloc(1, sizeof(*alloc_b));
    record_a = (struct frame_record*)calloc(1, sizeof(*record_a));
    record_b = (struct frame_record*)calloc(1, sizeof(*record_b));
    if (!ctx_a || !ctx_b || !alloc_a || !alloc_b || !record_a || !record_b) {
        result.pass = 0;
        result.failure.kind = FAIL_ALLOCATOR;
        snprintf(result.failure.message, sizeof(result.failure.message),
            "host allocation failure while setting up test contexts");
        goto cleanup;
    }

    allocator_a.userdata.ptr = alloc_a;
    allocator_a.alloc = tracked_alloc;
    allocator_a.free = tracked_free;
    allocator_b.userdata.ptr = alloc_b;
    allocator_b.alloc = tracked_alloc;
    allocator_b.free = tracked_free;

    nk_init(ctx_a, &allocator_a, &g_font);
    nk_init(ctx_b, &allocator_b, &g_font);
    nk_style_from_table(ctx_a, NULL);
    nk_style_from_table(ctx_b, NULL);
    ctx_a->style.cursor_visible = 0;
    ctx_b->style.cursor_visible = 0;

    for (frame = 0; frame < frames; ++frame) {
        struct failure failure_a;
        struct failure failure_b;
        memset(&failure_a, 0, sizeof(failure_a));
        memset(&failure_b, 0, sizeof(failure_b));

        render_frame(ctx_a, sc, record_a, frame, sc->seed);
        render_frame(ctx_b, sc, record_b, frame, sc->seed);

        if (!verify_record(sc, record_a, frame, &failure_a)) {
            result.pass = 0;
            result.failure = failure_a;
            goto finish_contexts;
        }
        if (!verify_record(sc, record_b, frame, &failure_b)) {
            result.pass = 0;
            result.failure = failure_b;
            goto finish_contexts;
        }
        if (record_a->layout_count != record_b->layout_count ||
            record_a->layout_hash != record_b->layout_hash) {
            result.pass = 0;
            result.failure.kind = FAIL_LAYOUT_DETERMINISM;
            result.failure.frame = frame;
            snprintf(result.failure.message,
                sizeof(result.failure.message),
                "layout mismatch frame=%d count=%d/%d hash=%016llx/%016llx",
                frame, record_a->layout_count, record_b->layout_count,
                (unsigned long long)record_a->layout_hash,
                (unsigned long long)record_b->layout_hash);
            goto finish_contexts;
        }
        if (record_a->command_count != record_b->command_count ||
            record_a->command_hash != record_b->command_hash) {
            result.pass = 0;
            result.failure.kind = FAIL_COMMAND_DETERMINISM;
            result.failure.frame = frame;
            snprintf(result.failure.message,
                sizeof(result.failure.message),
                "command mismatch frame=%d count=%d/%d hash=%016llx/%016llx",
                frame, record_a->command_count, record_b->command_count,
                (unsigned long long)record_a->command_hash,
                (unsigned long long)record_b->command_hash);
            goto finish_contexts;
        }
        if (record_a->hidden_count != record_b->hidden_count ||
            record_a->hidden_hash != record_b->hidden_hash) {
            result.pass = 0;
            result.failure.kind = FAIL_HIDDEN_DETERMINISM;
            result.failure.frame = frame;
            snprintf(result.failure.message,
                sizeof(result.failure.message),
                "hidden-node mismatch frame=%d count=%d/%d hash=%016llx/%016llx",
                frame, record_a->hidden_count, record_b->hidden_count,
                (unsigned long long)record_a->hidden_hash,
                (unsigned long long)record_b->hidden_hash);
            goto finish_contexts;
        }
        if (!record_equal(record_a, record_b)) {
            result.pass = 0;
            result.failure.kind = FAIL_LAYOUT_DETERMINISM;
            result.failure.frame = frame;
            snprintf(result.failure.message,
                sizeof(result.failure.message),
                "detailed frame records differ at frame %d", frame);
            goto finish_contexts;
        }

        if (collect_stats)
            accumulate_stats(record_a);
        if (frames_file)
            emit_frame(frames_file, sc->case_no, record_a);

        nk_clear(ctx_a);
        nk_clear(ctx_b);
    }

finish_contexts:
    nk_free(ctx_a);
    nk_free(ctx_b);
    if (alloc_a->bad_ops || alloc_b->bad_ops) {
        if (result.pass) {
            result.pass = 0;
            result.failure.kind = FAIL_ALLOCATOR;
            snprintf(result.failure.message, sizeof(result.failure.message),
                "allocator saw %llu bad operations",
                (unsigned long long)(alloc_a->bad_ops + alloc_b->bad_ops));
        }
    }
    if (alloc_a->head || alloc_b->head) {
        if (result.pass) {
            result.pass = 0;
            result.failure.kind = FAIL_LEAK;
            snprintf(result.failure.message, sizeof(result.failure.message),
                "leak after nk_free: %zu bytes in %s allocator",
                alloc_a->live_bytes + alloc_b->live_bytes,
                alloc_a->live_bytes ? "first" : "second");
        }
        allocator_force_release(alloc_a);
        allocator_force_release(alloc_b);
    }

cleanup:
    free(record_a);
    free(record_b);
    free(alloc_a);
    free(alloc_b);
    free(ctx_a);
    free(ctx_b);
    return result;
}

/* =========================================================================
 * Counterexample minimization and printing
 * ========================================================================= */

static void
print_alive_tree(FILE *file, const struct scenario *sc, int parent, int depth)
{
    int i;
    for (i = 0; i < sc->node_count; ++i) {
        const struct scene_node *node = &sc->nodes[i];
        int d;
        if (!node->alive || node->parent != parent)
            continue;
        for (d = 0; d < depth; ++d)
            fputs("  ", file);
        fprintf(file, "%s id=%s label=\"%s\"",
            g_node_names[node->kind], node->id, node->label);
        if (is_window(node->kind)) {
            fprintf(file, " bounds=(%.1f,%.1f,%.1f,%.1f) flags=0x%x",
                node->bounds.x, node->bounds.y,
                node->bounds.w, node->bounds.h, node->window_flags);
        } else if (node->kind == NODE_POPUP) {
            fprintf(file, " bounds=(%.1f,%.1f,%.1f,%.1f)",
                node->bounds.x, node->bounds.y,
                node->bounds.w, node->bounds.h);
        } else {
            fprintf(file, " row_h=%.1f clip_mode=%d",
                node->row_height, node->clip_mode);
        }
        if (node->hidden_phase >= 0)
            fprintf(file, " hidden_phase=%d", node->hidden_phase);
        if (node->minimized_phase >= 0)
            fprintf(file, " minimized_phase=%d", node->minimized_phase);
        fprintf(file, " style=%d\n", node->style_id);
        print_alive_tree(file, sc, i, depth + 1);
    }
}

static void
print_counterexample(FILE *file, const struct scenario *sc,
    const struct failure *failure)
{
    int i;
    int indent = 0;
    fprintf(file, "case %d seed=0x%016llx failure=%d frame=%d: %s\n",
        sc->case_no, (unsigned long long)sc->seed, (int)failure->kind,
        failure->frame, failure->message);
    fprintf(file, "minimal window tree (%d nodes):\n", count_alive(sc));
    print_alive_tree(file, sc, -1, indent);
    for (i = 0; i < sc->node_count; ++i) {
        const struct scene_node *node = &sc->nodes[i];
        if (node->alive)
            fprintf(file, "  node[%d] %s parent=%d\n", i,
                node->id, node->parent);
    }
    fputc('\n', file);
}

static int
shrink_failure(struct scenario *sc, int frames, const struct failure *original)
{
    int pass;
    int changed;
    int i;
    for (pass = 0; pass < sc->node_count + 2; ++pass) {
        changed = 0;
        for (i = sc->node_count - 1; i >= 0; --i) {
            struct scenario candidate;
            struct run_result result;
            if (!sc->nodes[i].alive)
                continue;
            candidate = *sc;
            mark_subtree_dead(&candidate, i);
            result = run_scenario(&candidate, frames, NULL, 0);
            if (!result.pass && result.failure.kind == original->kind) {
                *sc = candidate;
                changed = 1;
                break;
            }
        }
        if (!changed)
            break;
    }
    return count_alive(sc);
}

/* =========================================================================
 * Reporting and main
 * ========================================================================= */

static void
write_summary(FILE *file, int cases_requested, int cases_run, int failures)
{
    int i;
    int first = 1;
    fprintf(file, "{\n");
    fprintf(file, "  \"cases_requested\": %d,\n", cases_requested);
    fprintf(file, "  \"cases_run\": %d,\n", cases_run);
    fprintf(file, "  \"frames\": %d,\n", g_stats.frames);
    fprintf(file, "  \"status\": \"%s\",\n", failures ? "fail" : "pass");
    fprintf(file, "  \"properties\": {\n");
    fprintf(file, "    \"crashes\": 0,\n");
    fprintf(file, "    \"leaks\": %d,\n",
        g_stats.failure_kind_counts[FAIL_LEAK]);
    fprintf(file, "    \"allocator_bad_ops\": %llu,\n",
        (unsigned long long)g_stats.bad_alloc_ops);
    fprintf(file, "    \"layout_determinism\": \"%s\",\n",
        g_stats.failure_kind_counts[FAIL_LAYOUT_DETERMINISM] ||
        g_stats.failure_kind_counts[FAIL_LAYOUT_GEOMETRY] ?
        "see counterexamples" : "pass");
    fprintf(file, "    \"command_determinism\": \"%s\",\n",
        g_stats.failure_kind_counts[FAIL_COMMAND_DETERMINISM] ?
        "see counterexamples" : "pass");
    fprintf(file, "    \"style_stack_balance\": \"%s\",\n",
        g_stats.failure_kind_counts[FAIL_STYLE_STACK] ?
        "see counterexamples" : "pass");
    fprintf(file, "    \"clipping\": \"%s\"\n",
        g_stats.failure_kind_counts[FAIL_CLIP_GEOMETRY] ?
        "see counterexamples" : "pass");
    fprintf(file, "  },\n");
    fprintf(file, "  \"totals\": {\n");
    fprintf(file, "    \"commands\": %d,\n", g_stats.commands);
    fprintf(file, "    \"layouts\": %d,\n", g_stats.layouts);
    fprintf(file, "    \"hidden_nodes\": %d,\n", g_stats.hidden);
    fprintf(file, "    \"commands_per_frame_min\": %d,\n",
        g_stats.min_commands);
    fprintf(file, "    \"commands_per_frame_max\": %d,\n",
        g_stats.max_commands);
    fprintf(file, "    \"layouts_per_frame_min\": %d,\n",
        g_stats.min_layouts);
    fprintf(file, "    \"layouts_per_frame_max\": %d\n",
        g_stats.max_layouts);
    fprintf(file, "  },\n");
    fprintf(file, "  \"allocator\": {\n");
    fprintf(file, "    \"alloc_calls\": %llu,\n",
        (unsigned long long)g_stats.alloc_calls);
    fprintf(file, "    \"free_calls\": %llu,\n",
        (unsigned long long)g_stats.free_calls);
    fprintf(file, "    \"realloc_calls\": %llu,\n",
        (unsigned long long)g_stats.realloc_calls);
    fprintf(file, "    \"peak_live_bytes\": %zu\n", g_stats.peak_live_bytes);
    fprintf(file, "  },\n");
    fprintf(file, "  \"command_types\": {");
    for (i = 0; i <= NK_COMMAND_CUSTOM; ++i) {
        if (!g_stats.command_type_counts[i])
            continue;
        if (!first) fputs(", ", file);
        first = 0;
        fprintf(file, "\"%s\": %d", g_command_names[i],
            g_stats.command_type_counts[i]);
    }
    fprintf(file, "},\n");
    fprintf(file, "  \"failure_kinds\": {");
    first = 1;
    for (i = FAIL_ALLOCATOR; i <= FAIL_NO_COMMANDS; ++i) {
        if (!g_stats.failure_kind_counts[i])
            continue;
        if (!first) fputs(", ", file);
        first = 0;
        fprintf(file, "\"%d\": %d", i, g_stats.failure_kind_counts[i]);
    }
    fprintf(file, "},\n");
    fprintf(file, "  \"counterexamples\": %d\n", failures);
    fprintf(file, "}\n");
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--cases N] [--frames N] [--seed HEX] [--case N]\n"
        "          [--frames-out PATH] [--summary-out PATH]\n"
        "          [--failures-out PATH] [--no-frames]\n",
        argv0);
}

static uint64_t
parse_u64(const char *text, uint64_t fallback)
{
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno || !end || *end)
        return fallback;
    return (uint64_t)value;
}

int
main(int argc, char **argv)
{
    int i;
    int case_arg = -1;
    int no_frames = 0;
    int cases_requested = 200;
    int frames = 12;
    int failures = 0;
    int cases_run = 0;
    uint64_t seed = UINT64_C(0x6d656e752d696e76);
    const char *frames_path = "artifacts/property_frames.ndjson";
    const char *summary_path = "artifacts/property_summary.json";
    const char *failures_path = "artifacts/counterexamples.txt";

    font_init();
    memset(&g_stats, 0, sizeof(g_stats));
    memset(&g_config, 0, sizeof(g_config));

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--cases") == 0 && i + 1 < argc) {
            cases_requested = (int)parse_u64(argv[++i], 200);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = (int)parse_u64(argv[++i], 12);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = parse_u64(argv[++i], seed);
        } else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            case_arg = (int)parse_u64(argv[++i], 0);
        } else if (strcmp(argv[i], "--frames-out") == 0 && i + 1 < argc) {
            frames_path = argv[++i];
        } else if (strcmp(argv[i], "--summary-out") == 0 && i + 1 < argc) {
            summary_path = argv[++i];
        } else if (strcmp(argv[i], "--failures-out") == 0 && i + 1 < argc) {
            failures_path = argv[++i];
        } else if (strcmp(argv[i], "--no-frames") == 0) {
            no_frames = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (cases_requested < 0) cases_requested = 0;
    if (frames < 1) frames = 1;
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;

    g_config.cases = cases_requested;
    g_config.frames = frames;
    g_config.seed = seed;
    if (!no_frames) {
        g_config.frames_file = fopen(frames_path, "w");
        if (!g_config.frames_file) {
            fprintf(stderr, "cannot open %s: %s\n", frames_path,
                strerror(errno));
            return 2;
        }
    }
    g_config.summary_file = fopen(summary_path, "w");
    g_config.failure_file = fopen(failures_path, "w");
    if (!g_config.summary_file || !g_config.failure_file) {
        fprintf(stderr, "cannot open report files: %s\n", strerror(errno));
        return 2;
    }

    if (case_arg >= 0) {
        struct scenario sc;
        struct run_result result;
        generate_scenario(&sc, case_arg, seed);
        result = run_scenario(&sc, frames, g_config.frames_file, 1);
        ++cases_run;
        if (!result.pass) {
            ++failures;
            ++g_stats.failure_kind_counts[result.failure.kind];
            shrink_failure(&sc, frames, &result.failure);
            print_counterexample(stdout, &sc, &result.failure);
            print_counterexample(g_config.failure_file, &sc, &result.failure);
        } else {
            printf("case %d seed=0x%016llx PASS frames=%d\n",
                case_arg, (unsigned long long)sc.seed, frames);
        }
    } else {
        for (i = 0; i < cases_requested; ++i) {
            struct scenario sc;
            struct run_result result;
            generate_scenario(&sc, i, seed);
            result = run_scenario(&sc, frames, g_config.frames_file, 1);
            ++cases_run;
            if (!result.pass) {
                ++failures;
                ++g_stats.failure_kind_counts[result.failure.kind];
                shrink_failure(&sc, frames, &result.failure);
                print_counterexample(stdout, &sc, &result.failure);
                print_counterexample(g_config.failure_file, &sc,
                    &result.failure);
            }
            if ((i + 1) % 25 == 0)
                printf("progress: %d/%d cases, %d failures\n",
                    i + 1, cases_requested, failures);
        }
    }

    if (failures == 0) {
        fputs("No counterexamples: all generated menu/inventory scenes "
            "passed every property.\n", g_config.failure_file);
    }
    write_summary(g_config.summary_file, cases_requested, cases_run, failures);
    if (g_config.frames_file)
        fclose(g_config.frames_file);
    fclose(g_config.summary_file);
    fclose(g_config.failure_file);

    printf("summary: cases=%d frames=%d commands=%d layouts=%d "
        "failures=%d alloc=%llu free=%llu peak=%zu bytes\n",
        cases_run, g_stats.frames, g_stats.commands, g_stats.layouts,
        failures, (unsigned long long)g_stats.alloc_calls,
        (unsigned long long)g_stats.free_calls, g_stats.peak_live_bytes);
    printf("artifacts: %s, %s, %s\n", no_frames ? "(frames disabled)" :
        frames_path, summary_path, failures_path);
    return failures ? 1 : 0;
}
