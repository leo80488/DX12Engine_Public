#pragma once

// Mp4FrameSource — FFmpeg-backed Video::IDecodedFrameSource.
//
// Builds on libavformat (demux) + libavcodec (decode) + libswscale (NV12
// conversion if the codec doesn't emit NV12 natively). The wrapper takes a
// VideoComponent each Tick(), advances the decoder until the next frame's
// PTS exceeds the component clock, then uploads the decoded NV12 bytes
// straight into VideoComponent::dpb[0] via IGraphicsDevice::UpdateTexture.
//
// Conditional compilation:
//   The implementation is enabled ONLY when WITH_FFMPEG is defined at
//   project level. Without that define, Video::OpenMp4 always returns
//   nullptr + logs an error explaining the missing dependency.
//
// To enable FFmpeg support:
//   1. Install vcpkg (https://github.com/microsoft/vcpkg).
//   2. Set VCPKG_ROOT environment variable to the install dir.
//   3. `vcpkg install ffmpeg[avcodec,avformat,swscale]:x64-windows`
//      (about 30 min first time; ~30 MB of DLLs end up in
//      $VCPKG_ROOT\installed\x64-windows\bin — copy or PATH them next to
//      Editor.exe at runtime).
//   4. Add `WITH_FFMPEG` to the PreprocessorDefinitions of EngineCore.vcxproj.
//   5. Add `avformat.lib;avcodec.lib;avutil.lib;swscale.lib;` to
//      AdditionalDependencies in the Linker section (vcpkg copies these
//      next to the include dir).
//   6. Rebuild.

#include "ECS/VideoComponent.h"

#include <memory>
#include <string>

class IGraphicsDevice;

namespace Video
{
    // Open an mp4 / mkv / mov file and return a decoded-frame source. Returns
    // nullptr when WITH_FFMPEG isn't defined, the file is missing, or the
    // first video stream couldn't be opened. On success the caller stashes
    // the result into VideoComponent::decodedFrameSource — VideoSystem
    // picks the decoded path automatically when that pointer is set.
    //
    // The source tries D3D12VA HARDWARE decode first (FFmpeg's
    // AV_HWDEVICE_TYPE_D3D12VA hwaccel piggy-backed onto the engine's
    // existing ID3D12Device). On any setup failure it transparently falls
    // back to software decode + libswscale NV12 conversion.
    std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& gfx,
                                                  const std::string& path);
} // namespace Video
