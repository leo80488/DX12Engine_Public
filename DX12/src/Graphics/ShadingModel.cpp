#include "Graphics/ShadingModel.h"

const char* ShadingModelName(ShadingModel m)
{
    switch (m)
    {
        case ShadingModel::Standard:    return "Standard";
        case ShadingModel::Unlit:       return "Unlit";
        case ShadingModel::ClearCoat:   return "Clear Coat";
        case ShadingModel::Subsurface:  return "Subsurface";
        case ShadingModel::Anisotropic: return "Anisotropic";
        default:                        return "Unknown";
    }
}
