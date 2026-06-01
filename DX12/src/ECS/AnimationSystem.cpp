#include "ECS/AnimationSystem.h"
#include "System/TaskSystem.h"
#include "System/Log.h"

#include <cmath>
#include <algorithm>
#include <cassert>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t };
}

static XMFLOAT4 Slerp4(const XMFLOAT4& a, const XMFLOAT4& b, float t)
{
    XMVECTOR qa = XMLoadFloat4(&a);
    XMVECTOR qb = XMLoadFloat4(&b);
    XMVECTOR qr = XMQuaternionSlerp(qa, qb, t);
    XMFLOAT4 out;
    XMStoreFloat4(&out, qr);
    return out;
}

// Build a TRS matrix from a LocalPose (row-major, matching DirectX convention)
static XMFLOAT4X4 LocalPoseToMatrix(const AnimationSystem::LocalPose& p)
{
    XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
    XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
    XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, S * R * T);
    return out;
}

// Multiply two row-major XMFLOAT4X4 matrices (parent × local)
static XMFLOAT4X4 Mul4x4(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
{
    XMFLOAT4X4 out;
    XMStoreFloat4x4(&out, XMLoadFloat4x4(&a) * XMLoadFloat4x4(&b));
    return out;
}

// ===========================================================================
// AnimationSystem
// ===========================================================================
void AnimationSystem::SampleClip(const ClipAsset& clip, float time,
                                  uint32_t boneCount, LocalPose* out) const
{
    if (clip.frameCount == 0 || clip.frameRate <= 0.f) return;

    const float rawFrame  = time * clip.frameRate;
    const uint32_t f0     = static_cast<uint32_t>(rawFrame) % clip.frameCount;
    const uint32_t f1     = (f0 + 1) % clip.frameCount;
    const float    alpha  = rawFrame - std::floorf(rawFrame);

    const uint32_t n = std::min(boneCount, clip.boneCount);
    for (uint32_t b = 0; b < n; ++b)
    {
        const uint32_t i0 = b * clip.frameCount + f0;
        const uint32_t i1 = b * clip.frameCount + f1;
        out[b].pos = Lerp3(clip.positions[i0], clip.positions[i1], alpha);
        out[b].rot = Slerp4(clip.rotations[i0], clip.rotations[i1], alpha);
        out[b].scl = Lerp3(clip.scales[i0],     clip.scales[i1],     alpha);
    }
}

void AnimationSystem::EnsureCacheSize(Entity e)
{
    if (e >= m_localPoseCache.size())
    {
        m_localPoseCache.resize(static_cast<size_t>(e) + 1);
        m_lastClipCache.resize(static_cast<size_t>(e) + 1, ~0u);
    }
}

void AnimationSystem::Update(World& world, float dt,
                              const std::unordered_set<Entity>* activeSet)
{
    // Phase 1: collect animated entities + advance time (single-threaded due to
    // morph sampling + time advance writing to AnimationComponent).
    struct AnimJob {
        Entity e;
        AnimationComponent* anim;
        SkeletonComponent* skel;
    };
    std::vector<AnimJob> animJobs;

    // Iterate the AnimationComponent pool directly — animated entities are a
    // tiny fraction of the world. Walking world.GetEntities() previously did
    // 22k GetComponent<AnimationComponent>+<Skeleton> hash lookups per frame
    // just to find the handful of animated skeletons.
    auto* pAnim = world.GetPool<AnimationComponent>();
    auto* pSkelA = world.GetPool<SkeletonComponent>();
    const size_t animPoolN = pAnim ? pAnim->Data().size() : 0;
    const auto&  animEnts  = pAnim ? pAnim->Entities() : std::vector<Entity>{};
    for (size_t ai = 0; ai < animPoolN; ++ai)
    {
        const Entity e = animEnts[ai];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        auto* anim = &pAnim->Data()[ai];
        auto* skel = pSkelA ? pSkelA->Get(e) : nullptr;
        if (!skel || skel->assetIndex == kInvalidAnimHandle) continue;
        if (anim->primaryClip == kInvalidAnimHandle) continue;

        const ClipAsset& clip = m_clips.Get(anim->primaryClip);
        if (clip.frameCount == 0) continue;

        // Advance time (writes to component — must be single-threaded).
        if (!anim->paused)
            anim->primaryTime += dt * anim->speed;
        if (anim->looping)
            anim->primaryTime = std::fmod(anim->primaryTime, clip.duration);
        else
            anim->primaryTime = std::min(anim->primaryTime, clip.duration);

        EnsureCacheSize(e);

        // Init rest pose if needed (must be serial — may resize the inner vector).
        std::vector<LocalPose>& poseVec = m_localPoseCache[e];
        uint32_t& lastClip = m_lastClipCache[e];
        const bool needsInit = (poseVec.size() != skel->boneCount)
                             || (lastClip != anim->primaryClip);
        poseVec.resize(skel->boneCount);
        if (needsInit)
        {
            const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);
            for (uint32_t b = 0; b < skel->boneCount; ++b)
            {
                XMVECTOR sc, rot, tr;
                XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skelAsset.restPoseLocal[b]));
                XMStoreFloat3(&poseVec[b].pos, tr);
                XMStoreFloat4(&poseVec[b].rot, rot);
                XMStoreFloat3(&poseVec[b].scl, sc);
            }
            lastClip = anim->primaryClip;
        }

        // Sample morph weights (reads MorphComponent — must be serial for ECS safety).
        auto* morphComp = world.GetComponent<MorphComponent>(e);
        if (morphComp && morphComp->primaryMorphClip != kInvalidAnimHandle
                      && morphComp->primaryMorphClip < m_morphClips.Count())
        {
            const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
            SampleMorphClip(mc, anim->primaryTime, morphComp);
        }

        animJobs.push_back({ e, anim, skel });
    }

    // Phase 2 (parallel): sample keyframes + cross-fade blend + pre-IK grants.
    // Each entity writes to its own m_localPoseCache[e] slot — no conflicts.
    auto sampleEntity = [this](const AnimJob& j)
    {
        std::vector<LocalPose>& poseVec = m_localPoseCache[j.e];
        const ClipAsset& clip = m_clips.Get(j.anim->primaryClip);

        SampleClip(clip, j.anim->primaryTime, j.skel->boneCount, poseVec.data());

        // Cross-fade blend
        if (j.anim->secondaryClip != kInvalidAnimHandle && j.anim->blendWeight > 0.001f)
        {
            const ClipAsset& sec = m_clips.Get(j.anim->secondaryClip);
            if (sec.frameCount > 0)
            {
                std::vector<LocalPose> secPose(j.skel->boneCount);
                SampleClip(sec, j.anim->secondaryTime, j.skel->boneCount, secPose.data());

                const float w = j.anim->blendWeight;
                for (uint32_t b = 0; b < j.skel->boneCount; ++b)
                {
                    poseVec[b].pos = Lerp3(poseVec[b].pos, secPose[b].pos, w);
                    poseVec[b].rot = Slerp4(poseVec[b].rot, secPose[b].rot, w);
                    poseVec[b].scl = Lerp3(poseVec[b].scl, secPose[b].scl, w);
                }
            }
        }

        // Pre-IK grants
        const SkeletonAsset& skelAsset = m_skeletons.Get(j.skel->assetIndex);
        int32_t minIKOrder = INT32_MAX;
        for (const auto& chain : skelAsset.ikChains)
        {
            int32_t o = skelAsset.transformOrder[chain.ikBoneIndex];
            if (o < minIKOrder) minIKOrder = o;
        }

        const XMVECTOR identityQ = XMQuaternionIdentity();
        for (uint32_t b = 0; b < j.skel->boneCount; ++b)
        {
            const int32_t src = skelAsset.grantSource[b];
            if (src < 0) continue;
            const float ratio = skelAsset.grantRatio[b];
            const float absRatio = fabsf(ratio);
            if (absRatio < 0.001f) continue;
            if (skelAsset.transformOrder[b] >= minIKOrder) continue;

            XMVECTOR srcRot = XMLoadFloat4(&poseVec[static_cast<uint32_t>(src)].rot);
            if (ratio < 0.0f)
                srcRot = XMQuaternionConjugate(srcRot);

            XMVECTOR grantRot = (absRatio >= 0.999f)
                ? srcRot
                : XMQuaternionSlerp(identityQ, srcRot, absRatio);

            XMVECTOR ownRot = XMLoadFloat4(&poseVec[b].rot);
            XMStoreFloat4(&poseVec[b].rot,
                          XMQuaternionNormalize(XMQuaternionMultiply(ownRot, grantRot)));
        }
    };

    if (animJobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(animJobs.size()),
            [&](uint32_t i) { sampleEntity(animJobs[i]); });
    }
    else if (!animJobs.empty())
    {
        sampleEntity(animJobs[0]);
    }

    // ---- Second pass: morph-only entities (no AnimationComponent) ----------
    // Iterate MorphComponent pool directly.
    auto* pMorph = world.GetPool<MorphComponent>();
    const size_t morphN = pMorph ? pMorph->Data().size() : 0;
    const auto&  morphEnts = pMorph ? pMorph->Entities() : std::vector<Entity>{};
    for (size_t mi = 0; mi < morphN; ++mi)
    {
        const Entity e = morphEnts[mi];
        auto* morphComp = &pMorph->Data()[mi];
        if (morphComp->primaryMorphClip == kInvalidAnimHandle) continue;
        if (morphComp->primaryMorphClip >= m_morphClips.Count()) continue;

        // Skip entities already handled by the bone animation pass above
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (anim && anim->primaryClip != kInvalidAnimHandle && !anim->paused) continue;

        if (!morphComp->paused)
        {
            morphComp->time += dt;
            const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
            if (morphComp->looping && mc.duration > 0.f)
                morphComp->time = std::fmod(morphComp->time, mc.duration);
            else
                morphComp->time = std::min(morphComp->time, mc.duration);
        }

        const MorphClipAsset& mc = m_morphClips.Get(morphComp->primaryMorphClip);
        SampleMorphClip(mc, morphComp->time, morphComp);
    }

    // ---- Third pass: rest-pose fallback for skeletons without a clip --------
    // Keeps the GPU skinning pipeline running on un-animated characters so
    // MorphComponent weights (authored in the editor or by scripts) still
    // deform the mesh. Without this, LocalToWorldSystem skips → poseByteOffset
    // stays ~0u → SkinningPass + morph compute never run.
    auto* pSkelAll = world.GetPool<SkeletonComponent>();
    const size_t skelAllN = pSkelAll ? pSkelAll->Data().size() : 0;
    const auto&  skelAllEnts = pSkelAll ? pSkelAll->Entities() : std::vector<Entity>{};
    for (size_t si = 0; si < skelAllN; ++si)
    {
        const Entity e = skelAllEnts[si];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        auto* skel = &pSkelAll->Data()[si];
        if (skel->assetIndex == kInvalidAnimHandle || skel->boneCount == 0) continue;

        // Skip if a clip already wrote a fresh pose in pass 1.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (anim && anim->primaryClip != kInvalidAnimHandle &&
            anim->primaryClip < m_clips.Count()) continue;

        EnsureCacheSize(e);
        std::vector<LocalPose>& poseVec = m_localPoseCache[e];
        if (poseVec.size() == skel->boneCount && m_lastClipCache[e] == kInvalidAnimHandle - 1u)
            continue; // already initialized to rest pose previously, no clip churn

        const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);
        poseVec.resize(skel->boneCount);
        for (uint32_t b = 0; b < skel->boneCount; ++b)
        {
            XMVECTOR sc, rot, tr;
            // Check the return value: XMMatrixDecompose leaves sc/rot/tr
            // UNSPECIFIED on failure (typically NaN). If we store those
            // unchecked, LocalToWorld propagates NaN into the skin matrix,
            // processAABB reads it, vmin/vmax become NaN/Inf, that lands
            // in WorldAabb and SceneBVH::BuildRecursive AVs at bins[b]
            // (Center() = NaN → (int)NaN = INT_MIN, out-of-range write).
            // Fall back to translation-only on failure — preserves bone
            // position so the chain still places skinned vertices roughly
            // right and stops the NaN propagation cold.
            if (XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skelAsset.restPoseLocal[b])))
            {
                XMStoreFloat3(&poseVec[b].pos, tr);
                XMStoreFloat4(&poseVec[b].rot, rot);
                XMStoreFloat3(&poseVec[b].scl, sc);
            }
            else
            {
                const XMFLOAT4X4& m = skelAsset.restPoseLocal[b];
                poseVec[b].pos = { m._41, m._42, m._43 };
                poseVec[b].rot = { 0.f, 0.f, 0.f, 1.f };
                poseVec[b].scl = { 1.f, 1.f, 1.f };
            }
        }
        // Sentinel: rest-pose state. Re-bind the clip later → first pass sees
        // lastClip != primaryClip and re-inits properly.
        m_lastClipCache[e] = kInvalidAnimHandle - 1u;
    }
}

// ---------------------------------------------------------------------------
// ApplyPostIKGrants — D-bones and other grants that need post-IK source data.
// Called after IKSystem::Update() so source bones have their IK-solved rotations.
// ---------------------------------------------------------------------------
void AnimationSystem::ApplyPostIKGrants(World& world)
{
    const XMVECTOR identityQ = XMQuaternionIdentity();

    // Iterate SkeletonComponent pool directly.
    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = pSkel ? pSkel->Entities() : std::vector<Entity>{};
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        auto* skel = &pSkel->Data()[si];
        if (skel->boneCount == 0) continue;

        if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) continue;
        LocalPose* poseVec = m_localPoseCache[e].data();

        const SkeletonAsset& skelAsset = m_skeletons.Get(skel->assetIndex);

        // Determine min IK transform order (same logic as pre-IK pass)
        int32_t minIKOrder = INT32_MAX;
        for (const auto& chain : skelAsset.ikChains)
        {
            int32_t o = skelAsset.transformOrder[chain.ikBoneIndex];
            if (o < minIKOrder) minIKOrder = o;
        }

        for (uint32_t b = 0; b < skel->boneCount; ++b)
        {
            const int32_t src = skelAsset.grantSource[b];
            if (src < 0) continue;
            const float ratio = skelAsset.grantRatio[b];
            const float absRatio = fabsf(ratio);
            if (absRatio < 0.001f) continue;

            // Only process post-IK grants (skipped in pre-IK pass)
            if (skelAsset.transformOrder[b] < minIKOrder) continue;

            XMVECTOR srcRot = XMLoadFloat4(&poseVec[static_cast<uint32_t>(src)].rot);
            if (ratio < 0.0f)
                srcRot = XMQuaternionConjugate(srcRot);

            XMVECTOR grantRot = (absRatio >= 0.999f)
                ? srcRot
                : XMQuaternionSlerp(identityQ, srcRot, absRatio);

            XMVECTOR ownRot = XMLoadFloat4(&poseVec[b].rot);
            XMStoreFloat4(&poseVec[b].rot,
                          XMQuaternionNormalize(XMQuaternionMultiply(ownRot, grantRot)));
        }
    }
}

void AnimationSystem::SampleMorphClip(const MorphClipAsset& mc,
                                       float                 time,
                                       MorphComponent*       out) const
{
    if (mc.frameCount == 0 || mc.frameRate <= 0.f || mc.morphCount == 0) return;

    const float    rawFrame = time * mc.frameRate;
    const uint32_t f0       = static_cast<uint32_t>(rawFrame) % mc.frameCount;
    const uint32_t f1       = (f0 + 1) % mc.frameCount;
    const float    alpha    = rawFrame - std::floorf(rawFrame);

    out->count = std::min(mc.morphCount, static_cast<uint32_t>(MorphComponent::MAX));
    for (uint32_t m = 0; m < out->count; ++m)
    {
        const uint32_t i0 = m * mc.frameCount + f0;
        const uint32_t i1 = m * mc.frameCount + f1;
        out->weights[m]   = mc.weights[i0] + alpha * (mc.weights[i1] - mc.weights[i0]);
    }
}

const AnimationSystem::LocalPose* AnimationSystem::GetLocalPose(Entity e) const
{
    if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) return nullptr;
    return m_localPoseCache[e].data();
}

AnimationSystem::LocalPose* AnimationSystem::GetMutableLocalPose(Entity e)
{
    if (e >= m_localPoseCache.size() || m_localPoseCache[e].empty()) return nullptr;
    return m_localPoseCache[e].data();
}

// ===========================================================================
// LocalToWorldSystem
// ===========================================================================
void LocalToWorldSystem::Update(World& world, PoseRingBuffer& poseBuffer,
                                const std::unordered_set<Entity>* activeSet)
{
    // Phase 1 (sequential): collect skeleton entities + allocate ring buffer slots.
    // poseBuffer.Alloc is NOT thread-safe, so allocation must be serial.
    struct SkelJob {
        const SkeletonAsset* asset;
        const AnimationSystem::LocalPose* local;
        XMFLOAT4X4* poses;
        uint32_t boneCount;
    };
    std::vector<SkelJob> skelJobs;

    // Iterate SkeletonComponent pool directly.
    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = pSkel ? pSkel->Entities() : std::vector<Entity>{};
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;
        auto* skel = &pSkel->Data()[si];
        if (skel->assetIndex == kInvalidAnimHandle) continue;

        skel->poseByteOffset = ~0u;

        const AnimationSystem::LocalPose* local = m_animSys.GetLocalPose(e);
        if (!local) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);

        const uint32_t slot = poseBuffer.Alloc(skel->boneCount);
        skel->poseByteOffset = slot * static_cast<uint32_t>(sizeof(XMFLOAT4X4));

        XMFLOAT4X4* poses = poseBuffer.MapSlice(slot, skel->boneCount);
        if (!poses) continue;

        skelJobs.push_back({ &asset, local, poses, skel->boneCount });
    }

    // Phase 2 (parallel): accumulate world matrices + fuse with inverseBindPose.
    // Each skeleton is independent — different pose output slots, different assets.
    auto processJob = [](const SkelJob& j)
    {
        thread_local std::vector<XMFLOAT4X4> worldBuf;
        worldBuf.resize(j.boneCount);

        for (uint32_t b = 0; b < j.boneCount; ++b)
        {
            const int32_t parent = j.asset->parentIndex[b];
            XMMATRIX localMat;

            if (parent < 0)
            {
                XMVECTOR bindSC, bindRot, bindTr;
                XMMatrixDecompose(&bindSC, &bindRot, &bindTr,
                                  XMLoadFloat4x4(&j.asset->restPoseLocal[b]));
                XMFLOAT3 bindPos;
                XMStoreFloat3(&bindPos, bindTr);
                AnimationSystem::LocalPose rootPose = j.local[b];
                rootPose.pos = bindPos;
                localMat = XMMatrixScaling(rootPose.scl.x, rootPose.scl.y, rootPose.scl.z)
                         * XMMatrixRotationQuaternion(XMLoadFloat4(&rootPose.rot))
                         * XMMatrixTranslation(rootPose.pos.x, rootPose.pos.y, rootPose.pos.z);
            }
            else
            {
                const auto& lp = j.local[b];
                localMat = XMMatrixScaling(lp.scl.x, lp.scl.y, lp.scl.z)
                         * XMMatrixRotationQuaternion(XMLoadFloat4(&lp.rot))
                         * XMMatrixTranslation(lp.pos.x, lp.pos.y, lp.pos.z);
                localMat = localMat * XMLoadFloat4x4(&worldBuf[static_cast<uint32_t>(parent)]);
            }

            XMStoreFloat4x4(&worldBuf[b], localMat);
            XMMATRIX skin = XMLoadFloat4x4(&j.asset->inverseBindPose[b]) * localMat;
            XMStoreFloat4x4(&j.poses[b], skin);
        }
    };

    if (skelJobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(skelJobs.size()),
            [&](uint32_t i) { processJob(skelJobs[i]); });
    }
    else if (!skelJobs.empty())
    {
        processJob(skelJobs[0]);
    }
}

// ===========================================================================
// SkinMatrixSystem
// ===========================================================================
// Skin matrices are now finalized in LocalToWorldSystem (once per skeleton).
// This pass only allocates per-mesh vertex ring buffer output slots.
// It resolves the skeleton entity via SkeletonRef (new layout) or falls back
// to the entity itself (legacy layout: all components on the same entity).
void SkinMatrixSystem::Update(World& world, PoseRingBuffer& /*poseBuffer*/,
                               SkinnedVertexRing& vertRing)
{
    // Iterate MeshSkinnedComponent pool directly — only ~tens of skinned
    // entities vs 22k world entities, killing the hash-lookup dominance.
    auto* pMesh    = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut = world.GetPool<SkinningOutputComponent>();
    auto* pRef     = world.GetPool<SkeletonRef>();
    auto* pSkel    = world.GetPool<SkeletonComponent>();
    if (!pMesh) return;
    const size_t meshN = pMesh->Data().size();
    const auto&  meshEnts = pMesh->Entities();
    for (size_t mi = 0; mi < meshN; ++mi)
    {
        const Entity e = meshEnts[mi];
        auto* mesh    = &pMesh->Data()[mi];
        auto* skinOut = pSkinOut ? pSkinOut->Get(e) : nullptr;
        if (!skinOut) continue;

        // Resolve which entity holds SkeletonComponent (new: via SkeletonRef;
        // legacy: same entity has both MeshSkinnedComponent + SkeletonComponent).
        Entity skelEntity = e;
        const SkeletonRef* ref = pRef ? pRef->Get(e) : nullptr;
        if (ref && ref->entity != NullEntity)
            skelEntity = ref->entity;

        const SkeletonComponent* skel = pSkel ? pSkel->Get(skelEntity) : nullptr;
        if (!skel || skel->assetIndex == kInvalidAnimHandle) continue;

        // Propagate pose validity. ~0u = no active animation this frame.
        // When invalid, skip vertex ring allocation — BuildSkinJobs will also skip.
        skinOut->poseByteOffset = skel->poseByteOffset;
        if (skinOut->poseByteOffset == ~0u) continue;

        // Allocate per-mesh output slices in the skinned vertex ring.
        skinOut->outPosByteOffset = vertRing.AllocPosition(mesh->vertexCount);
        skinOut->outNrmByteOffset = vertRing.AllocNormal(mesh->vertexCount);
    }
}
