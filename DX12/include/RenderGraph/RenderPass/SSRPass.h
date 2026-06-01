#pragma once

// SSRPass.h — backwards-compat umbrella.
//
// The five SSR sub-pass classes used to live here as one ~450-line file;
// Phase 4 of the SSR refactor split each into its own per-class header under
// include/Graphics/SSR/. New code should #include the specific header (or
// just Graphics/SSR/SSRSubsystem.h, which owns the whole pipeline). This stub
// stays so existing #include "RenderGraph/RenderPass/SSRPass.h" callers keep
// compiling unchanged.

#include "Graphics/SSR/SSRTracePass.h"
#include "Graphics/SSR/SSRResolvePass.h"
#include "Graphics/SSR/SSRTemporalPass.h"
#include "Graphics/SSR/SSRUpsamplePass.h"
#include "Graphics/SSR/SSRCompositePass.h"
