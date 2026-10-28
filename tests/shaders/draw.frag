#version 450
layout(location = 0) in vec4 vcol;
layout(set = 0, binding = 1) uniform F { vec4 tint; } f;       // NVN uniform slot 0 of the fragment stage
layout(location = 0) out vec4 o;
void main() { o = vcol * f.tint; }
