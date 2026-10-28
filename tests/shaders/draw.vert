#version 450
// A vertex shader in the shape the translated shaders have: attributes by location, and a uniform buffer in set 0 (NVN uniform slot 0 of the vertex stage).
layout(location = 0) in vec2 pos;
layout(location = 1) in vec4 col;
layout(set = 0, binding = 0) uniform V { vec4 offset; } v;     // x, y shift the triangle; z is its depth in NDC
layout(location = 0) out vec4 vcol;
void main() {
    gl_Position = vec4(pos + v.offset.xy, v.offset.z, 1.0);
    vcol = col;
}
