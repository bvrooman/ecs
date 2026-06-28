#version 330 core
// Position is in framebuffer pixels, origin top-left; convert to clip space.
layout(location = 0) in vec2 aPixel;
uniform vec2 uFb; // framebuffer size in pixels
void main() {
    vec2 ndc = vec2(aPixel.x / uFb.x * 2.0 - 1.0, 1.0 - aPixel.y / uFb.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
