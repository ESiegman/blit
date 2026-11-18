#version 450
// blit: the whole text view in one draw call.
// Each pixel finds its cell, reads the glyph slot from the cell buffer and
// blends the atlas coverage. Compiled three times (shaders/build.sh): as it
// stands; with BLIT_FX defined, which rounds the corners of what is
// selected; and with BLIT_DIM as well, which dims the view. Each costs
// every pixel a little, so a view gets the build that does no more than it
// needs. (Popups are drawn over it by popup.frag, which
// shares the uniform block.) Set/binding numbers follow SDL_GPU conventions:
// set 2 = sampled textures then storage buffers, set 3 = uniform buffers.

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D atlas;
layout(std430, set = 2, binding = 1) readonly buffer Cells { uint cells[]; };
layout(set = 2, binding = 2) uniform sampler2D colours;  // colour glyphs (emoji): the atlas's last rows, RGBA

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
    vec4 tones[8];     // (a popup's)
    vec4 colour_inv;   // 1 / the colour atlas's size
    uvec4 colour_at;   // x the first colour glyph's slot, y its row in the atlas
};

// How far a point is outside a box with rounded corners (negative: inside).
// q is measured from the middle of the box.
float round_box(vec2 q, vec2 half_size, float r) {
    vec2 d = abs(q) - half_size + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

// The cell at a grid position, or `outside` where the grid has none.
uint cell_at(ivec2 q, uint outside) {
    if (q.x < 0 || q.y < 0 || q.x >= int(cols) || q.y >= int(rows)) return outside;
    return cells[cell_base + uint(q.y) * cols + uint(q.x)];
}

// A colour glyph's pixel (premultiplied), from the colour atlas.
vec4 colour_of(uint slot, vec2 in_cell) {
    vec2 xy = vec2(float(slot & ((1u << atlas_shift) - 1u)), float((slot >> atlas_shift) - colour_at.y));
    return textureLod(colours, (xy * cell + in_cell + 0.5) * colour_inv.xy, 0.0);
}

void main() {
    vec2 px = floor(gl_FragCoord.xy);
    vec2 p = px + origin;  // never negative: the CPU side keeps origin >= 0
    // +0.5 keeps the division clear of cell edges, so it is exact.
    vec2 c = floor((p + 0.5) * inv_cell);
    vec2 in_cell = p - c * cell;

    // The grid always covers the target; the clamp only guards the buffer.
    uint v = cells[cell_base + min(uint(c.y) * cols + uint(c.x), cols * rows - 1u)];
    uint slot = v & 0xFFFFu;

    // Slots up to 32 (space) are blank: most of the screen skips the atlas.
    float a = 0.0;
#ifdef BLIT_FX
    // A colour glyph (an emoji) from the colour atlas, in the build of the
    // shader a view gets when it has one on show (render.cpp): the plain
    // build does not pay for it.
    vec4 emoji = vec4(0.0);
    if (slot > 32u && slot >= colour_at.x) {
        emoji = colour_of(slot, in_cell);
    } else
#endif
    if (slot > 32u) {
        vec2 slot_xy = vec2(float(slot & ((1u << atlas_shift) - 1u)), float(slot >> atlas_shift));
        a = textureLod(atlas, (slot_xy * cell + in_cell + 0.5) * inv_atlas, 0.0).r;
    }

    bool lit = (v & 0x80000000u) != 0u;
    vec3 back = lit ? sel.rgb : bg.rgb;
#ifdef BLIT_FX
    // What is selected has rounded corners, outward ones and inward ones
    // (where a longer line meets a shorter). Which corners of which cells
    // has been worked out on the CPU (round_selection() in grid.cpp) and is
    // in bits 24-27 of the cell: top left, top right, bottom left, bottom
    // right. In a selected cell a marked corner is rounded off; in one that
    // is not, it is filled in. (Even unused this costs every pixel a
    // little, which is why it is in a variant of the shader that draws only
    // views with a selection in them.)
    uint corners = (v >> 24) & 15u;
    if (corners != 0u) {
        float r = radii.x;
        vec2 e = min(in_cell, cell - 1.0 - in_cell);  // pixels from the nearest edges of the cell
        uint which = (in_cell.x * 2.0 < cell.x ? 0u : 1u) + (in_cell.y * 2.0 < cell.y ? 0u : 2u);
        if (e.x < r && e.y < r && (corners & (1u << which)) != 0u) {
            float inside = clamp(r - length(r - e - 0.5) + 0.5, 0.0, 1.0);  // of the quarter circle in that corner
            back = lit ? mix(bg.rgb, sel.rgb, inside) : mix(sel.rgb, bg.rgb, inside);
        }
    }
#endif
    vec3 col = mix(back, palette[(v >> 16) & 31u].rgb, a);
#ifdef BLIT_FX
    col = col * (1.0 - emoji.a) + emoji.rgb;  // (an emoji: from the colour atlas, no draw of its own)
#endif

    // Bit 30 marks the few cells that need more than a glyph: those the
    // cursor or the scroll thumb touch, and those with a wavy underline.
    // Every other pixel skips all of this.
    if ((v & 0x40000000u) != 0u) {
        // Bits 21-23: an underline in the colour of a diagnostic. The wave
        // is a function of the position in the grid, so it runs unbroken
        // from cell to cell.
        // Kind 5 is not an underline: it is an indent line, a thin bar down
        // the left edge of the cell in the cell's own ink (the cell holds
        // white space, so its ink is not otherwise used).
        uint deco = (v >> 21) & 7u;
        if (deco == 5u) {
            if (in_cell.x < guide.x) col = palette[(v >> 16) & 31u].rgb;
        } else if (deco == 7u) {
            // Kind 7: another caret than the cursor, a bar down the left
            // edge of the cell in the cursor's colour.
            if (in_cell.x < max(2.0 * guide.x, 2.0)) col = cur_color.rgb;
        } else if (deco == 6u) {
            // Kind 6: a search match, on a background of its own, the ends
            // of the run rounded.
            float r = radii.z;
            ivec2 ci = ivec2(c);
            float dx = 0.0;
            if (in_cell.x < r && ((cell_at(ci + ivec2(-1, 0), v) >> 21) & 7u) != 6u) dx = r - in_cell.x - 0.5;
            else if (in_cell.x >= cell.x - r && ((cell_at(ci + ivec2(1, 0), v) >> 21) & 7u) != 6u) dx = in_cell.x + 0.5 - (cell.x - r);
            float dy = max(max(r - in_cell.y - 0.5, in_cell.y + 0.5 - (cell.y - r)), 0.0);
            float inside = clamp(r - length(vec2(dx, dy)) + 0.5, 0.0, 1.0);
            col = mix(mix(back, mix(back, tint_color.rgb, 0.4), inside), palette[(v >> 16) & 31u].rgb, a);
        } else if (deco != 0u) {
            float t = abs(fract(p.x / squiggle.x) - 0.5) * 4.0 - 1.0;  // a triangle wave, -1..1
            float mid = cell.y - squiggle.w + t * squiggle.y;
            float cover = clamp(squiggle.z * 0.5 + 0.5 - abs(in_cell.y + 0.5 - mid), 0.0, 1.0);
            col = mix(col, palette[23u + deco].rgb, cover);
        }
        vec2 at = px + 0.5;
        // Where something dragged would land: a rounded box, washed, with a rim.
        if (tint_rect.z > tint_rect.x) {
            vec2 half_size = (tint_rect.zw - tint_rect.xy) * 0.5 - 3.0 * fx.z;
            float d = round_box(at - (tint_rect.xy + tint_rect.zw) * 0.5, half_size, radii.w);
            float rim = clamp(d + 1.5 * fx.z + 0.5, 0.0, 1.0);
            col = mix(col, tint_color.rgb, clamp(0.5 - d, 0.0, 1.0) * mix(tint_color.a, 0.8, rim));
        }
        // The scroll thumb: a bar with round ends.
        if (thumb_rect.z > thumb_rect.x) {
            vec2 half_size = (thumb_rect.zw - thumb_rect.xy) * 0.5;
            float d = round_box(at - (thumb_rect.xy + thumb_rect.zw) * 0.5, half_size, min(half_size.x, half_size.y));
            col = mix(col, thumb_color.rgb, thumb_color.a * clamp(0.5 - d, 0.0, 1.0));
        }
        // The cursor: a bar, or (cur_color.a below 1) a block with the
        // character under it showing through in the background's colour.
        // Its edges are smooth, so it can stand between pixels on its way
        // from one place to the next.
        if (cur_rect.z > cur_rect.x) {
            vec2 half_size = (cur_rect.zw - cur_rect.xy) * 0.5;
            float d = round_box(at - (cur_rect.xy + cur_rect.zw) * 0.5, half_size, min(radii.y, min(half_size.x, half_size.y)));
            col = mix(col, cur_color.a < 0.75 ? mix(cur_color.rgb, bg.rgb, a) : cur_color.rgb, clamp(0.5 - d, 0.0, 1.0));
        }
    }
#ifdef BLIT_DIM
    // A split without the keyboard can stand back a little.
    col = mix(col, bg.rgb, fx.x);
#endif
    out_color = vec4(col, 1.0);
}
