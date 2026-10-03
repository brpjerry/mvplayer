#version 440

// Draws `source` clipped to an antialiased rounded rectangle. The texture can
// be zoomed and cropped inside the shape without the shape itself changing.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;      // item size in logical pixels
    float radius;   // corner radius in logical pixels
    vec2 uvScale;   // portion of the texture shown (for aspect-crop)
    float zoom;     // extra zoom around the centre
    float dim;      // 0..1 darkening
};

layout(binding = 1) uniform sampler2D source;

void main()
{
    vec2 p = (qt_TexCoord0 - 0.5) * size;
    vec2 q = abs(p) - (size * 0.5 - vec2(radius));
    float dist = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    float aa = max(fwidth(dist), 0.0001);
    float alpha = 1.0 - smoothstep(-aa, 0.0, dist);

    vec2 uv = (qt_TexCoord0 - 0.5) * uvScale / zoom + 0.5;
    vec4 c = texture(source, uv);
    c.rgb *= (1.0 - dim);
    fragColor = c * alpha * qt_Opacity;
}
