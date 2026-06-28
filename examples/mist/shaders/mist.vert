#version 330 core
layout(location = 0) in vec3 aPos;
uniform float uCamZ;      // camera on +z, looking toward -z
uniform float uFocal;     // perspective focal length
uniform float uPointBase; // base point size in device px (scaled by nearness)
uniform float uNearClip;  // cull birds nearer than this view depth
uniform float uFarFade;   // depth at which birds haze fully out
out float vFade;
void main() {
    float d = uCamZ - aPos.z; // view depth
    if (d < uNearClip) {      // flown over/behind the viewer: cull
        gl_Position  = vec4(2.0, 2.0, 2.0, 1.0); // offscreen
        gl_PointSize = 0.0;
        vFade        = 0.0;
        return;
    }
    float inv    = uFocal / d;
    gl_Position  = vec4(aPos.xy * inv, 0.0, 1.0);
    gl_PointSize = clamp(uPointBase * inv, 1.0, 26.0); // nearer => larger
    // Fade out only in the last sliver before the cull plane (so a bird that
    // sweeps overhead stays large and dark until it actually passes, no pop),
    // and out with distance (atmospheric haze).
    float nearFade = smoothstep(uNearClip, uNearClip + 0.06, d);
    float farFade  = 1.0 - smoothstep(uFarFade * 0.7, uFarFade, d);
    vFade          = nearFade * farFade;
}
