#pragma once

// RGTextureHandle and RGTextureDesc have been moved into GraphicsStruct.h
// so that RHI::CommandList can reference them without a circular include.
// This header is retained for backward compatibility.
#include "Graphics/GraphicsStruct.h"
