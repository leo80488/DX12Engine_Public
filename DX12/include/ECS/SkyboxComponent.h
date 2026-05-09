#pragma once

#include <cstdint>
#include <string>
#include "Resource/SystemHandles.h"

// SkyboxComponent — attaches IBL environment maps to an entity.
// Set irradiancePath + radiancePath to .itex cubemap assets (DDS with 6-face array).
// The Renderer scans for one SkyboxComponent per frame and forwards the
// GPU handles to LightingPass (IBL) and SkyboxPass (visual skybox).
struct SkyboxComponent
{
    // Paths to converted cubemap assets (.itex or .dds cubemap, 6-face array).
    std::string irradiancePath;   // diffuse IBL irradiance cubemap
    std::string radiancePath;     // specular IBL prefiltered radiance cubemap
    std::string skyboxPath;       // unfiltered environment for the skybox visual

    uint32_t    radianceMipLevels = 7;    // mip count in the radiance map
    float       iblStrength       = 1.0f; // blend weight for IBL vs. flat ambient

    // Runtime GPU SRV handles — written by Renderer each frame, read-only elsewhere.
    uint64_t irradianceGpuHandle = 0;
    uint64_t radianceGpuHandle   = 0;
    uint64_t skyboxGpuHandle     = 0;
};
