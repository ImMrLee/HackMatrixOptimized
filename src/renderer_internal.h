#pragma once

#include "screen.h"

// Shared by renderer.cpp (draw paths and constructor) and renderer_resources.cpp.
// Inline variables give both translation units the complete array type (so
// sizeof works) with a single definition. HEIGHT/appVertices are recomputed from
// the real viewport in the Renderer constructor.
inline float HEIGHT = SCREEN_HEIGHT / SCREEN_WIDTH / 2.0;
inline float appVertices[] = {
  -0.5f, -HEIGHT, 0, 0.0f, 0.0f, 0.5f,  -HEIGHT, 0, 1.0f, 0.0f,
  0.5f,  HEIGHT,  0, 1.0f, 1.0f, 0.5f,  HEIGHT,  0, 1.0f, 1.0f,
  -0.5f, HEIGHT,  0, 0.0f, 1.0f, -0.5f, -HEIGHT, 0, 0.0f, 0.0f,
};

inline float directRenderQuad[] = {
  -1, -1, 0, 0, 0, 1,  -1, 0, 1, 0, 1,  1,  0, 1, 1,
  1,  1,  0, 1, 1, -1, 1,  0, 0, 1, -1, -1, 0, 0, 0
};
