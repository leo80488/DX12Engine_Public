#include "Graphics/SkinnedMeshSubsystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshManager.h"
#include "ECS/ECS.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4X4;

void SkinnedMeshSubsystem::Init(IGraphicsDevice& gfx, MeshManager& meshMgr)
{
    m_poseBuffer.Init(gfx);
    m_vertRing.Init(gfx);

    // Register every frame's pos/nrm slots in the bindless table. Each frame
    // holds its own buffer so the GPU can read frame N-1 while the CPU writes
    // frame N. Use the PERSISTENT (engine-lifetime) register path: the vertex
    // ring buffers live for the entire app lifetime, so their bindless slot
    // index must survive every world reload. Without this, MeshDescriptorHeap::
    // OnWorldClear would invalidate the slot and per-frame UpdateMeshThisFrame
    // patches would point posBindlessIdx[] at whatever the new world's mesh
    // VB happened to land at — animated skinned meshes would then sample
    // Bistro's vertex bytes as skinned positions (the disappearing-character
    // bug).
    auto& descHeap = meshMgr.GetDescriptorHeap();
    for (uint32_t i = 0; i < SkinnedVertexRing::kFramesInFlight; ++i)
    {
        m_vertRing.BeginFrame(i);
        m_vertRing.posBindlessIdx[i] = descHeap.RegisterPersistentBuffer(m_vertRing.GetPosBuffer());
        m_vertRing.nrmBindlessIdx[i] = descHeap.RegisterPersistentBuffer(m_vertRing.GetNrmBuffer());
    }
    m_vertRing.BeginFrame(0);

    m_animSystem         = std::make_unique<AnimationSystem>(m_skeletonRegistry, m_clipLibrary, m_morphClipLibrary);
    m_ikSystem           = std::make_unique<IKSystem>(*m_animSystem, m_skeletonRegistry);
    m_footIKSystem       = std::make_unique<FootIKTargetSystem>(*m_animSystem, m_skeletonRegistry);
    m_characterStateSystem = std::make_unique<CharacterStateSystem>(m_skeletonRegistry, nullptr);
    m_chainPhysicsSystem = std::make_unique<ChainPhysicsSystem>(*m_animSystem, m_skeletonRegistry);
    m_localToWorldSystem = std::make_unique<LocalToWorldSystem>(*m_animSystem, m_skeletonRegistry);
    m_socketSystem       = std::make_unique<SocketSystem>(m_poseBuffer, m_skeletonRegistry);
    m_followSystem       = std::make_unique<FollowSystem>();
    m_skinMatrixSystem   = std::make_unique<SkinMatrixSystem>(m_skeletonRegistry);

    auto pass = std::make_unique<SkinningPass>();
    pass->SetBuffers(&m_poseBuffer, &m_vertRing);
    m_skinningPass = pass.get();
    m_skinningPass->Init(gfx);
    m_skinPassOwned = std::move(pass);

    m_initialised = true;
    LOG_SUCCESS("SkinnedMeshSubsystem: initialised");
}

void SkinnedMeshSubsystem::SetAnimationClipSystem(Resource::AnimationClipSystem* cs)
{
    if (m_characterStateSystem) m_characterStateSystem->SetClipSystem(cs);
}

void SkinnedMeshSubsystem::OnWorldClear()
{
    m_skinnedMeshDescCache.clear();
    m_prevPoseCache.clear();
    m_animVisibleSet.clear();
}

void SkinnedMeshSubsystem::OnEntityDestroyed(Entity e)
{
    m_skinnedMeshDescCache.erase(e);
    m_prevPoseCache.erase(e);
    m_animVisibleSet.erase(e);
}

void SkinnedMeshSubsystem::BuildSkinJobs(World& world, MeshManager& meshMgr)
{
    m_skinJobs.clear();

    std::unordered_map<Entity, uint32_t> prevPoseCopied;

    auto* pSkinOut     = world.GetPool<SkinningOutputComponent>();
    auto* pSkelRef     = world.GetPool<SkeletonRef>();
    auto* pSkelComp    = world.GetPool<SkeletonComponent>();
    auto* pMorphComp   = world.GetPool<MorphComponent>();
    auto* pAnimComp    = world.GetPool<AnimationComponent>();
    auto  pGet = [](auto* p, Entity e) { return p ? p->Get(e) : nullptr; };

    auto* pSkinnedMesh = world.GetPool<MeshSkinnedComponent>();
    if (!pSkinnedMesh) return;
    auto& skinEnts = pSkinnedMesh->Entities();
    auto& skinData = pSkinnedMesh->Data();
    const size_t skinN = skinData.size();
    for (size_t si = 0; si < skinN; ++si)
    {
        const Entity e = skinEnts[si];
        auto* mesh    = &skinData[si];
        auto* skinOut = pGet(pSkinOut, e);
        if (!skinOut) continue;
        if (mesh->vertexCount == 0) continue;
        if (!mesh->blendSRVHandle || !mesh->restPosSRVHandle || !mesh->restNrmSRVHandle) continue;
        if (skinOut->poseByteOffset == ~0u) continue;

        SkinDispatchDesc job{};
        job.restPosSRVHandle    = mesh->restPosSRVHandle;
        job.restNrmSRVHandle    = mesh->restNrmSRVHandle;
        job.blendSRVHandle      = mesh->blendSRVHandle;
        job.morphDeltaSRVHandle = mesh->morphDeltaSRVHandle;
        job.poseByteOffset      = skinOut->poseByteOffset;
        job.outPosByteOffset    = skinOut->outPosByteOffset;
        job.outNrmByteOffset    = skinOut->outNrmByteOffset;
        job.vertexCount         = mesh->vertexCount;
        job.morphCount          = mesh->morphTargetCount;
        std::memset(job.morphWeights, 0, sizeof(job.morphWeights));

        job.prevPoseByteOffset    = 0xFFFFFFFFu;
        job.outPrevPosByteOffset  = 0;
        {
            Entity skelEnt = e;
            const SkeletonRef* ref = pGet(pSkelRef, e);
            if (ref && ref->entity != NullEntity) skelEnt = ref->entity;

            auto cachedIt = prevPoseCopied.find(skelEnt);
            if (cachedIt != prevPoseCopied.end())
            {
                job.prevPoseByteOffset = cachedIt->second;
            }
            else
            {
                auto prevIt = m_prevPoseCache.find(skelEnt);
                if (prevIt != m_prevPoseCache.end() && prevIt->second.poseByteOffset != ~0u)
                {
                    uint32_t prevBoneCount = prevIt->second.boneCount;
                    const XMFLOAT4X4* prevMats =
                        m_poseBuffer.ReadMapped(prevIt->second.poseByteOffset);

                    if (prevMats && prevBoneCount > 0)
                    {
                        uint32_t prevSlot = m_poseBuffer.Alloc(prevBoneCount);
                        XMFLOAT4X4* dst = m_poseBuffer.MapSlice(prevSlot, prevBoneCount);
                        if (dst)
                        {
                            std::memcpy(dst, prevMats, prevBoneCount * sizeof(XMFLOAT4X4));
                            job.prevPoseByteOffset = prevSlot * 64u;
                            prevPoseCopied[skelEnt] = job.prevPoseByteOffset;
                        }
                    }
                }
            }

            job.outPrevPosByteOffset = m_vertRing.AllocPosition(mesh->vertexCount);
            skinOut->prevPosElementBase = job.outPrevPosByteOffset / 12u;

            const SkeletonComponent* skelComp = pGet(pSkelComp, skelEnt);
            m_prevPoseCache[skelEnt] = { skinOut->poseByteOffset,
                                         skelComp ? skelComp->boneCount : 0 };
        }

        if (mesh->morphTargetCount > 0)
        {
            Entity skelEnt = e;
            const SkeletonRef* ref = pGet(pSkelRef, e);
            if (ref && ref->entity != NullEntity) skelEnt = ref->entity;

            const SkeletonComponent* skelComp = pGet(pSkelComp, skelEnt);
            const MorphComponent*    morph    = pGet(pMorphComp, skelEnt);

            if (morph && morph->count > 0 && skelComp &&
                skelComp->assetIndex < m_skeletonRegistry.Count())
            {
                const SkeletonAsset& skelAsset = m_skeletonRegistry.Get(skelComp->assetIndex);

                const AnimationComponent* anim = pGet(pAnimComp, skelEnt);
                const MorphClipAsset* morphClip = nullptr;
                if (morph->primaryMorphClip != kInvalidAnimHandle &&
                    morph->primaryMorphClip < m_morphClipLibrary.Count())
                    morphClip = &m_morphClipLibrary.Get(morph->primaryMorphClip);

                if (morphClip)
                {
                    // Clip mode: weights[] is indexed by clip channel; remap to
                    // mesh target slots via channel-name ↔ target-name match.
                    std::unordered_map<std::string, uint32_t> clipNameMap;
                    clipNameMap.reserve(morphClip->morphCount);
                    for (uint32_t ci = 0; ci < morphClip->morphCount; ++ci)
                        clipNameMap[morphClip->morphNames[ci]] = ci;

                    for (uint32_t ti = 0; ti < skelAsset.morphTargetCount && ti < mesh->morphTargetCount; ++ti)
                    {
                        auto it = clipNameMap.find(skelAsset.morphTargetNames[ti]);
                        if (it != clipNameMap.end() && it->second < morph->count)
                            job.morphWeights[ti] = morph->weights[it->second];
                    }
                }
                else
                {
                    // Manual mode (editor / scripted): weights[] is already
                    // indexed by mesh morph-target slot; copy straight through.
                    const uint32_t n = (std::min)(mesh->morphTargetCount,
                                                  (std::min)(morph->count,
                                                             static_cast<uint32_t>(MorphComponent::MAX)));
                    for (uint32_t ti = 0; ti < n; ++ti)
                        job.morphWeights[ti] = morph->weights[ti];
                }
            }
        }

        m_skinJobs.push_back(job);

        if (mesh->meshDescriptorIdx != ~0u)
        {
            auto it = m_skinnedMeshDescCache.find(e);
            if (it == m_skinnedMeshDescCache.end())
                it = m_skinnedMeshDescCache.emplace(e, mesh->meshDescriptorIdx).first;
            const uint32_t descSlot = it->second;

            RHI::MeshDescriptor patchedDesc = mesh->baseMeshDesc;
            patchedDesc.position.bufferIndex = m_vertRing.GetCurrentPosBindlessIdx();
            patchedDesc.position.byteOffset  = skinOut->outPosByteOffset;
            patchedDesc.position.byteStride  = sizeof(float) * 3;
            patchedDesc.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
            patchedDesc.normal.bufferIndex   = m_vertRing.GetCurrentNrmBindlessIdx();
            patchedDesc.normal.byteOffset    = skinOut->outNrmByteOffset;
            patchedDesc.normal.byteStride    = sizeof(float) * 3;
            patchedDesc.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);
            patchedDesc.vertexCount          = mesh->vertexCount;

            // Per-frame patch: `position.bufferIndex` is `posBindlessIdx[currentFrameSlot]`,
            // which only stays valid for the current frame.  Use UpdateMeshThisFrame
            // so the write goes ONLY into the active GPU slot — without this, the
            // shared MeshDescriptor buffer was raced by the next frame's CPU write
            // while late passes (e.g. OutlinePass) were still reading it, causing
            // the inverted-hull outline to trail the GBuffer body during motion.
            meshMgr.GetDescriptorHeap().UpdateMeshThisFrame(descSlot, patchedDesc);
        }
    }

    if (m_skinningPass)
        m_skinningPass->SetJobs(&m_skinJobs);
}

void SkinnedMeshSubsystem::RegisterSkinnedMesh(IGraphicsDevice& gfx,
                                                World& world, Entity e,
                                                const XMFLOAT3* positions,
                                                const XMFLOAT3* normals,
                                                const BlendVertex* blendData,
                                                uint32_t vertexCount,
                                                uint32_t baseMeshDescIdx)
{
    if (!positions || !normals || !blendData || vertexCount == 0) return;
    (void)baseMeshDescIdx;

    MeshSkinnedComponent comp{};
    comp.vertexCount       = vertexCount;
    comp.meshDescriptorIdx = baseMeshDescIdx;
    // baseMeshDesc filled in by caller (needs MeshManager), patched below.

    const uint64_t posBufSize   = static_cast<uint64_t>(vertexCount) * sizeof(XMFLOAT3);
    const uint64_t nrmBufSize   = static_cast<uint64_t>(vertexCount) * sizeof(XMFLOAT3);
    const uint64_t blendBufSize = static_cast<uint64_t>(vertexCount) * sizeof(BlendVertex);

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = posBufSize;
        bd.stride     = sizeof(XMFLOAT3);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, comp.restPosBuffer, positions))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: restPosBuffer create failed (entity %u)", e);
            return;
        }
        comp.restPosSRVHandle = gfx.GetBufferSRVGpuHandle(comp.restPosBuffer);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = nrmBufSize;
        bd.stride     = sizeof(XMFLOAT3);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::NONE;
        if (!gfx.CreateBuffer(bd, comp.restNrmBuffer, normals))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: restNrmBuffer create failed (entity %u)", e);
            return;
        }
        comp.restNrmSRVHandle = gfx.GetBufferSRVGpuHandle(comp.restNrmBuffer);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = blendBufSize;
        bd.stride     = sizeof(BlendVertex);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, comp.blendBuffer, blendData))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMesh: blendBuffer create failed (entity %u)", e);
            return;
        }
        comp.blendSRVHandle = gfx.GetBufferSRVGpuHandle(comp.blendBuffer);
    }

    if (vertexCount > 0)
    {
        comp.aabbMin = comp.aabbMax = positions[0];
        for (uint32_t i = 1; i < vertexCount; ++i)
        {
            comp.aabbMin.x = std::min(comp.aabbMin.x, positions[i].x);
            comp.aabbMin.y = std::min(comp.aabbMin.y, positions[i].y);
            comp.aabbMin.z = std::min(comp.aabbMin.z, positions[i].z);
            comp.aabbMax.x = std::max(comp.aabbMax.x, positions[i].x);
            comp.aabbMax.y = std::max(comp.aabbMax.y, positions[i].y);
            comp.aabbMax.z = std::max(comp.aabbMax.z, positions[i].z);
        }
    }

    {
        std::unordered_set<uint16_t> boneSet;
        for (uint32_t vi = 0; vi < vertexCount; ++vi)
        {
            for (uint32_t bi = 0; bi < MAX_BONES_PER_VERTEX; ++bi)
            {
                if (blendData[vi].boneWeights[bi] > 0)
                    boneSet.insert(blendData[vi].boneIndices[bi]);
            }
        }
        comp.influenceBones.assign(boneSet.begin(), boneSet.end());
        std::sort(comp.influenceBones.begin(), comp.influenceBones.end());
    }

    world.AddComponent<MeshSkinnedComponent>(e, std::move(comp));
    world.AddComponent<SkinningOutputComponent>(e, SkinningOutputComponent{});

    LOG_INFO("SkinnedMeshSubsystem: registered skinned mesh for entity %u (%u verts)", e, vertexCount);
}

bool SkinnedMeshSubsystem::RegisterSkinnedMeshFull(IGraphicsDevice& gfx,
                                                    MeshManager&     meshMgr,
                                                    World&           world,
                                                    const SceneLoader::SkinnedMeshPending& pending)
{
    const uint32_t vertexCount = static_cast<uint32_t>(pending.restPositions.size());
    const uint32_t indexCount  = static_cast<uint32_t>(pending.indices.size());

    if (vertexCount == 0 || indexCount == 0)
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: empty mesh for entity %u", pending.entity);
        return false;
    }
    if (pending.blendData.empty())
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: no blend data for entity %u", pending.entity);
        return false;
    }

    {
        const uint32_t blendCount = static_cast<uint32_t>(pending.blendData.size());
        uint32_t zeroWeightVerts = 0;
        uint32_t lowSumVerts = 0;
        uint32_t highSumVerts = 0;
        uint32_t maxBoneIdx = 0;
        const uint32_t boneCount = (pending.skeletonIndex < m_skeletonRegistry.Count())
            ? m_skeletonRegistry.Get(pending.skeletonIndex).boneCount : 0;
        uint32_t oobVerts = 0;

        for (uint32_t vi = 0; vi < blendCount; ++vi)
        {
            const BlendVertex& bv = pending.blendData[vi];
            uint32_t wsum = bv.boneWeights[0] + bv.boneWeights[1]
                          + bv.boneWeights[2] + bv.boneWeights[3];
            if (wsum == 0) ++zeroWeightVerts;
            if (wsum < 240) ++lowSumVerts;
            if (wsum > 260) ++highSumVerts;
            for (int k = 0; k < 4; ++k)
            {
                if (bv.boneWeights[k] > 0)
                {
                    if (bv.boneIndices[k] > maxBoneIdx)
                        maxBoneIdx = bv.boneIndices[k];
                    if (boneCount > 0 && bv.boneIndices[k] >= boneCount)
                        ++oobVerts;
                }
            }
        }

        if (zeroWeightVerts > 0 || oobVerts > 0)
            LOG_ERROR("SkinnedMeshSubsystem: BLEND ISSUE entity %u — verts=%u blend=%u "
                      "zeroWeight=%u lowSum=%u highSum=%u maxBoneIdx=%u oob=%u boneCount=%u",
                      pending.entity, vertexCount, blendCount,
                      zeroWeightVerts, lowSumVerts, highSumVerts,
                      maxBoneIdx, oobVerts, boneCount);
        else
            LOG_INFO("SkinnedMeshSubsystem: blend OK entity %u — verts=%u blend=%u "
                     "maxBoneIdx=%u boneCount=%u",
                     pending.entity, vertexCount, blendCount,
                     maxBoneIdx, boneCount);
    }

    RHI::GPUBuffer uvBuffer, indexBuffer, tangentBuffer;
    uint32_t uvBindlessIdx      = RHI::kInvalidBufferIndex;
    uint32_t indexBindlessIdx   = RHI::kInvalidBufferIndex;
    uint32_t tangentBindlessIdx = RHI::kInvalidBufferIndex;

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 2;
        bd.stride     = sizeof(float) * 2;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::NONE;
        const void* initData = pending.uvs.empty() ? nullptr : pending.uvs.data();
        if (!gfx.CreateBuffer(bd, uvBuffer, initData))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: uvBuffer create failed (entity %u)", pending.entity);
            return false;
        }
        uvBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(uvBuffer);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::BUFFER_RAW;
        if (!gfx.CreateBuffer(bd, indexBuffer, pending.indices.data()))
        {
            LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: indexBuffer create failed (entity %u)", pending.entity);
            return false;
        }
        indexBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(indexBuffer);
    }
    if (!pending.tangents.empty())
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(vertexCount) * sizeof(float) * 4;
        bd.stride     = sizeof(float) * 4;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
        bd.misc_flags = RHI::ResourceMiscFlag::NONE;
        if (gfx.CreateBuffer(bd, tangentBuffer, pending.tangents.data()))
            tangentBindlessIdx = meshMgr.GetDescriptorHeap().RegisterBuffer(tangentBuffer);
    }

    RHI::MeshDescriptor baseDesc{};
    if (uvBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.uv0.bufferIndex = uvBindlessIdx;
        baseDesc.uv0.byteOffset  = 0;
        baseDesc.uv0.byteStride  = sizeof(float) * 2;
        baseDesc.uv0.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (tangentBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.tangent.bufferIndex = tangentBindlessIdx;
        baseDesc.tangent.byteOffset  = 0;
        baseDesc.tangent.byteStride  = sizeof(float) * 4;
        baseDesc.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }
    if (indexBindlessIdx != RHI::kInvalidBufferIndex)
    {
        baseDesc.indexBufferIndex = indexBindlessIdx;
        baseDesc.indexByteOffset  = 0;
        baseDesc.indexFormat      = 1;
    }
    baseDesc.vertexCount = vertexCount;

    const uint32_t baseMeshDescIdx = meshMgr.GetDescriptorHeap().RegisterMesh(baseDesc);
    if (baseMeshDescIdx == RHI::kInvalidBufferIndex)
    {
        LOG_ERROR("SkinnedMeshSubsystem::RegisterSkinnedMeshFull: MeshDescriptorHeap full (entity %u)", pending.entity);
        return false;
    }

    RegisterSkinnedMesh(gfx, world, pending.entity,
                        pending.restPositions.data(),
                        pending.restNormals.data(),
                        pending.blendData.data(),
                        vertexCount,
                        baseMeshDescIdx);

    MeshSkinnedComponent* comp = world.GetComponent<MeshSkinnedComponent>(pending.entity);
    if (comp)
    {
        comp->uvBuffer      = std::move(uvBuffer);
        comp->indexBuffer   = std::move(indexBuffer);
        comp->tangentBuffer = std::move(tangentBuffer);
        comp->indexCount    = indexCount;
        // Cache the base descriptor (UVs / index / tangent) so BuildSkinJobs
        // can override only position + normal streams each frame.
        comp->baseMeshDesc  = meshMgr.GetDescriptorHeap().GetMesh(baseMeshDescIdx);
    }

    if (pending.skeletonIndex != kInvalidSkeletonIndex)
    {
        const Entity rootEnt = (pending.rootEntity != NullEntity)
                                ? pending.rootEntity
                                : pending.entity;

        const SkeletonAsset& skel = m_skeletonRegistry.Get(pending.skeletonIndex);

        if (!world.HasComponent<SkeletonComponent>(rootEnt))
        {
            SkeletonComponent sc{};
            sc.assetIndex = pending.skeletonIndex;
            sc.boneCount  = skel.boneCount;
            world.AddComponent<SkeletonComponent>(rootEnt, sc);
            world.AddComponent<AnimationComponent>(rootEnt, AnimationComponent{});

            // Auto-attach a MorphComponent when the skeleton ships morph targets,
            // so the editor inspector can author weights manually without first
            // needing to drop a .ianim morph clip onto the character.
            if (skel.morphTargetCount > 0 && !world.HasComponent<MorphComponent>(rootEnt))
            {
                MorphComponent mc{};
                mc.primaryMorphClip = kInvalidAnimHandle; // manual mode
                mc.count = (std::min)(skel.morphTargetCount,
                                       static_cast<uint32_t>(MorphComponent::MAX));
                world.AddComponent<MorphComponent>(rootEnt, mc);
                LOG_INFO("SkinnedMeshSubsystem: MorphComponent auto-attached to root %u (%u targets)",
                         rootEnt, mc.count);
            }

            bool hasPhysBones = false;
            for (uint32_t b = 0; b < skel.boneCount && !hasPhysBones; ++b)
            {
                const char* nm = skel.boneNames[b];
                if (std::strstr(nm, "Hair") || std::strstr(nm, "hair") ||
                    std::strstr(nm, "PonyTail") || std::strstr(nm, "ponytail") ||
                    std::strstr(nm, "Skirt") || std::strstr(nm, "skirt") ||
                    std::strstr(nm, "Chest") || std::strstr(nm, "chest") ||
                    std::strstr(nm, "Butt")  || std::strstr(nm, "butt"))
                    hasPhysBones = true;
            }
            if (hasPhysBones)
            {
                world.AddComponent<ChainPhysicsComponent>(rootEnt, ChainPhysicsComponent{});
                LOG_INFO("SkinnedMeshSubsystem: ChainPhysicsComponent → root entity %u (auto-detected)", rootEnt);
            }

            LOG_INFO("SkinnedMeshSubsystem: SkeletonComponent + AnimationComponent → root entity %u", rootEnt);
        }

        if (pending.entity != rootEnt)
        {
            SkeletonRef ref{};
            ref.entity = rootEnt;
            world.AddComponent<SkeletonRef>(pending.entity, ref);
        }
    }

    if (pending.morphCount > 0 && !pending.morphDeltas.empty())
    {
        auto* msc = world.GetComponent<MeshSkinnedComponent>(pending.entity);
        if (msc)
        {
            const uint64_t deltaBufSize = static_cast<uint64_t>(pending.morphDeltas.size()) * sizeof(DirectX::XMFLOAT3);
            RHI::GPUBufferDesc bd{};
            bd.size       = deltaBufSize;
            bd.stride     = sizeof(DirectX::XMFLOAT3);
            bd.usage      = RHI::Usage::DEFAULT;
            bd.bind_flags = RHI::BindFlag::SHADER_RESOURCE;
            bd.misc_flags = RHI::ResourceMiscFlag::NONE;

            if (gfx.CreateBuffer(bd, msc->morphDeltaBuffer, pending.morphDeltas.data()))
            {
                msc->morphDeltaSRVHandle = gfx.GetBufferSRVGpuHandle(msc->morphDeltaBuffer);
                msc->morphTargetCount    = pending.morphCount;
                LOG_INFO("SkinnedMeshSubsystem: morph delta buffer entity %u — %u targets, %zu deltas",
                         pending.entity, pending.morphCount,
                         pending.morphDeltas.size());
            }
            else
                LOG_WARNING("SkinnedMeshSubsystem: failed to create morph delta buffer for entity %u", pending.entity);
        }
    }

    LOG_INFO("SkinnedMeshSubsystem: RegisterSkinnedMeshFull entity %u — %u verts, %u idx, skeleton %u, morphs %u",
             pending.entity, vertexCount, indexCount, pending.skeletonIndex, pending.morphCount);
    return true;
}
