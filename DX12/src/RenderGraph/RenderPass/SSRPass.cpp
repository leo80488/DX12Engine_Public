// SSRPass.cpp — Phase 4b split: this file is intentionally empty.
//
// The five SSR sub-pass implementations now live under src/Graphics/SSR/
// (SSRTracePass.cpp, SSRResolvePass.cpp, SSRTemporalPass.cpp,
// SSRUpsamplePass.cpp, SSRCompositePass.cpp). The vcxproj no longer compiles
// this file — it lingers only so existing relative-path references resolve;
// `git rm` it once the next "no SSRPass.cpp in the build" sweep is verified.
