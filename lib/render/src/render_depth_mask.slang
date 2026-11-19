// lib/render's masked depth: the draws that write only a view's depth for the materials that cut their surface out
// where their alpha is low. The fragment shader computes the material's surface as every other pass does, and keeps the
// fragment if the surface's alpha reaches the material's cutoff. A shader set's source of the pass includes this file,
// with a vertex and a fragment shader for each layout of its material types' attributes, and the fragment shader of a
// layout draws each of the layout's types by the set's dispatch (render_shaders_check.cmake).

#include "render_shader_set.slang"
