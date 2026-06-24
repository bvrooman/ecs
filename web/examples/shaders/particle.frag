#version 300 es
// Point-sprite fragment shader (GLES 3.0 / WebGL2): a soft round spark with a
// white-hot young core and an ember cool-down as life (vColor.a) fades.
precision highp float;
in vec4 vColor;
out vec4 FragColor;
void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;            // 0 at center .. 1 at edge
    float falloff = 1.0 - smoothstep(0.0, 1.0, r);

    float life = vColor.a;                // 1 when young .. 0 when old (age fade)
    vec3 col = vColor.rgb * vec3(1.0, mix(0.45, 1.0, life), mix(0.12, 1.0, life));
    col = mix(col, vec3(1.0), falloff * falloff * life); // white-hot young core
    FragColor = vec4(col, life * falloff);
}
