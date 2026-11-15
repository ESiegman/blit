#version 450
// blit: a popup (completion list, hover text, a tab bar) drawn over the text
// view. The same idea as grid.frag, one draw call per popup, cut to the
// popup's box by the scissor: a grid of cells of its own, read from the same
// buffer at cell_base, with a margin around it. The box may have an outline
// and rounded corners, cast a shadow (the scissor is then wider than the
// box, and the output is blended over what is there), and show runs of
// selected cells as rounded boxes of their own. Compiled twice: as a plain
// box with square corners (line numbers, a divider), and with BLIT_FX defined
// (shaders/build.sh) for everything else, so that the plain ones pay for
// none of it. Set/binding numbers follow SDL_GPU conventions:
// set 2 = sampled textures then storage buffers, set 3 = uniform buffers.

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D atlas;
layout(std430, set = 2, binding = 1) readonly buffer Cells { uint cells[]; };

layout(std140, set = 3, binding = 0) uniform Frame {
    vec4 bg;
    vec4 fg;
    vec4 sel;
    vec4 cur_color;
    vec4 thumb_color; // scroll thumb: rgb, and opacity in a
    vec4 cur_rect;    // x0 y0 x1 y1, px
    vec4 thumb_rect;  // x0 y0 x1 y1, px
    vec2 cell;        // cell size, px
    vec2 inv_cell;
    vec2 origin;      // grid position of pixel (0,0), px; >= 0 for the text layer
    vec2 inv_atlas;   // 1 / atlas size
    uint cols;
    uint rows;
    uint atlas_shift;  // log2(slots per atlas row)
    uint cell_base;    // where this layer's cells start in the buffer
    vec4 frame_rect;   // a popup's box: x0 y0 x1 y1, px
    vec4 border;       // its outline: rgb, and the width in px in a (0 = no outline)
    vec4 squiggle;     // the wavy underline: period, amplitude, thickness, middle above the cell bottom (px)
    vec4 guide;        // an indent line: its width in px in x
    vec4 tint_rect;    // x0 y0 x1 y1, px: washed with tint_color (where a dragged tab would land)
    vec4 tint_color;   // rgb, and how strongly in a
    vec4 radii;        // corner radii, px. Text: x of what is selected, y of the cursor, z of a search match,
                       // w of the tinted box. Popup: x of the box, y of a lit run of cells.
    vec4 shadow;       // a popup's shadow: x how far it reaches (px), y how dark, zw which way it falls (px)
    vec4 inset;        // a popup: xy how far inside its cells a lit run's rounded box is (px; y negative:
                       // outside), z the opacity of the whole popup, w how far a run at the grid's edge
                       // goes on into the margin
    vec4 fx;           // text: x how far it is dimmed toward the background, y above 0 while anything is
                       // selected, z pixels to a point
    vec4 palette[32];  // ink colours; a cell picks one with bits 16-20. 0 is plain text,
                       // 24-27 are the four kinds of diagnostic
    vec4 tones[8];     // a popup's other backgrounds for selected cells: a cell names one with bits 21-23 (0: none)
};

// How far a point is outside a box with rounded corners (negative: inside).
// q is measured from the middle of the box.
float round_box(vec2 q, vec2 half_size, float r) {
    vec2 d = abs(q) - half_size + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

// How dark a popup's shadow is at a point outside its box (q from the box's
// middle, d its distance from the box). Two shadows, as a lamp and a lit
// room give: a wide soft one that falls to one side, and a close one all
// round the edge.
float shade(vec2 q, vec2 half_size, float d) {
    if (shadow.y <= 0.0) return 0.0;
    float far = round_box(q - shadow.zw, half_size - shadow.x * 0.25, radii.x);
    float wide = 1.0 - smoothstep(-0.6 * shadow.x, 0.6 * shadow.x, far);
    float close = 1.0 - smoothstep(0.0, max(2.0, shadow.x * 0.12), d);
    return shadow.y * (1.0 - (1.0 - wide) * (1.0 - 0.45 * close));
}

// The cell at a grid position, or `outside` where the grid has none.
uint cell_at(ivec2 q, uint outside) {
    if (q.x < 0 || q.y < 0 || q.x >= int(cols) || q.y >= int(rows)) return outside;
    return cells[cell_base + uint(q.y) * cols + uint(q.x)];
}

void main() {
#ifndef BLIT_FX
    vec2 px = floor(gl_FragCoord.xy);
    vec2 p = px + origin;
    vec2 c = floor((p + 0.5) * inv_cell);
    vec2 in_cell = p - c * cell;

    // Pixels in the margin around the cells are blank.
    uint v = 0u;
    if (p.x >= 0.0 && p.y >= 0.0 && c.x < float(cols) && c.y < float(rows))
        v = cells[cell_base + uint(c.y) * cols + uint(c.x)];
    uint slot = v & 0xFFFFu;

    float a = 0.0;
    if (slot > 32u) {
        vec2 slot_xy = vec2(float(slot & ((1u << atlas_shift) - 1u)), float(slot >> atlas_shift));
        a = textureLod(atlas, (slot_xy * cell + in_cell + 0.5) * inv_atlas, 0.0).r;
    }

    vec3 back = (v & 0x80000000u) != 0u ? (((v >> 21) & 7u) != 0u ? tones[(v >> 21) & 7u].rgb : sel.rgb) : bg.rgb;
    vec3 col = mix(back, palette[(v >> 16) & 31u].rgb, a);

    // The outline.
    vec2 edge = min(px - frame_rect.xy, frame_rect.zw - 1.0 - px);
    if (min(edge.x, edge.y) < border.a) col = border.rgb;
    out_color = vec4(col, 1.0);
#else
    vec2 px = floor(gl_FragCoord.xy);
    // The box first: its rounded outline, and whether this pixel is inside
    // it at all. One that is not is in the box's shadow, and needs nothing
    // of the cells.
    bool boxed = radii.x > 0.0 || shadow.y > 0.0 || border.a > 0.0;
    vec2 half_size = (frame_rect.zw - frame_rect.xy) * 0.5;
    vec2 q = px + 0.5 - (frame_rect.xy + frame_rect.zw) * 0.5;
    float d = -1e4;
    if (boxed) {
        d = round_box(q, half_size, radii.x);
        if (d >= 0.5) {
            out_color = vec4(0.0, 0.0, 0.0, shade(q, half_size, d)) * inset.z;
            return;
        }
    }

    vec2 p = px + origin;
    vec2 size = vec2(float(cols), float(rows)) * cell;
    // The cell the pixel is in. One in the margin shows no glyph, but goes by
    // the nearest cell for backgrounds: a lit run's box may reach out there.
    vec2 c = clamp(floor((p + 0.5) * inv_cell), vec2(0.0), vec2(float(cols), float(rows)) - 1.0);
    vec2 in_cell = p - c * cell;
    bool in_grid = p.x >= 0.0 && p.y >= 0.0 && p.x < size.x && p.y < size.y;
    uint v = cells[cell_base + uint(c.y) * cols + uint(c.x)];
    uint slot = v & 0xFFFFu;

    float a = 0.0;
    if (in_grid && slot > 32u) {
        vec2 slot_xy = vec2(float(slot & ((1u << atlas_shift) - 1u)), float(slot >> atlas_shift));
        a = textureLod(atlas, (slot_xy * cell + in_cell + 0.5) * inv_atlas, 0.0).r;
    }

    // Bit 31: selected. Bit 24: under the mouse. Either lights the cell's
    // background: the cell itself, or (radii.y above 0) a rounded box that
    // runs through its lit neighbours on the row. Where such a run starts
    // and ends is marked in the cells (bits 25 and 26) by the CPU, so that no
    // pixel has to look at the cells beside its own.
    vec3 back = bg.rgb;
    uint lit = v & 0x81000000u;
    if (lit != 0u) {
        vec3 tone = (v & 0x80000000u) != 0u ? sel.rgb : cur_color.rgb;
        if ((v & 0x80000000u) != 0u && ((v >> 21) & 7u) != 0u) tone = tones[(v >> 21) & 7u].rgb;  // (one of the popup's other tones)
        float r = radii.y;
        if (r <= 0.0) {
            if (in_grid) back = tone;
        } else {
            ivec2 ci = ivec2(c);
            bool left = (v & 0x02000000u) == 0u, right = (v & 0x04000000u) == 0u;  // it goes on that way
            // (A run that reaches the first or the last column goes on into the margin by inset.w.)
            vec2 lo = vec2(left ? -1e4 : ci.x == 0 ? -inset.w : inset.x, inset.y);
            vec2 hi = vec2(right ? 1e4 : cell.x + (ci.x == int(cols) - 1 ? inset.w : -inset.x), cell.y - inset.y);
            vec2 at = in_cell + 0.5;
            vec2 beyond = max(max(lo + r - at, at - (hi - r)), 0.0);
            back = mix(back, tone, clamp(r - length(beyond) + 0.5, 0.0, 1.0));
        }
    }
    vec3 col = mix(back, palette[(v >> 16) & 31u].rgb, a);

    // The outline, and the box's own edge: a pixel the edge runs through is
    // partly the box and partly its shadow.
    float alpha = 1.0;
    if (boxed) {
        if (border.a > 0.0) col = mix(col, border.rgb, clamp(d + border.a + 0.5, 0.0, 1.0));
        float inside = clamp(0.5 - d, 0.0, 1.0);
        col *= inside;
        alpha = inside + (1.0 - inside) * shade(q, half_size, d);
    }
    // (Colour times alpha: the blend adds it to what is under, kept by 1 - alpha.)
    out_color = vec4(col, alpha) * inset.z;
#endif
}
