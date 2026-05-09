#pragma once

// Backward-compatibility shim.
// New code should prefer:
//   "Graphics/IGraphicsDevice.h"  for the platform-agnostic abstract interface
//   "Graphics/GraphicsDX12.h"     for DX12-specific accessors (GetDevice, GetNativeCommandList, etc.)
//
// This header keeps existing code compiling by exposing both and providing the alias.

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"

// Alias: "Graphics" is the DX12 backend.  Existing code using "Graphics" continues to work.
using Graphics = GraphicsDX12;
