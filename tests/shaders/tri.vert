#version 450
#extension GL_EXT_buffer_reference : require
// Vertices come from a buffer addressed by its GPU address, as NVN's vertex buffers are.
layout(buffer_reference, std430) readonly buffer Verts { vec4 v[]; };     // two vec4 per vertex: position, color
layout(push_constant) uniform PC { Verts verts; } pc;
layout(location = 0) out vec4 vcol;
void main() {
    gl_Position = pc.verts.v[2 * gl_VertexIndex];
    vcol = pc.verts.v[2 * gl_VertexIndex + 1];
}
