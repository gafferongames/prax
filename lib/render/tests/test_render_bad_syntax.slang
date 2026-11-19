// A source of a shader set that does not compile: it calls a function that nothing defines. The build of the tests'
// broken set of it reports the compiler's message (test_render_shaders.cmake).

#pragma once

#include "pr/render_material.slang"

float brokenValue() {
    return undefinedFunction(1.0);
}
