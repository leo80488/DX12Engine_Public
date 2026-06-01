#pragma once

#include "Resource/ResourceHandle.h"
#include <cstdint>
#include <cassert>

// Asset Header pattern: every resource blob loaded from disk begins with a fixed
// AssetHeader, followed by type-specific Metadata, then the raw payload bytes.
//
// Memory layout:
//   [AssetHeader (24 bytes)] [XxxMetadata (variable)] [raw payload bytes]
//
// Resource Layer  : reads blob, validates magic, exposes Metadata pointer.
// Render Layer    : calls GetPayload(), passes raw bytes + metadata to DX12 API.
namespace Resource
{
    // Magic numbers (4-CC style, readable in hex dumps)
    constexpr uint32_t MAGIC_TEXTURE  = 'RTXT'; // 0x52545854
    constexpr uint32_t MAGIC_MESH     = 'RMSH'; // 0x524D5348
    constexpr uint32_t MAGIC_SHADER   = 'RSDR'; // 0x52534452
    constexpr uint32_t MAGIC_MATERIAL = 'RMAT'; // 0x524D4154
    constexpr uint32_t MAGIC_SCENE    = 'RSCN'; // 0x5253434E
    constexpr uint32_t MAGIC_PREFAB   = 'RPFB'; // 0x52504642
    constexpr uint32_t MAGIC_SKELETON   = 'RSKL'; // 0x52534B4C
    constexpr uint32_t MAGIC_ANIMATION  = 'RANI'; // 0x52414E49
    constexpr uint32_t MAGIC_MORPH      = 'RMRF'; // 0x524D5246
    constexpr uint32_t MAGIC_WORLD      = 'RWLD'; // 0x52574C44
    constexpr uint32_t MAGIC_MESHLIB    = 'RMLB'; // 0x524D4C42 — new mesh library (P1-P6 rewrite)
    constexpr uint32_t MAGIC_AUDIO      = 'RACL'; // 0x5241434C — .aclip audio sample

    constexpr uint16_t ASSET_VERSION = 1;

#pragma pack(push, 1)
    // Fixed 24-byte header at offset 0 of every asset blob.
    struct AssetHeader
    {
        uint32_t magic;         // identifies resource type (MAGIC_TEXTURE etc.)
        uint16_t version;       // format version for forward-compat checks
        uint16_t resourceType;  // ResourceType enum value
        uint32_t metadataSize;  // bytes occupied by the Metadata struct that follows
        uint32_t dataSize;      // bytes of raw payload that follows the Metadata
        uint32_t flags;         // reserved (compression / encryption flags)
        uint32_t reserved;      // padding — keep header 20 bytes aligned
    };
    static_assert(sizeof(AssetHeader) == 24, "AssetHeader size changed");

    // --- Per-type Metadata structs ---
    // These are stored immediately after AssetHeader in the blob.

    struct TextureMetadata
    {
        uint32_t width;
        uint32_t height;
        uint32_t depth;        // 1 for 2-D, >1 for 3-D / array
        uint16_t mipLevels;
        uint16_t format;       // maps to DXGI_FORMAT (cast on the Render Layer)
        uint8_t  dimension;    // 1=1D  2=2D  3=3D  6=Cube
        uint8_t  padding[3];
    };

    struct MeshMetadata
    {
        uint32_t vertexCount;
        uint32_t indexCount;
        uint32_t vertexStride; // bytes per vertex
        uint32_t indexStride;  // 2 (uint16) or 4 (uint32)
        uint32_t subMeshCount; // count of SubMeshRecord entries appended after
                               // the index buffer. 0 or 1 mean "one submesh
                               // covering the whole index buffer" (legacy).
        uint32_t padding;
    };

    // One sub-range within a merged mesh. The importer groups all aiMesh
    // primitives that belong to the same source scene node into ONE .imsh
    // file and records their per-material index ranges here. Runtime reads
    // the table and emits one MeshDescriptor slot per entry (all pointing
    // at the shared VB/IB, differing only by indexByteOffset / vertexCount).
    //
    // Layout at the tail of a .imsh payload (after vertex + index blobs):
    //   [vertexCount × vertexStride bytes] vertices
    //   [indexCount  × indexStride  bytes] indices
    //   [subMeshCount × sizeof(SubMeshRecord)] submesh table
    struct SubMeshRecord
    {
        uint32_t indexStart;    // first index (into shared IB) for this submesh
        uint32_t indexCount;    // number of indices
        uint32_t materialIndex; // per-node index into the scene's F-line material list
        uint32_t _pad;
    };
    static_assert(sizeof(SubMeshRecord) == 16, "SubMeshRecord must be 16 bytes");

    struct ShaderMetadata
    {
        uint32_t bytecodeSize;
        uint8_t  stage;          // maps to RHI::ShaderStage
        uint8_t  shaderModel;    // maps to RHI::ShaderModel
        uint8_t  padding[2];
        char     entryPoint[64];
    };

    // Payload: raw key=value text of the .mat source file (null-terminated).
    struct MaterialMetadata
    {
        uint32_t textLength;     // byte count of the text payload (excluding null terminator)
        uint32_t reserved[3];
    };

    // .ipfb — prefab: a serialized ECS entity tree (hierarchy + transforms + materials + mesh refs).
    // Payload: line-based text (null-terminated). Lines starting with 'N' = node, 'M' = material.
    struct PrefabMetadata
    {
        uint32_t nodeCount;   // total node entries
        uint32_t textLength;  // byte count of text payload (excluding null terminator)
        uint32_t reserved[2];
    };

    // .iskel — skeleton asset: bone hierarchy + bind poses + animation clips.
    // Payload: raw binary blob (SkeletonAsset POD + clip SOA data).
    //
    // If flags (reserved[1]) has ISKEL_FLAG_EXTENDED set, the payload includes
    // extended PMX data after the standard blend data:
    //   grantSource[boneCount], grantRatio[boneCount], transformOrder[boneCount],
    //   ikChainCount, followed by each IK chain's data.
    struct SkeletonMetadata
    {
        uint32_t boneCount;     // number of bones in the skeleton
        uint32_t clipCount;     // number of embedded ClipAssets
        uint32_t reserved[2];   // [0] = meshBlendCount, [1] = flags
    };
    constexpr uint32_t ISKEL_FLAG_EXTENDED = 0x01; // has grant/IK/transformOrder data

    // .ianim — standalone animation clips (named-channel, not bound to a skeleton).
    // Payload: binary clip data; see AnimationImporter / AnimationSerializer for layout.
    // Payload order: [bone clips × clipCount] [morph clips × morphClipCount]
    //                [notify section]   (only present when ANIM_FLAG_HAS_NOTIFIES set)
    //
    // The notify section is appended at the very tail so legacy loaders that
    // stop after morph clips ignore it, and old .ianim files (flag clear) keep
    // loading unchanged. Layout (see AnimationSerializer.cpp):
    //   [uint32 notifyClipCount]                       // == clipCount
    //   for each clip i: [uint32 jsonLen][jsonLen bytes]  // NotifyIO JSON string
    struct AnimationMetadata
    {
        uint32_t clipCount;       // number of bone AnimClipData entries
        uint32_t morphClipCount;  // number of MorphClipData entries (0 = no morph; backward-compat)
        uint32_t flags;           // see ANIM_FLAG_* below
        uint32_t reserved;
    };
    constexpr uint32_t ANIM_FLAG_POS_OFFSETS  = 0x1; // VMD: positions additive to rest pose
    constexpr uint32_t ANIM_FLAG_ROT_OFFSETS  = 0x2; // VMD: rotations compose with rest pose
    constexpr uint32_t ANIM_FLAG_HAS_NOTIFIES = 0x4; // tail notify section present

    // .imorph — per-mesh morph target deltas (vertex blend shapes).
    // One .imorph file per mesh that has morph targets.
    // Payload: morphTargetCount × { name[64], dense float3 deltas[vertexCount] }
    // The morph target order matches MorphClipAsset channel order by name.
    struct MorphTargetMetadata
    {
        uint32_t morphTargetCount; // number of morph targets in this file
        uint32_t vertexCount;      // vertices per mesh (each target has this many deltas)
        uint32_t meshFileIndex;    // index into .iscn F-lines (which .imsh this belongs to)
        uint32_t reserved;
    };

    // .iscn — scene hierarchy container.
    // Payload: line-based text (null-terminated) describing nodes and mesh file references.
    // See SceneImporter for format specification.
    struct SceneMetadata
    {
        uint32_t nodeCount;       // total node entries in payload
        uint32_t meshFileCount;   // total mesh file (F) entries in payload
        uint32_t textLength;      // byte count of text payload (excluding null terminator)
        uint32_t reserved;
    };

    // .meshlib — new mesh-library container. Replaces the legacy merged .imsh
    // model where submeshes were identified by (handle, indexStart, indexCount).
    // Now each mesh is a first-class entry with a stable meshId, so scene-graph
    // nodes reference them unambiguously and per-entity materials work cleanly.
    //
    // Layout at blob root:
    //   [AssetHeader]
    //   [MeshLibraryMetadata]
    //   [MeshLibraryEntry × meshCount]       — metadata table, indexed by meshId
    //   [vertexCount × vertexStride bytes]   — one shared pool for all meshes
    //   [indexCount × 4 bytes]               — uint32 indices, one shared pool
    //
    // meshId = position in the entry array (0..meshCount-1). Each entry carries
    // its own vertex/index range into the shared pools, its own AABB, and an
    // optional default material index (into the scene's material registry).
    // ----- MeshLibraryMetadata::flags bits ------------------------------------
    // bit 0: vertices include float4 tangent at byte offset 32 (xyz = tangent
    //        direction, w = bitangent handedness sign ±1). When set,
    //        vertexStride == 48 (12 pos + 12 normal + 8 uv + 16 tangent).
    //        When clear, vertexStride == 32 (legacy: pos + normal + uv only).
    constexpr uint32_t MESHLIB_FLAG_HAS_TANGENT = 1u << 0;

    struct MeshLibraryMetadata
    {
        uint32_t meshCount;       // count of MeshLibraryEntry entries
        uint32_t vertexCount;     // total vertices in the shared pool
        uint32_t indexCount;      // total indices in the shared pool
        uint16_t vertexStride;    // bytes/vertex. 32 = legacy (no tangent),
                                  //               48 = with float4 tangent (see flags).
        uint16_t indexStride;     // 4 (uint32 only for the new format)
        uint32_t flags;           // see MESHLIB_FLAG_* above. bit 0 = has tangent.
        uint32_t reserved;
    };

    // One logical mesh inside the library. Vertex range is OPTIONAL — when
    // vertexCount==0 (common in merged pools) the shader pulls positions by
    // dereferencing the indices against the library-wide VB, and each index
    // is absolute. When vertexCount>0 the renderer can draw this mesh with a
    // non-indexed path (no IB fetch) at offset vertexStart.
    // .aclip — audio clip (PCM today; codec field reserved for Vorbis/Opus).
    // Payload layout:
    //   [data bytes × dataSize]              raw PCM (or codec-specific) bytes
    //   [AudioCueRecord × cueCount]          optional cue / marker table
    //
    // Sample-rate / channel-count / bit-depth match XAudio2's WAVEFORMATEX so
    // the engine can SubmitSourceBuffer the data block directly.
    enum AudioCodec : uint32_t
    {
        AUDIO_CODEC_PCM    = 0,   // raw PCM, runtime-zero-copy
        AUDIO_CODEC_ADPCM  = 1,   // reserved
        AUDIO_CODEC_VORBIS = 2,   // reserved
        AUDIO_CODEC_OPUS   = 3,   // reserved
    };

    enum AudioLoadStrategy : uint32_t
    {
        AUDIO_LOAD_FULL       = 0,   // entire data block resident; v1 default
        AUDIO_LOAD_COMPRESSED = 1,   // reserved (decode-on-load to PCM)
        AUDIO_LOAD_STREAMING  = 2,   // reserved (streamer thread feeds buffers)
    };

    struct AudioClipMetadata
    {
        uint32_t codec;               // AudioCodec
        uint16_t channels;            // 1 / 2 / 6 / 8
        uint16_t bitsPerSample;       // 8 / 16 / 24 / 32
        uint32_t sampleRate;          // Hz
        uint32_t sampleCount;         // total frames (samples per channel)
        uint32_t blockAlign;          // bytes per frame (== channels * bitsPerSample/8 for PCM)

        float    duration;            // seconds
        float    peakAmplitude;       // 0..1 — for mixer normalization (0 if not measured)
        float    loudnessLUFS;        // integrated loudness; 0 if not measured

        uint32_t strategy;            // AudioLoadStrategy
        uint32_t loopStart;           // sample index; 0xFFFFFFFF = no loop
        uint32_t loopEnd;             // sample index; 0xFFFFFFFF = no loop

        uint32_t cueCount;            // count of AudioCueRecord entries appended after data
        uint32_t reserved;
    };
    static_assert(sizeof(AudioClipMetadata) == 52, "AudioClipMetadata layout drift");

    struct AudioCueRecord
    {
        uint32_t sampleOffset;        // sample-index marker
        char     name[32];            // e.g. "footstep_left", "beat_1"
        uint8_t  _pad[28];            // pad to 64 bytes for cheap row scanning
    };
    static_assert(sizeof(AudioCueRecord) == 64, "AudioCueRecord layout drift");

    struct MeshLibraryEntry
    {
        uint32_t vertexStart;        // first vertex index in shared VB
        uint32_t vertexCount;        // number of vertices this mesh owns
        uint32_t indexStart;         // first index into shared IB
        uint32_t indexCount;         // number of indices this mesh draws
        float    aabbMin[3];         // mesh-local AABB
        float    aabbMax[3];
        uint32_t defaultMaterialIdx; // index into scene's material registry
                                     // (kInvalidMaterial = 0xFFFFFFFF)
        uint32_t flags;              // reserved
    };
    static_assert(sizeof(MeshLibraryEntry) == 48, "MeshLibraryEntry must be 48 bytes");
#pragma pack(pop)

    // -------------------------------------------------------------------------
    // Accessor helpers — no copies, pure pointer arithmetic.
    // All functions require blob != nullptr and sufficient size.
    // -------------------------------------------------------------------------

    inline const AssetHeader* GetHeader(const uint8_t* blob)
    {
        assert(blob);
        return reinterpret_cast<const AssetHeader*>(blob);
    }

    // Cast the Metadata region to the concrete struct T.
    // Caller is responsible for matching T to the correct ResourceType.
    //
    // Example (Render Layer):
    //   const uint8_t* blob = rm.GetRawData(handle)->data();
    //   assert(GetHeader(blob)->magic == MAGIC_TEXTURE);
    //   const TextureMetadata* meta = GetMetadata<TextureMetadata>(blob);
    //   DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(meta->format);
    template<typename T>
    inline const T* GetMetadata(const uint8_t* blob)
    {
        assert(blob);
        return reinterpret_cast<const T*>(blob + sizeof(AssetHeader));
    }

    // Returns pointer to the raw payload (DDS bytes, vertex data, DXBC bytecode…).
    inline const uint8_t* GetPayload(const uint8_t* blob)
    {
        assert(blob);
        const AssetHeader* h = GetHeader(blob);
        return blob + sizeof(AssetHeader) + h->metadataSize;
    }

    // Validate magic and version before trusting the rest of the header.
    inline bool ValidateHeader(const uint8_t* blob, size_t blobSize, uint32_t expectedMagic)
    {
        if (!blob || blobSize < sizeof(AssetHeader))
            return false;
        const AssetHeader* h = GetHeader(blob);
        if (h->magic != expectedMagic)
            return false;
        if (h->version != ASSET_VERSION)
            return false;
        const size_t expectedTotal = sizeof(AssetHeader) + h->metadataSize + h->dataSize;
        return blobSize >= expectedTotal;
    }
}
