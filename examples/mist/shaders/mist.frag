#version 330 core
in float vFade;
out vec4 FragColor;
// A tiny crisp black speck per bird. Standard over-blend, no depth test, so the
// flock composites like absorption: dense parts of the murmuration darken, thin
// edges stay pale -- the eerie density waves emerge on their own.
void main() {
    vec2 d        = gl_PointCoord - vec2(0.5);
    float r       = length(d) * 2.0;
    float falloff = 1.0 - smoothstep(0.45, 1.0, r); // small solid dot, soft rim
    FragColor     = vec4(0.0, 0.0, 0.0, falloff * vFade * 0.75);
}
