#include "ECS/FootIKTargetSystem.h"
#include "ECS/FootIKComponent.h"
#include "ECS/IKSystem.h"             // public LocalPoseToMatrix / ComputeBoneWorldTransform
#include "ECS/AnimationSystem.h"
#include "ECS/AnimationComponents.h"  // SkeletonComponent
#include "ECS/HierarchyComponents.h"  // GlobalTransform
#include "Resource/SkeletonAsset.h"
#include "Physics/PhysicsSystem.h"
#include "System/Log.h"

#include <cctype>
#include <cstring>
#include <string>

using namespace DirectX;

namespace
{

// Naive case-insensitive substring match on a 64-char bone name buffer.
// PMX bone names are UTF-8 with multi-byte Japanese codepoints, so we don't
// touch UTF-8 bytes — just do a byte-wise compare. The romaji branches are
// ASCII-only and so safe under tolower.
bool ContainsBytes(const char* hay, const char* needle)
{
    return std::strstr(hay, needle) != nullptr;
}

bool ContainsCI(const char* hay, const char* needle)
{
    // ASCII case-insensitive. Used only for romaji probes.
    const size_t hlen = std::strlen(hay), nlen = std::strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i)
    {
        bool ok = true;
        for (size_t j = 0; j < nlen; ++j)
        {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

// UTF-8 byte sequences for the PMX Japanese substrings we match against.
// Written as hex escapes so MSVC's C++20 char8_t coercion doesn't get in
// the way — these stay `const char[]` regardless of source-charset.
//   左   = E5 B7 A6
//   右   = E5 8F B3
//   足   = E8 B6 B3
//   ＩＫ = EF BC A9 EF BC AB  (full-width I + K)
static constexpr const char kJpLeft[]      = "\xE5\xB7\xA6";
static constexpr const char kJpRight[]     = "\xE5\x8F\xB3";
static constexpr const char kJpFootIKFull[] = "\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB"; // 足ＩＫ
static constexpr const char kJpFootIKHalf[] = "\xE8\xB6\xB3" "IK";                    // 足IK

// Returns 0 = left, 1 = right, -1 = unknown.
int FootSide(const char* name)
{
    if (ContainsBytes(name, kJpLeft))  return 0;
    if (ContainsBytes(name, kJpRight)) return 1;
    if (ContainsCI(name, "left") || std::strncmp(name, "L_", 2) == 0) return 0;
    if (ContainsCI(name, "right") || std::strncmp(name, "R_", 2) == 0) return 1;
    return -1;
}

// Foot IK control bones in PMX are typically named with the substring
// "足ＩＫ" (UTF-8 ash + full-width ＩＫ). GLTF/FBX rigs imported as PMX-
// compatible sometimes use ASCII "FootIK" / "Foot_IK".
bool LooksLikeFootIKBone(const char* name)
{
    if (ContainsBytes(name, kJpFootIKFull)) return true;
    if (ContainsBytes(name, kJpFootIKHalf)) return true;
    if (ContainsCI(name, "footik"))         return true;
    if (ContainsCI(name, "foot_ik"))        return true;
    return false;
}

// Sweep the skeleton's ikChains and resolve left/right indices. Each side
// stays -1 when no plausible chain exists — caller treats that as "skip".
void AutoDetectFootChains(const SkeletonAsset& skel,
                          int32_t& outLeft, int32_t& outRight)
{
    for (size_t i = 0; i < skel.ikChains.size(); ++i)
    {
        const auto& ch = skel.ikChains[i];
        if (ch.ikBoneIndex >= skel.boneCount) continue;
        const char* boneName = skel.boneNames[ch.ikBoneIndex];
        if (!LooksLikeFootIKBone(boneName)) continue;
        const int side = FootSide(boneName);
        if      (side == 0 && outLeft  < 0) outLeft  = static_cast<int32_t>(i);
        else if (side == 1 && outRight < 0) outRight = static_cast<int32_t>(i);
    }
}

} // namespace

void FootIKTargetSystem::Update(World&                            world,
                                const DX12Physics::PhysicsSystem* physics,
                                const std::unordered_set<Entity>* activeSet)
{
    if (!m_enabled || !physics) return;

    auto* pSkel = world.GetPool<SkeletonComponent>();
    if (!pSkel) return;
    auto* pIK   = world.GetPool<FootIKComponent>();
    if (!pIK) return;
    auto* pAnim = world.GetPool<AnimationComponent>();

    auto& skelData = pSkel->Data();
    auto& skelEnts = pSkel->Entities();
    const size_t n = skelData.size();

    for (size_t si = 0; si < n; ++si)
    {
        const Entity e = skelEnts[si];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        FootIKComponent* fk = pIK->Get(e);
        if (!fk || !fk->enabled || fk->enableWeight <= 0.f) continue;

        const SkeletonComponent& sc = skelData[si];
        if (sc.assetIndex == kInvalidAnimHandle) continue;
        const SkeletonAsset& skel = m_skeletons.Get(sc.assetIndex);
        if (skel.ikChains.empty()) continue;

        // No clip → LocalPose is rest-pose (so morphs deform); foot-snap
        // would shift the rest-pose IK bone Y and the CCD solver would
        // twist the leg. Restore the pre-rest-pose-fallback gating.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (!anim || anim->primaryClip == kInvalidAnimHandle) continue;

        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        // Auto-detect missing chain indices.
        if (fk->leftChainIdx < 0 || fk->rightChainIdx < 0)
            AutoDetectFootChains(skel, fk->leftChainIdx, fk->rightChainIdx);

        // Entity's world transform — used to project the IK bone's animated
        // skeleton-local pose into true world space so we can raycast the
        // physics world at the foot's actual XZ.
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        const XMMATRIX entityW = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();

        auto SnapOne = [&](int32_t chainIdx)
        {
            if (chainIdx < 0 || static_cast<size_t>(chainIdx) >= skel.ikChains.size())
                return;
            const auto& chain = skel.ikChains[chainIdx];
            if (chain.ikBoneIndex >= skel.boneCount) return;

            // Additive delta model — preserves VMD authored IK keyframes.
            //
            // The VMD clip has already written the IK control bone's
            // LocalPose.pos for THIS frame (its choreographed foot
            // position). The animation was authored against a flat floor
            // at the skeleton's entity-local Y=0. On terrain that floor
            // height varies; the entity's GlobalTransform.y is already
            // pinned to the ground at the ENTITY's XZ by GroundSnap, so
            // the per-foot delta is just `terrainY_under_foot - entityY`.
            //
            // We apply that delta to the IK control bone's authored Y —
            // the dance / step keeps its full XYZ trajectory, only
            // shifted to follow the floor under each foot. Flat ground
            // (terrainY == entityY) → delta = 0 → animation untouched.

            // 1. IK control bone world position, AFTER animation wrote it
            // for this frame. We probe terrain at the IK bone's XZ (= where
            // VMD says the foot is heading), not the ankle's XZ (which is
            // last frame's solver output and lags by one IK iteration).
            AnimationSystem::LocalPose& ikPose = poses[chain.ikBoneIndex];
            const XMMATRIX ikBoneSkelW =
                IKSystem::ComputeBoneWorldTransform(chain.ikBoneIndex, poses, skel);
            const XMMATRIX ikBoneTrueW = XMMatrixMultiply(ikBoneSkelW, entityW);
            const float footX = ikBoneTrueW.r[3].m128_f32[0];
            const float footZ = ikBoneTrueW.r[3].m128_f32[2];

            // 2. Entity origin Y in world (the animation's flat baseline).
            const float entityY = gt ? gt->matrix._42 : 0.f;

            // 3. Raycast downward at the foot's (x,z) — start above the
            // entity origin so jumping clips (foot above the body) still
            // find the floor below.
            const DX12Physics::RayHit hit = physics->CastRayClosest(
                { footX, entityY + fk->rayUp, footZ },
                { 0.f, -1.f, 0.f },
                fk->rayUp + fk->rayDown);
            if (!hit.hit) return;  // miss → animation plays unmodified

            // 4. Terrain delta in true world space.
            const float terrainY = hit.point.y + fk->footOffset;
            const float worldDeltaY = (terrainY - entityY) * fk->enableWeight;

            // 5. Apply delta to the authored ikPose.pos. The IK control bone
            // is parented under the skeleton root, whose Y axis tracks the
            // entity's world Y for upright characters (the vast majority of
            // PMX rigs). For prone / tilted entities the delta should be
            // transformed by inv(parent.world) but the visual difference is
            // negligible for upright dancers and gameplay characters.
            ikPose.pos.y += worldDeltaY;
        };

        SnapOne(fk->leftChainIdx);
        SnapOne(fk->rightChainIdx);
    }
}
