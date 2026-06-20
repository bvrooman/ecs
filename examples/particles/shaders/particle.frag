#version 330 core
in vec4 vColor;
out vec4 FragColor;
void main() {
    // Soft round sprite: radial falloff from the point center.
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;            // 0 at center .. 1 at edge
    float falloff = 1.0 - smoothstep(0.0, 1.0, r);

    float life = vColor.a;                // 1 when young .. 0 when old (age fade)
    // Ember cooling: pull green/blue down as the spark ages, so it reddens.
    vec3 col = vColor.rgb * vec3(1.0, mix(0.45, 1.0, life), mix(0.12, 1.0, life));
    // Incandescent white-hot core while the spark is young.
    col = mix(col, vec3(1.0), falloff * falloff * life);

    FragColor = vec4(col, life * falloff);
}
