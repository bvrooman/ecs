#version 300 es
// Point-sprite vertex shader (GLES 3.0 / WebGL2). aColor.a carries "life"
// (1 young .. 0 old): it drives the sprite size and, in the fragment shader,
// the ember fade. Shared by the C++ and JS particle render hosts.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
    gl_PointSize = mix(2.0, 11.0, aColor.a); // bright sparks shrink as they die
}
