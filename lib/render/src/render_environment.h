// What a renderer's creation, its destruction and its frames ask of the environments (pr/render_environment.h).

#pragma once

#include "pr/base.h"
#include "pr/status.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Makes what environments are made and read with: the black cube, the sampler and the pipelines that fill a cube's
// levels. Called when a renderer is created, once its shader libraries are made.
PrStatus renderEnvironmentObjectsCreate(Render* render);

// Frees what renderEnvironmentObjectsCreate made.
void renderEnvironmentObjectsDestroy(Render* render);

// Writes a scene's environment to the uniforms: its cube, its turn, its intensity and its diffuse light. A scene
// without an environment, or with one that has no light, gets the black cube. Called on the frame thread.
void renderEnvironmentUniforms(const Render* render, const PrRenderSceneEnvironment& environment,
    PrRenderFrameUniforms* uniforms);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
