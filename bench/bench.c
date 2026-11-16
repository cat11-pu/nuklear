#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_IMPLEMENTATION
#include "nuklear.h"

#define CANVAS_W 1280
#define CANVAS_H 800
#define MAX_WINDOWS 256

#ifdef BENCH_FLOAT_COLOR
typedef float bench_color;
#define BENCH_COLOR_FORMAT NK_FORMAT_R32G32B32A32_FLOAT
#else
typedef unsigned char bench_color;
#define BENCH_COLOR_FORMAT NK_FORMAT_R8G8B8A8
#endif

struct vertex {
    float x, y;
    float u, v;
    bench_color r, g, b, a;
};

struct image {
    unsigned char *pixels;
    int width;
    int height;
};

static int slider_values[MAX_WINDOWS];
static size_t progress_values[MAX_WINDOWS];
static int check_values[MAX_WINDOWS];
static int stress_paths;

static double
now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static float
font_width(nk_handle handle, float height, const char *text, int len)
{
    NK_UNUSED(handle);
    NK_UNUSED(text);
    return (float)len * height * 0.55f;
}

static void
font_query(nk_handle handle, float height, struct nk_user_font_glyph *glyph,
    nk_rune codepoint, nk_rune next_codepoint)
{
    NK_UNUSED(handle);
    NK_UNUSED(codepoint);
    NK_UNUSED(next_codepoint);
    glyph->uv[0] = nk_vec2(0.0f, 0.0f);
    glyph->uv[1] = nk_vec2(0.55f, 0.85f);
    glyph->offset = nk_vec2(0.0f, 0.0f);
    glyph->width = height * 0.55f;
    glyph->height = height * 0.85f;
    glyph->xadvance = height * 0.55f;
}

static void
draw_stress_polygon(void *canvas, short x, short y, unsigned short w,
    unsigned short h, nk_handle userdata)
{
    struct nk_draw_list *list = (struct nk_draw_list *)canvas;
    struct nk_color color = nk_rgba(40, 120, 200, 180);
    const float cx = (float)x + (float)w * 0.5f;
    const float cy = (float)y + (float)h * 0.5f;
    const float rx = (float)w * 0.45f;
    const float ry = (float)h * 0.45f;
    int i;
    NK_UNUSED(userdata);
    for (i = 0; i < 130; ++i) {
        const float angle = (float)i * 6.2831853f / 130.0f;
        nk_draw_list_path_line_to(list,
            nk_vec2(cx + rx * nk_cos(angle), cy + ry * nk_sin(angle)));
    }
    nk_draw_list_path_fill(list, color);
}

static void
draw_ui(struct nk_context *ctx, int window_count, int collapsed_every)
{
    int i;
    for (i = 0; i < window_count; ++i) {
        char title[32];
        char row_label[32];
        nk_flags flags = NK_WINDOW_BORDER | NK_WINDOW_TITLE |
            NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_MINIMIZABLE;
        struct nk_rect bounds;
        int row;

        if (collapsed_every > 0 && (i % collapsed_every) == 0)
            flags |= NK_WINDOW_MINIMIZED;

        snprintf(title, sizeof(title), "Panel %03d", i);
        bounds = nk_rect((float)(20 + (i % 4) * 310),
            (float)(20 + ((i / 4) % 3) * 250), 290.0f, 230.0f);
        if (!nk_begin(ctx, title, bounds, flags))
            goto end_window;

        for (row = 0; row < 6; ++row) {
            snprintf(row_label, sizeof(row_label), "Metric %02d", row);
            nk_layout_row_dynamic(ctx, 22.0f, 2);
            nk_label(ctx, row_label, NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Apply"))
                slider_values[i] = (slider_values[i] + 1) % 17;
        }

        nk_layout_row_dynamic(ctx, 18.0f, 1);
        nk_progress(ctx, &progress_values[i], 100, NK_MODIFIABLE);
        nk_layout_row_dynamic(ctx, 24.0f, 1);
        nk_slider_int(ctx, 0, &slider_values[i], 16, 1);
        nk_layout_row_dynamic(ctx, 20.0f, 1);
        nk_checkbox_label(ctx, "Enabled", &check_values[i]);
        if (stress_paths && i == 1)
            nk_push_custom(nk_window_get_canvas(ctx),
                nk_rect(bounds.x + 20.0f, bounds.y + 190.0f,
                100.0f, 28.0f), draw_stress_polygon, nk_handle_id(i));

        end_window:
        nk_end(ctx);
    }
}

static void
prepare_frame(struct nk_context *ctx, int window_count, int collapsed_every)
{
    nk_clear(ctx);
    nk_input_begin(ctx);
    nk_input_motion(ctx, CANVAS_W + 100, CANVAS_H + 100);
    nk_input_end(ctx);
    draw_ui(ctx, window_count, collapsed_every);
}

static const struct nk_convert_config *
convert_config(void)
{
    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct vertex, x)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct vertex, u)},
        {NK_VERTEX_COLOR, BENCH_COLOR_FORMAT, NK_OFFSETOF(struct vertex, r)},
#ifdef BENCH_GENERIC_LAYOUT
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct vertex, u)},
#endif
        {NK_VERTEX_LAYOUT_END}
    };
    static struct nk_convert_config config;
    memset(&config, 0, sizeof(config));
    config.vertex_layout = layout;
    config.vertex_size = sizeof(struct vertex);
    config.vertex_alignment = NK_ALIGNOF(struct vertex);
    config.null.texture.id = 0;
    config.null.uv = nk_vec2(0.0f, 0.0f);
    config.circle_segment_count = 22;
    config.curve_segment_count = 16;
    config.arc_segment_count = 22;
    config.global_alpha = 1.0f;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;
    return &config;
}

static float
color_component(bench_color value)
{
#ifdef BENCH_FLOAT_COLOR
    return value;
#else
    return (float)value / 255.0f;
#endif
}

static float
edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static unsigned char
texture_sample(float u, float v)
{
    unsigned x = (unsigned)(u * 255.0f) & 255u;
    unsigned y = (unsigned)(v * 255.0f) & 255u;
    return (unsigned char)((x * 37u + y * 17u + ((x ^ y) * 13u)) & 255u);
}

static void
raster_triangle(struct image *img, const struct vertex *v0,
    const struct vertex *v1, const struct vertex *v2,
    struct nk_rect clip, int textured)
{
    float min_x_f = v0->x;
    float max_x_f = v0->x;
    float min_y_f = v0->y;
    float max_y_f = v0->y;
    float area;
    int min_x, max_x, min_y, max_y, x, y;

    if (v1->x < min_x_f) min_x_f = v1->x;
    if (v2->x < min_x_f) min_x_f = v2->x;
    if (v1->x > max_x_f) max_x_f = v1->x;
    if (v2->x > max_x_f) max_x_f = v2->x;
    if (v1->y < min_y_f) min_y_f = v1->y;
    if (v2->y < min_y_f) min_y_f = v2->y;
    if (v1->y > max_y_f) max_y_f = v1->y;
    if (v2->y > max_y_f) max_y_f = v2->y;

    min_x = (int)min_x_f;
    max_x = (int)max_x_f + 1;
    min_y = (int)min_y_f;
    max_y = (int)max_y_f + 1;
    if (min_x < (int)clip.x) min_x = (int)clip.x;
    if (min_y < (int)clip.y) min_y = (int)clip.y;
    if (max_x > (int)(clip.x + clip.w)) max_x = (int)(clip.x + clip.w);
    if (max_y > (int)(clip.y + clip.h)) max_y = (int)(clip.y + clip.h);
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > img->width) max_x = img->width;
    if (max_y > img->height) max_y = img->height;

    area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (area == 0.0f)
        return;

    for (y = min_y; y < max_y; ++y) {
        for (x = min_x; x < max_x; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = edge(v1->x, v1->y, v2->x, v2->y, px, py);
            float w1 = edge(v2->x, v2->y, v0->x, v0->y, px, py);
            float w2 = edge(v0->x, v0->y, v1->x, v1->y, px, py);
            float b0, b1, b2, alpha;
            float r, g, b;
            unsigned char *dst;

            if (area > 0.0f) {
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                    continue;
            } else {
                if (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f)
                    continue;
            }

            b0 = w0 / area;
            b1 = w1 / area;
            b2 = w2 / area;
            alpha = color_component(v0->a) * b0 +
                color_component(v1->a) * b1 + color_component(v2->a) * b2;
            if (alpha <= 0.0f)
                continue;

            r = color_component(v0->r) * b0 +
                color_component(v1->r) * b1 + color_component(v2->r) * b2;
            g = color_component(v0->g) * b0 +
                color_component(v1->g) * b1 + color_component(v2->g) * b2;
            b = color_component(v0->b) * b0 +
                color_component(v1->b) * b1 + color_component(v2->b) * b2;
            if (textured) {
                float u = v0->u * b0 + v1->u * b1 + v2->u * b2;
                float v = v0->v * b0 + v1->v * b1 + v2->v * b2;
                float tex = (float)texture_sample(u, v) / 255.0f;
                r *= tex;
                g *= tex;
                b *= tex;
            }

            dst = img->pixels + ((size_t)y * (size_t)img->width + (size_t)x) * 4u;
            dst[0] = (unsigned char)(r * alpha * 255.0f +
                (float)dst[0] * (1.0f - alpha) + 0.5f);
            dst[1] = (unsigned char)(g * alpha * 255.0f +
                (float)dst[1] * (1.0f - alpha) + 0.5f);
            dst[2] = (unsigned char)(b * alpha * 255.0f +
                (float)dst[2] * (1.0f - alpha) + 0.5f);
            dst[3] = 255;
        }
    }
}

static void
raster_image(struct nk_context *ctx, struct nk_buffer *cmds,
    struct nk_buffer *vertices, struct nk_buffer *elements, struct image *img)
{
    const struct nk_draw_command *cmd;
    const struct vertex *verts = (const struct vertex *)vertices->memory.ptr;
    const nk_draw_index *indices = (const nk_draw_index *)elements->memory.ptr;
    nk_size index_offset = 0;

    memset(img->pixels, 255, (size_t)img->width * (size_t)img->height * 4u);
    nk_draw_foreach(cmd, ctx, cmds) {
        struct nk_rect clip = cmd->clip_rect;
        nk_size i;
        int textured = cmd->texture.id == 1;
        if (clip.x < 0.0f) {
            clip.w += clip.x;
            clip.x = 0.0f;
        }
        if (clip.y < 0.0f) {
            clip.h += clip.y;
            clip.y = 0.0f;
        }
        if (clip.x + clip.w > (float)img->width)
            clip.w = (float)img->width - clip.x;
        if (clip.y + clip.h > (float)img->height)
            clip.h = (float)img->height - clip.y;
        if (clip.w <= 0.0f || clip.h <= 0.0f) {
            index_offset += cmd->elem_count;
            continue;
        }

        for (i = 0; i + 2 < cmd->elem_count; i += 3) {
            const struct vertex *v0 = &verts[indices[index_offset + i + 0]];
            const struct vertex *v1 = &verts[indices[index_offset + i + 1]];
            const struct vertex *v2 = &verts[indices[index_offset + i + 2]];
            raster_triangle(img, v0, v1, v2, clip, textured);
        }
        index_offset += cmd->elem_count;
    }
}

static unsigned long long
hash_image(const struct image *img)
{
    unsigned long long hash = 1469598103934665603ull;
    size_t i;
    size_t size = (size_t)img->width * (size_t)img->height * 4u;
    for (i = 0; i < size; ++i) {
        hash ^= img->pixels[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static int
dump_image(const struct image *img, const char *path)
{
    FILE *file = fopen(path, "wb");
    size_t size = (size_t)img->width * (size_t)img->height * 4u;
    if (!file)
        return 0;
    fwrite(img->pixels, 1, size, file);
    fclose(file);
    return 1;
}

int
main(int argc, char **argv)
{
    struct nk_context ctx;
    struct nk_user_font font;
    struct nk_buffer cmds, vertices, elements;
    struct image image;
    int window_count = 64;
    int collapsed_every = 3;
    int frames = 200;
    int warmup = 20;
    int i, frame, frame_commands = 0;
    double start, elapsed = 0.0, gen_elapsed = 0.0, convert_elapsed = 0.0;
    double raster_elapsed = 0.0;
    unsigned long long pixel_hash;
    const char *dump = 0;
    const struct nk_command *nk_cmd;

    if (argc > 1)
        window_count = atoi(argv[1]);
    if (argc > 2)
        collapsed_every = atoi(argv[2]);
    if (argc > 3)
        frames = atoi(argv[3]);
    if (argc > 4)
        dump = argv[4];
    if (argc > 5)
        stress_paths = atoi(argv[5]);
    if (window_count < 1 || window_count > MAX_WINDOWS || frames < 1)
        return 2;

    memset(&font, 0, sizeof(font));
    font.height = 13.0f;
    font.width = font_width;
    font.query = font_query;
    font.texture.id = 1;

    if (!nk_init_default(&ctx, &font))
        return 1;
    ctx.style.cursor_visible = nk_false;
    nk_buffer_init_default(&cmds);
    nk_buffer_init_default(&vertices);
    nk_buffer_init_default(&elements);
    image.width = CANVAS_W;
    image.height = CANVAS_H;
    image.pixels = (unsigned char *)malloc((size_t)CANVAS_W * CANVAS_H * 4u);
    if (!image.pixels)
        return 1;

    for (i = 0; i < MAX_WINDOWS; ++i) {
        slider_values[i] = i % 17;
        progress_values[i] = (size_t)(i * 7 % 101);
        check_values[i] = (i & 1) == 0;
    }

    for (frame = 0; frame < warmup; ++frame) {
        prepare_frame(&ctx, window_count, collapsed_every);
        nk_buffer_clear(&cmds);
        nk_buffer_clear(&vertices);
        nk_buffer_clear(&elements);
        nk_convert(&ctx, &cmds, &vertices, &elements, convert_config());
    }

    for (frame = 0; frame < frames; ++frame) {
        double frame_start = now_seconds();
        double convert_start;
        prepare_frame(&ctx, window_count, collapsed_every);
        convert_start = now_seconds();
        nk_buffer_clear(&cmds);
        nk_buffer_clear(&vertices);
        nk_buffer_clear(&elements);
        nk_convert(&ctx, &cmds, &vertices, &elements, convert_config());
        elapsed += now_seconds() - frame_start;
        gen_elapsed += convert_start - frame_start;
        convert_elapsed += now_seconds() - convert_start;
    }

    prepare_frame(&ctx, window_count, collapsed_every);
    nk_buffer_clear(&cmds);
    nk_buffer_clear(&vertices);
    nk_buffer_clear(&elements);
    nk_convert(&ctx, &cmds, &vertices, &elements, convert_config());
    nk_foreach(nk_cmd, &ctx) {
        NK_UNUSED(nk_cmd);
        frame_commands++;
    }
    raster_image(&ctx, &cmds, &vertices, &elements, &image);
    pixel_hash = hash_image(&image);

    start = now_seconds();
    raster_image(&ctx, &cmds, &vertices, &elements, &image);
    raster_elapsed = now_seconds() - start;

    printf("windows=%d collapsed_every=%d frames=%d\n",
        window_count, collapsed_every, frames);
    printf("avg_ms=%.3f gen_ms=%.3f convert_ms=%.3f commands=%d cmd_allocs=%llu convert_allocs=%llu draw_commands=%u vertices=%u elements=%u\n",
        elapsed / (double)frames * 1000.0,
        gen_elapsed / (double)frames * 1000.0,
        convert_elapsed / (double)frames * 1000.0,
        frame_commands, (unsigned long long)ctx.memory.calls,
        (unsigned long long)(cmds.calls + vertices.calls + elements.calls),
        ctx.draw_list.cmd_count,
        ctx.draw_list.vertex_count, ctx.draw_list.element_count);
    printf("pixels=%dx%d hash=%016llx raster_ms=%.3f\n",
        image.width, image.height, pixel_hash, raster_elapsed * 1000.0);
    if (dump && !dump_image(&image, dump))
        return 1;

    nk_buffer_free(&cmds);
    nk_buffer_free(&vertices);
    nk_buffer_free(&elements);
    nk_free(&ctx);
    free(image.pixels);
    return 0;
}
