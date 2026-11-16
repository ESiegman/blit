#version 450
// blit: a picture (an image file's tab, a picture in the Markdown preview),
// from a colour texture of its own. The same full-screen triangle as the
// others, cut by the scissor to what of the picture shows. The texture's
// colours are already multiplied by their alpha, and so is what this
// gives: the pipeline adds it to what is there, which keeps 1 - alpha of it.

layout(location = 0) out vec4 out_color;

layout(set = 2, binding = 0) uniform sampler2D picture;

layout(std140, set = 3, binding = 0) uniform Image {
    vec4 rect;     // where all of the picture is: x0 y0 x1 y1, px
    vec4 fx;       // x corner radius (px), y opacity, z above 0: each of its pixels a sharp square
                   // (it is drawn much larger than it is), w the checkerboard's squares (px; 0: none)
    vec4 check_a;  // the checkerboard behind clear pixels: its two colours
    vec4 check_b;
};

float round_box(vec2 q, vec2 half_size, float r) {
    vec2 d = abs(q) - half_size + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
    vec2 size = rect.zw - rect.xy;
    vec2 at = gl_FragCoord.xy - rect.xy;
    vec2 p = at / size;
    vec4 c;
    if (fx.z > 0.0) {
        ivec2 n = textureSize(picture, 0);
        c = texelFetch(picture, clamp(ivec2(p * vec2(n)), ivec2(0), n - 1), 0);
    } else {
        c = texture(picture, p);
    }
    if (fx.w > 0.0) {
        ivec2 k = ivec2(floor(at / fx.w));
        c += (((k.x + k.y) & 1) == 0 ? check_a : check_b) * (1.0 - c.a);
    }
    float keep = fx.y;
    if (fx.x > 0.0) keep *= clamp(0.5 - round_box(at - size * 0.5, size * 0.5, fx.x), 0.0, 1.0);
    // (outside the picture: nothing, where the scissor's edge is a pixel past it)
    if (p.x < 0.0 || p.y < 0.0 || p.x > 1.0 || p.y > 1.0) keep = 0.0;
    out_color = c * keep;
}
