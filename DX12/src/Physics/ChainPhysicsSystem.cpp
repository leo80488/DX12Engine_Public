#include "Physics/ChainPhysicsSystem.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h"
#include "System/TaskSystem.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <map>
#include <unordered_set>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static XMMATRIX LocalPoseToMat(const AnimationSystem::LocalPose& p)
{
    XMMATRIX S = XMMatrixScaling(p.scl.x, p.scl.y, p.scl.z);
    XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&p.rot));
    XMMATRIX T = XMMatrixTranslation(p.pos.x, p.pos.y, p.pos.z);
    return S * R * T;
}

static float Distance3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrtf(dx * dx + dy * dy + dz * dz);
}

// Check if a bone name matches any physics pattern.
// Returns: 0 = not physics, 1 = hair, 2 = skirt, 3 = spring bone (jiggle)
static int ClassifyBone(const char* name)
{
    if (!name || !name[0]) return 0;

    std::string s(name);

    // Hair patterns
    if (s.find("Hair") != std::string::npos ||
        s.find("hair") != std::string::npos ||
        s.find("PonyTail") != std::string::npos ||
        s.find("ponytail") != std::string::npos ||
        s.find("Ponytail") != std::string::npos ||
        s.find("Tape") != std::string::npos ||
        s.find("髮") != std::string::npos ||
        s.find("馬尾") != std::string::npos 


        )
        return 1;

    // Skirt patterns
    if (s.find("Skirt") != std::string::npos ||
        s.find("skirt") != std::string::npos ||
        s.find("Scarf") != std::string::npos ||
        s.find("Sleeve") != std::string::npos ||
		s.find("Coat") != std::string::npos 
        )
        return 2;

    // Spring bone patterns (single jiggle bones)
    if (s.find("Chest") != std::string::npos ||
        s.find("chest") != std::string::npos ||
        s.find("Butt") != std::string::npos ||
        s.find("butt") != std::string::npos)
        return 3;

    return 0;
}

// Parse skirt bone name "Skirt_X_Y" → group X, chain Y.
// X = constraint group (all chains in the same X share horizontal/shear constraints)
// Y = chain index (position around the ring)
// Also supports direction-based naming: "Skirt_LF", "Skirt_MF", etc.
struct SkirtID { int group; int chain; };

static SkirtID ParseSkirtID(const char* name)
{
    std::string s(name);

    // Direction-based naming: Skirt_LF, Skirt_MF, etc. → single group 0
    struct DirMap { const char* tag; int idx; };
    static const DirMap dirs[] = {
        { "LF", 0 }, { "MF", 1 }, { "RF", 2 },
        { "RB", 3 }, { "MB", 4 }, { "LB", 5 },
        { "FL", 0 }, { "FM", 1 }, { "FR", 2 },
        { "BR", 3 }, { "BM", 4 }, { "BL", 5 },
    };
    for (const auto& d : dirs)
    {
        if (s.find(d.tag) != std::string::npos)
            return { 0, d.idx };
    }

    // Numeric naming: "Skirt_X_Y" where X = group, Y = chain
    auto pos = s.find("Skirt_");
    if (pos == std::string::npos) pos = s.find("skirt_");
    if (pos != std::string::npos)
    {
        pos += 6; // skip "Skirt_"
        // Parse X
        int x = 0;
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                x = x * 10 + (s[pos++] - '0');
        }
        else return { 0, -1 };

        // Skip separator '_'
        if (pos < s.size() && s[pos] == '_') ++pos;

        // Parse Y
        int y = 0;
        if (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
                y = y * 10 + (s[pos++] - '0');
            return { x, y };
        }

        // Only X present (e.g. "Skirt_3"), treat as single group, X = chain
        return { 0, x };
    }

    return { 0, -1 };
}

// Build children list for skeleton (SkeletonAsset only stores parentIndex).
static void BuildChildrenList(const SkeletonAsset& skel,
                              std::vector<std::vector<uint32_t>>& children)
{
    children.resize(skel.boneCount);
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        int32_t p = skel.parentIndex[b];
        if (p >= 0 && static_cast<uint32_t>(p) < skel.boneCount)
            children[p].push_back(b);
    }
}

// ===========================================================================
// ComputeWorldTransform  -  same as IKSystem
// ===========================================================================
XMMATRIX ChainPhysicsSystem::ComputeWorldTransform(
    uint32_t boneIndex,
    const AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel) const
{
    uint32_t chain[64]; // 256 bytes vs 4KB — fits in L1 cache
    uint32_t depth = 0;
    int32_t  cur   = static_cast<int32_t>(boneIndex);
    while (cur >= 0 && depth < 64)
    {
        chain[depth++] = static_cast<uint32_t>(cur);
        cur = skel.parentIndex[cur];
    }

    XMMATRIX world = XMMatrixIdentity();
    for (uint32_t i = depth; i > 0; --i)
        world = LocalPoseToMat(poses[chain[i - 1]]) * world;

    return world;
}

// ===========================================================================
// ComputeBindWorldPos  -  world position of a bone from current poses
// ===========================================================================
XMFLOAT3 ChainPhysicsSystem::ComputeBindWorldPos(
    uint32_t bone,
    const SkeletonAsset& skel,
    const AnimationSystem::LocalPose* poses)
{
    XMMATRIX w = ComputeWorldTransform(bone, poses, skel);
    XMFLOAT3 pos;
    XMStoreFloat3(&pos, w.r[3]);
    return pos;
}

// ===========================================================================
// TraceChains  -  DFS that forks at branches, producing one chain per leaf path
// ===========================================================================
void ChainPhysicsSystem::TraceChains(
    uint32_t root,
    const SkeletonAsset& skel,
    const std::vector<bool>& isPhysBone,
    const std::vector<std::vector<uint32_t>>& children,
    std::vector<uint32_t>& current,
    std::vector<std::vector<uint32_t>>& outChains)
{
    current.push_back(root);

    // Collect physics children of this node
    std::vector<uint32_t> physChildren;
    for (uint32_t child : children[root])
        if (child < skel.boneCount && isPhysBone[child])
            physChildren.push_back(child);

    if (physChildren.empty())
    {
        // Tip reached — save current path as a complete chain
        outChains.push_back(current);
    }
    else
    {
        // Recurse into each physics child (fork = separate chains)
        for (uint32_t child : physChildren)
            TraceChains(child, skel, isPhysBone, children, current, outChains);
    }

    current.pop_back();
}

// ===========================================================================
// InitEntity  -  scan skeleton, detect chains, build particles & constraints
// ===========================================================================
void ChainPhysicsSystem::InitEntity(
    Entity e,
    const SkeletonAsset& skel,
    const AnimationSystem::LocalPose* poses)
{
    EntityData& data = m_entityData[e];
    data = EntityData{}; // reset

    // Step 1: Classify all bones
    std::vector<int> boneClass(skel.boneCount, 0);
    std::vector<bool> isPhysBone(skel.boneCount, false);

    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        boneClass[b] = ClassifyBone(skel.boneNames[b]);
        isPhysBone[b] = (boneClass[b] != 0);
    }

    // Step 2: Build children list once (shared by all TraceChains calls)
    std::vector<std::vector<uint32_t>> children;
    BuildChildrenList(skel, children);

    // Step 3: Find chain roots (physics bones whose parent is NOT a physics bone)
    std::vector<uint32_t> hairRoots, skirtRoots;
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        if (!isPhysBone[b]) continue;

        int32_t parent = skel.parentIndex[b];
        bool parentIsPhys = (parent >= 0 && isPhysBone[parent]);

        if (!parentIsPhys)
        {
            if (boneClass[b] == 1) hairRoots.push_back(b);
            else if (boneClass[b] == 2) skirtRoots.push_back(b);
        }
    }

    // Step 4: Build hair strands (DFS handles forks → one chain per leaf path)
    for (uint32_t root : hairRoots)
    {
        std::vector<std::vector<uint32_t>> chains;
        std::vector<uint32_t> current;
        TraceChains(root, skel, isPhysBone, children, current, chains);

        for (auto& chain : chains)
        {
            if (chain.size() < 2) continue; // need at least 2 bones for a constraint

            Strand strand;
            strand.type = StrandType::Hair;
            strand.particleOffset = static_cast<uint32_t>(data.particles.size());
            strand.particleCount  = static_cast<uint32_t>(chain.size());
            strand.ringIndex      = -1;

            for (size_t i = 0; i < chain.size(); ++i)
            {
                Particle p;
                p.position     = ComputeBindWorldPos(chain[i], skel, poses);
                p.prevPosition = p.position;
                p.invMass      = (i == 0) ? 0.f : 1.f; // root is pinned
                p.boneIndex    = chain[i];
                data.particles.push_back(p);
            }

            data.strands.push_back(strand);
        } // for each chain from DFS
    } // for each hair root

    // Step 5: Build skirt strands
    // Parse Skirt_X_Y: group by X (constraint group), sort by Y (chain position in ring).
    // Each X group forms an independent ring of horizontal/shear constraints.
    struct SkirtChainEntry { int group; int chain; std::vector<uint32_t> bones; };
    std::vector<SkirtChainEntry> skirtEntries;

    for (uint32_t root : skirtRoots)
    {
        SkirtID sid = ParseSkirtID(skel.boneNames[root]);
        if (sid.chain < 0) sid.chain = static_cast<int>(skirtEntries.size());

        std::vector<std::vector<uint32_t>> chains;
        std::vector<uint32_t> current;
        TraceChains(root, skel, isPhysBone, children, current, chains);

        if (chains.empty() || chains[0].size() < 2) continue;
        skirtEntries.push_back({ sid.group, sid.chain, std::move(chains[0]) });
    }

    // Sort by group first, then by chain index within group
    std::sort(skirtEntries.begin(), skirtEntries.end(),
        [](const SkirtChainEntry& a, const SkirtChainEntry& b) {
            if (a.group != b.group) return a.group < b.group;
            return a.chain < b.chain;
        });

    // Build strands and record group boundaries for constraint building
    struct SkirtGroup { int firstStrand; int strandCount; };
    std::map<int, SkirtGroup> skirtGroups; // group X -> strand range

    for (auto& entry : skirtEntries)
    {
        int strandIdx = static_cast<int>(data.strands.size());
        auto it = skirtGroups.find(entry.group);
        if (it == skirtGroups.end())
            skirtGroups[entry.group] = { strandIdx, 1 };
        else
            it->second.strandCount++;

        Strand strand;
        strand.type = StrandType::Skirt;
        strand.particleOffset = static_cast<uint32_t>(data.particles.size());
        strand.particleCount  = static_cast<uint32_t>(entry.bones.size());
        strand.ringIndex      = entry.chain;

        for (size_t i = 0; i < entry.bones.size(); ++i)
        {
            Particle p;
            p.position     = ComputeBindWorldPos(entry.bones[i], skel, poses);
            p.prevPosition = p.position;
            p.invMass      = (i == 0) ? 0.f : 1.f;
            p.boneIndex    = entry.bones[i];
            data.particles.push_back(p);
        }

        data.strands.push_back(strand);
    }

    // Alias for constraint building below
    bool hasSkirt = !skirtGroups.empty();

    // Step 6: Build constraints with color groups

    // Group 0/1: Vertical (structural)  -  even/odd segments
    for (auto& strand : data.strands)
    {
        for (uint32_t i = 0; i + 1 < strand.particleCount; ++i)
        {
            uint32_t pA = strand.particleOffset + i;
            uint32_t pB = strand.particleOffset + i + 1;

            Constraint c;
            c.particleA  = pA;
            c.particleB  = pB;
            c.restLength = Distance3(data.particles[pA].position,
                                     data.particles[pB].position);
            c.stiffness  = 1.0f;

            int group = i % 2; // 0 or 1
            data.constraintGroups[group].push_back(c);
        }
    }
    data.groupCount = 2;

    // Group 2/3: Horizontal (skirt only  -  ring neighbours, same segment level)
    // Group 4/5: Shear (skirt only  -  ring neighbours, cross segment)
    // Each constraint group X forms its own ring; chains within a group are
    // connected to their neighbours and the last wraps around to the first.
    if (hasSkirt)
    {
        for (auto& [groupId, sg] : skirtGroups)
        {
            // Collect strands belonging to this group
            std::vector<Strand*> ring;
            for (int si = sg.firstStrand; si < sg.firstStrand + sg.strandCount; ++si)
                ring.push_back(&data.strands[si]);

            int N = static_cast<int>(ring.size());
            if (N < 2) continue;

            for (int ci = 0; ci < N; ++ci)
            {
                Strand* cur  = ring[ci];
                Strand* next = ring[(ci + 1) % N]; // ring wrap

                uint32_t segCount = std::min(cur->particleCount, next->particleCount);

                for (uint32_t si = 0; si < segCount; ++si)
                {
                    uint32_t pA = cur->particleOffset + si;
                    uint32_t pB = next->particleOffset + si;
                    float t = (segCount > 1) ? static_cast<float>(si) / (segCount - 1.f) : 0.f;

                    // Horizontal constraint
                    Constraint h;
                    h.particleA  = pA;
                    h.particleB  = pB;
                    h.restLength = Distance3(data.particles[pA].position,
                                             data.particles[pB].position);
                    h.stiffness  = 0.8f;
                    h.segmentT   = t;
                    data.constraintGroups[2 + (ci % 2)].push_back(h);

                    // Shear constraint (cross-level)
                    if (si + 1 < segCount)
                    {
                        uint32_t pC = next->particleOffset + si + 1;
                        float tShear = (segCount > 1) ? (si + 0.5f) / (segCount - 1.f) : 0.f;

                        Constraint s;
                        s.particleA  = pA;
                        s.particleB  = pC;
                        s.restLength = Distance3(data.particles[pA].position,
                                                 data.particles[pC].position);
                        s.stiffness  = 0.5f;
                        s.segmentT   = tShear;
                        data.constraintGroups[4 + (ci % 2)].push_back(s);
                    }
                }
            }
        }
        data.groupCount = 6;
    }

    // Step 7: Detect spring bones (single jiggle bones like Chest, Butt)
    // First pass: collect all class-3 bones and detect chest L/R pairs.
    struct SpringCandidate { uint32_t bone; bool isChest; };
    std::vector<SpringCandidate> springCandidates;

    // Detect "Chest" bones that share the same parent — eligible for virtual root.
    std::vector<uint32_t> chestBones;
    for (uint32_t b = 0; b < skel.boneCount; ++b)
    {
        if (boneClass[b] != 3) continue;
        std::string name(skel.boneNames[b]);
        bool isChest = (name.find("Chest") != std::string::npos ||
                        name.find("chest") != std::string::npos);
        springCandidates.push_back({ b, isChest });
        if (isChest) chestBones.push_back(b);
    }

    // Group chest bones by shared parent — a pair with the same parent gets a virtual root.
    std::unordered_map<int32_t, std::vector<uint32_t>> chestByParent;
    for (uint32_t cb : chestBones)
        chestByParent[skel.parentIndex[cb]].push_back(cb);

    // Build a set of chest bones that will be reparented under a virtual root.
    std::unordered_set<uint32_t> chestWithVirtualRoot;
    for (auto& [parentIdx, group] : chestByParent)
    {
        if (group.size() >= 2)
            chestWithVirtualRoot.insert(group.begin(), group.end());
    }

    // Create spring bones: virtual roots first, then children, then independent.
    for (auto& [parentIdx, group] : chestByParent)
    {
        if (group.size() < 2) continue; // no pair, handled as independent below

        // Compute midpoint of all chest bones in this group.
        XMFLOAT3 midpoint = { 0.f, 0.f, 0.f };
        for (uint32_t cb : group)
        {
            XMFLOAT3 wp = ComputeBindWorldPos(cb, skel, poses);
            midpoint.x += wp.x; midpoint.y += wp.y; midpoint.z += wp.z;
        }
        float invN = 1.0f / static_cast<float>(group.size());
        midpoint.x *= invN; midpoint.y *= invN; midpoint.z *= invN;

        // Create virtual root spring bone.
        int32_t rootIdx = static_cast<int32_t>(data.springBones.size());
        SpringBone root;
        root.boneIndex      = ~0u;
        root.restLocalPos   = { 0.f, 0.f, 0.f };
        root.currentWorldPos = midpoint;
        root.velocity       = { 0.f, 0.f, 0.f };
        root.isVirtual      = true;
        root.parentBoneIndex = (parentIdx >= 0) ? static_cast<uint32_t>(parentIdx) : 0u;
        data.springBones.push_back(root);

        LOG_INFO("ChainPhysics: created virtual Chest_Root (parent bone %d) for %zu chest bones",
                 parentIdx, group.size());

        // Create child spring bones linked to the virtual root.
        for (uint32_t cb : group)
        {
            XMFLOAT3 wp = ComputeBindWorldPos(cb, skel, poses);
            SpringBone child;
            child.boneIndex      = cb;
            child.restLocalPos   = { poses[cb].pos.x, poses[cb].pos.y, poses[cb].pos.z };
            child.currentWorldPos = wp;
            child.velocity       = { 0.f, 0.f, 0.f };
            child.parentSpringIdx = rootIdx;
            child.offsetFromRoot  = { wp.x - midpoint.x, wp.y - midpoint.y, wp.z - midpoint.z };
            data.springBones.push_back(child);
        }
    }

    // Independent spring bones (non-paired chest or non-chest like Butt).
    for (auto& sc : springCandidates)
    {
        if (chestWithVirtualRoot.count(sc.bone)) continue; // already handled above

        SpringBone sb;
        sb.boneIndex      = sc.bone;
        sb.restLocalPos   = { poses[sc.bone].pos.x, poses[sc.bone].pos.y, poses[sc.bone].pos.z };
        sb.currentWorldPos = ComputeBindWorldPos(sc.bone, skel, poses);
        sb.velocity       = { 0.f, 0.f, 0.f };
        data.springBones.push_back(sb);
    }

    data.initialized = true;

    // Log summary
    uint32_t totalConstraints = 0;
    for (int g = 0; g < data.groupCount; ++g)
        totalConstraints += static_cast<uint32_t>(data.constraintGroups[g].size());

    LOG_INFO("ChainPhysics: entity %u  -  %zu strands, %u particles, %u constraints (%d groups), %zu spring bones",
             e, data.strands.size(),
             static_cast<uint32_t>(data.particles.size()),
             totalConstraints, data.groupCount,
             data.springBones.size());
}

// ===========================================================================
// UpdateRoots  -  pin root particles to their bone's current world position
// ===========================================================================
void ChainPhysicsSystem::UpdateRoots(
    EntityData& data,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    for (auto& strand : data.strands)
    {
        Particle& root = data.particles[strand.particleOffset];
        XMFLOAT3 worldPos = ComputeBindWorldPos(root.boneIndex, skel, poses);
        root.position     = worldPos;
        root.prevPosition = worldPos;
    }
}

// ===========================================================================
// IntegrateStrand  -  Verlet integration for a single strand
// ===========================================================================
void ChainPhysicsSystem::IntegrateStrand(
    EntityData& data, const Strand& strand, float dt,
    float damping, const XMFLOAT3& gravity, float maxVelocity)
{
    float dt2 = dt * dt;
    for (uint32_t i = 0; i < strand.particleCount; ++i)
    {
        Particle& p = data.particles[strand.particleOffset + i];
        if (p.invMass == 0.f) continue;

        float vx = (p.position.x - p.prevPosition.x) * damping;
        float vy = (p.position.y - p.prevPosition.y) * damping;
        float vz = (p.position.z - p.prevPosition.z) * damping;

        // Clamp velocity to prevent explosive motion
        if (maxVelocity > 0.f)
        {
            float vLen2 = vx * vx + vy * vy + vz * vz;
            float maxV2 = maxVelocity * maxVelocity;
            if (vLen2 > maxV2)
            {
                float scale = maxVelocity / std::sqrtf(vLen2);
                vx *= scale;
                vy *= scale;
                vz *= scale;
            }
        }

        XMFLOAT3 newPos;
        newPos.x = p.position.x + vx + gravity.x * dt2;
        newPos.y = p.position.y + vy + gravity.y * dt2;
        newPos.z = p.position.z + vz + gravity.z * dt2;

        p.prevPosition = p.position;
        p.position     = newPos;
    }
}

// ===========================================================================
// SolveConstraints  -  one pass of all constraint groups
// ===========================================================================
void ChainPhysicsSystem::SolveConstraints(EntityData& data, float globalStiffness)
{
    for (int g = 0; g < data.groupCount; ++g)
    {
        for (auto& c : data.constraintGroups[g])
        {
            Particle& pA = data.particles[c.particleA];
            Particle& pB = data.particles[c.particleB];

            float dx = pB.position.x - pA.position.x;
            float dy = pB.position.y - pA.position.y;
            float dz = pB.position.z - pA.position.z;
            float dist = std::sqrtf(dx * dx + dy * dy + dz * dz);

            if (dist < 1e-6f) continue;

            float error = (dist - c.restLength) / dist;
            float stiffness = c.stiffness * globalStiffness;
            float cx = dx * error * 0.5f * stiffness;
            float cy = dy * error * 0.5f * stiffness;
            float cz = dz * error * 0.5f * stiffness;

            if (pA.invMass > 0.f)
            {
                pA.position.x += cx;
                pA.position.y += cy;
                pA.position.z += cz;
            }
            if (pB.invMass > 0.f)
            {
                pB.position.x -= cx;
                pB.position.y -= cy;
                pB.position.z -= cz;
            }
        }
    }
}

// ===========================================================================
// ApplyLocalSpaceConstraint — pull particles toward animation rest positions.
//
// Stiffness decreases along the chain: root bones are pulled strongly (stay
// close to animation), tip bones are nearly free (bounce/sway naturally).
// This gives a natural "stiff root, floppy tip" behavior without angle clamps.
//
// Effective stiffness for particle i in a chain of length N:
//   blend = localStiffness * (1 - i/N)²
// Root (i=0): pinned (invMass=0), skipped.
// First child (i=1): strongest pull.
// Tip (i=N-1): nearly zero pull.
// ===========================================================================
void ChainPhysicsSystem::ApplyLocalSpaceConstraint(
    EntityData& data,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel,
    float localStiffness)
{
    if (localStiffness <= 0.f) return;

    for (auto& strand : data.strands)
    {
        const float chainLen = static_cast<float>(strand.particleCount);

        for (uint32_t i = 1; i < strand.particleCount; ++i)
        {
            uint32_t pIdx = strand.particleOffset + i;
            Particle& p = data.particles[pIdx];

            // Rest position = where animation wants this bone
            XMFLOAT3 restWorld = ComputeBindWorldPos(p.boneIndex, skel, poses);
            p.restPosition = restWorld;

            // Falloff: root child gets full stiffness, tip gets almost none
            float t = static_cast<float>(i) / (chainLen - 1.f); // 0=root child, 1=tip
            float falloff = (1.f - t) * (1.f - t);              // quadratic falloff
            float blend = localStiffness * falloff;

            p.position.x += (restWorld.x - p.position.x) * blend;
            p.position.y += (restWorld.y - p.position.y) * blend;
            p.position.z += (restWorld.z - p.position.z) * blend;
        }
    }
}

// ===========================================================================
// WriteBonePoses  -  convert particle positions back to bone local rotations
//
// Simple approach: work entirely in the PARENT bone's local frame.
//   1. Transform simulated direction (world) into parent's local space
//   2. Bind direction = child bone's local-space rest offset (already local)
//   3. Rotation from bind→sim in local space = the bone's new local rotation
// ===========================================================================
void ChainPhysicsSystem::WriteBonePoses(
    EntityData& data,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    for (auto& strand : data.strands)
    {
        for (uint32_t i = 1; i + 1 < strand.particleCount; ++i)
        {
            uint32_t pIdx      = strand.particleOffset + i;
            uint32_t childIdx  = pIdx + 1;
            uint32_t boneIdx   = data.particles[pIdx].boneIndex;
            uint32_t childBone = data.particles[childIdx].boneIndex;

            XMVECTOR bindLocal = XMVectorSet(
                poses[childBone].pos.x,
                poses[childBone].pos.y,
                poses[childBone].pos.z, 0.f);
            float bindLen = XMVectorGetX(XMVector3Length(bindLocal));
            if (bindLen < 1e-6f) continue;
            bindLocal = XMVectorScale(bindLocal, 1.f / bindLen);

            XMFLOAT3 from = data.particles[pIdx].position;
            XMFLOAT3 to   = data.particles[childIdx].position;
            XMVECTOR simWorld = XMVector3Normalize(XMVectorSubtract(
                XMLoadFloat3(&to), XMLoadFloat3(&from)));

            int32_t parentBone = skel.parentIndex[boneIdx];
            XMMATRIX parentWorld = (parentBone >= 0)
                ? ComputeWorldTransform(static_cast<uint32_t>(parentBone), poses, skel)
                : XMMatrixIdentity();
            XMMATRIX parentInv = XMMatrixInverse(nullptr, parentWorld);

            XMVECTOR simLocal = XMVector3Normalize(
                XMVector3TransformNormal(simWorld, parentInv));

            float dot = XMVectorGetX(XMVector3Dot(bindLocal, simLocal));
            dot = std::min(std::max(dot, -1.f), 1.f);
            if (dot > 0.9999f) continue;

            XMVECTOR cross = XMVector3Cross(bindLocal, simLocal);
            float crossLen = XMVectorGetX(XMVector3Length(cross));
            if (crossLen < 1e-8f) continue;

            float angle = std::acosf(dot);
            XMVECTOR axis = XMVector3Normalize(cross);
            XMVECTOR localRot = XMQuaternionRotationAxis(axis, angle);

            XMStoreFloat4(&poses[boneIdx].rot, XMQuaternionNormalize(localRot));
        }
    }
}

// ===========================================================================
// SimulateSpringBones  -  spring-damper dynamics for jiggle bones
//
// Each spring bone tracks a "goal" position (where animation wants it) and a
// "current" simulated position. The spring force pulls current toward goal,
// while inertia makes it overshoot and oscillate.
//
// ODE:  acceleration = (-k*(pos - goal) - d*vel + gravity) / mass
// Integration: semi-implicit Euler (velocity first, then position).
// ===========================================================================
void ChainPhysicsSystem::SimulateSpringBones(
    EntityData& data, const ChainPhysicsComponent& cfg,
    float dt, AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    // Helper lambda: simulate one spring bone with given params and goal.
    auto simulateOne = [&](SpringBone& sb, const XMFLOAT3& goalWorld,
                           float k, float d, float mass, float grav, float maxDisp)
    {
        float dx = sb.currentWorldPos.x - goalWorld.x;
        float dy = sb.currentWorldPos.y - goalWorld.y;
        float dz = sb.currentWorldPos.z - goalWorld.z;

        float fx = -k * dx - d * sb.velocity.x;
        float fy = -k * dy - d * sb.velocity.y + grav;
        float fz = -k * dz - d * sb.velocity.z;

        float invMass = 1.0f / std::max(mass, 0.01f);
        sb.velocity.x += fx * invMass * dt;
        sb.velocity.y += fy * invMass * dt;
        sb.velocity.z += fz * invMass * dt;

        sb.currentWorldPos.x += sb.velocity.x * dt;
        sb.currentWorldPos.y += sb.velocity.y * dt;
        sb.currentWorldPos.z += sb.velocity.z * dt;

        dx = sb.currentWorldPos.x - goalWorld.x;
        dy = sb.currentWorldPos.y - goalWorld.y;
        dz = sb.currentWorldPos.z - goalWorld.z;
        float dispLen = std::sqrtf(dx * dx + dy * dy + dz * dz);
        float md = std::max(maxDisp, 0.01f);
        if (dispLen > md)
        {
            float scale = md / dispLen;
            sb.currentWorldPos.x = goalWorld.x + dx * scale;
            sb.currentWorldPos.y = goalWorld.y + dy * scale;
            sb.currentWorldPos.z = goalWorld.z + dz * scale;
            sb.velocity.x *= 0.5f;
            sb.velocity.y *= 0.5f;
            sb.velocity.z *= 0.5f;
        }
    };

    // Pass 1: virtual roots — goal = midpoint of children's animation positions.
    for (size_t i = 0; i < data.springBones.size(); ++i)
    {
        SpringBone& sb = data.springBones[i];
        if (!sb.isVirtual) continue;

        // Compute goal: midpoint of all children's current animation positions.
        XMFLOAT3 goal = { 0.f, 0.f, 0.f };
        int childCount = 0;
        for (auto& child : data.springBones)
        {
            if (child.parentSpringIdx == static_cast<int32_t>(i))
            {
                XMFLOAT3 wp = ComputeBindWorldPos(child.boneIndex, skel, poses);
                goal.x += wp.x; goal.y += wp.y; goal.z += wp.z;
                ++childCount;
            }
        }
        if (childCount > 0)
        {
            float inv = 1.0f / static_cast<float>(childCount);
            goal.x *= inv; goal.y *= inv; goal.z *= inv;
        }

        simulateOne(sb, goal,
                    cfg.springStiffness, cfg.springDamping,
                    cfg.springMass, cfg.springGravity, cfg.springMaxDisp);
    }

    // Pass 2: children of virtual roots — goal = root simulated pos + offset.
    for (auto& sb : data.springBones)
    {
        if (sb.parentSpringIdx < 0) continue;
        const SpringBone& parent = data.springBones[sb.parentSpringIdx];

        XMFLOAT3 goal = {
            parent.currentWorldPos.x + sb.offsetFromRoot.x,
            parent.currentWorldPos.y + sb.offsetFromRoot.y,
            parent.currentWorldPos.z + sb.offsetFromRoot.z
        };

        simulateOne(sb, goal,
                    cfg.springChildStiffness, cfg.springChildDamping,
                    cfg.springChildMass, cfg.springChildGravity, cfg.springChildMaxDisp);
    }

    // Pass 3: independent spring bones (no virtual parent).
    for (auto& sb : data.springBones)
    {
        if (sb.isVirtual || sb.parentSpringIdx >= 0) continue;

        XMFLOAT3 goalWorld = ComputeBindWorldPos(sb.boneIndex, skel, poses);
        simulateOne(sb, goalWorld,
                    cfg.springStiffness, cfg.springDamping,
                    cfg.springMass, cfg.springGravity, cfg.springMaxDisp);
    }
}

// ===========================================================================
// WriteSpringBonePoses  -  convert spring bone world pos to local rotation
//
// The spring bone's simulated position defines a displacement from the
// animation goal. Convert this displacement to a local rotation delta
// relative to the parent bone's frame.
// ===========================================================================
void ChainPhysicsSystem::WriteSpringBonePoses(
    EntityData& data,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel)
{
    for (auto& sb : data.springBones)
    {
        if (sb.isVirtual) continue;

        uint32_t bi = sb.boneIndex;
        if (bi >= skel.boneCount) continue;
        int32_t parentIdx = skel.parentIndex[bi];
        if (parentIdx < 0) continue;

        XMMATRIX parentWorld = ComputeWorldTransform(
            static_cast<uint32_t>(parentIdx), poses, skel);
        XMMATRIX parentInv = XMMatrixInverse(nullptr, parentWorld);

        XMVECTOR bindLocal = XMVectorSet(
            sb.restLocalPos.x, sb.restLocalPos.y, sb.restLocalPos.z, 0.f);
        float bindLen = XMVectorGetX(XMVector3Length(bindLocal));
        if (bindLen < 1e-6f) continue;
        XMVECTOR bindDir = XMVectorScale(bindLocal, 1.f / bindLen);

        XMFLOAT3 goalWorld = ComputeBindWorldPos(bi, skel, poses);
        XMVECTOR displacement = XMVectorSubtract(
            XMLoadFloat3(&sb.currentWorldPos),
            XMLoadFloat3(&goalWorld));

        XMVECTOR dispLocal = XMVector3TransformNormal(displacement, parentInv);

        XMVECTOR simLocal = XMVector3Normalize(
            XMVectorAdd(bindDir, dispLocal));
        // ────────────────────────────────────────────────────

        float dot = XMVectorGetX(XMVector3Dot(bindDir, simLocal));
        dot = std::min(std::max(dot, -1.f), 1.f);
        if (dot > 0.9999f) continue;

        XMVECTOR cross = XMVector3Cross(bindDir, simLocal);
        float crossLen = XMVectorGetX(XMVector3Length(cross));
        if (crossLen < 1e-8f) continue;

        float angle = std::acosf(dot);
        XMVECTOR axis = XMVector3Normalize(cross);
        XMVECTOR localRot = XMQuaternionRotationAxis(axis, angle);

        XMStoreFloat4(&poses[bi].rot, XMQuaternionNormalize(localRot));
    }
}

// ===========================================================================
// SolveCapsuleCollision — push non-root particles out of capsule colliders
// ===========================================================================
void ChainPhysicsSystem::SolveCapsuleCollision(
    EntityData& data,
    const WorldCapsule* capsules, int capsuleCount)
{
    for (auto& p : data.particles)
    {
        if (p.invMass <= 0.f) continue; // skip pinned roots

        for (int c = 0; c < capsuleCount; ++c)
        {
            const WorldCapsule& cap = capsules[c];
            float abx = cap.b.x - cap.a.x, aby = cap.b.y - cap.a.y, abz = cap.b.z - cap.a.z;
            float apx = p.position.x - cap.a.x, apy = p.position.y - cap.a.y, apz = p.position.z - cap.a.z;
            float abDot = abx*abx + aby*aby + abz*abz;
            float t = (abDot > 1e-8f) ? std::max(0.f, std::min(1.f, (apx*abx + apy*aby + apz*abz) / abDot)) : 0.f;

            float cx = cap.a.x + t * abx - p.position.x;
            float cy = cap.a.y + t * aby - p.position.y;
            float cz = cap.a.z + t * abz - p.position.z;
            float dist = std::sqrtf(cx*cx + cy*cy + cz*cz);

            if (dist < cap.radius && dist > 1e-6f)
            {
                float push = (cap.radius - dist) / dist;
                p.position.x -= cx * push;
                p.position.y -= cy * push;
                p.position.z -= cz * push;
            }
        }
    }
}

// ===========================================================================
// Simulate  -  full simulation step for one entity
// ===========================================================================
void ChainPhysicsSystem::Simulate(
    EntityData& data,
    const ChainPhysicsComponent& cfg,
    float dt,
    AnimationSystem::LocalPose* poses,
    const SkeletonAsset& skel,
    const WorldCapsule* capsules, int capsuleCount)
{
    // Clamp dt to avoid explosion
    float cdt = std::min(dt, 1.f / 30.f);

    // Bone world matrices are pre-computed in Update() before calling Simulate().
    // Helper: fast bone world-pos lookup from the cache (no chain walk).
    auto cachedBonePos = [&](uint32_t bi) -> XMFLOAT3 {
        if (bi >= skel.boneCount) return { 0,0,0 };
        const auto& m = data.boneWorldMatCache[bi];
        return { m._41, m._42, m._43 };
    };

    // ---- Chain physics (Verlet + PBD + capsule collision with substeps) ----
    if (!data.particles.empty())
    {
        const int   nSub  = std::max(1, std::min(cfg.substeps, 8));
        const float subDt = cdt / static_cast<float>(nSub);
        const bool  hasCapsules = capsules && capsuleCount > 0;

        const float invN        = 1.0f / static_cast<float>(nSub);
        const float hairDampSS  = std::powf(cfg.damping,      invN);
        const float skirtDampSS = std::powf(cfg.skirtDamping,  invN);

        for (int sub = 0; sub < nSub; ++sub)
        {
            const bool isLastSub = (sub == nSub - 1);

            // Pin roots — use cached positions (no chain walk).
            for (auto& strand : data.strands)
            {
                Particle& root = data.particles[strand.particleOffset];
                root.position     = cachedBonePos(root.boneIndex);
                root.prevPosition = root.position;
            }

            for (auto& strand : data.strands)
            {
                const bool  isSkirt = (strand.type == StrandType::Skirt);
                const float damp    = isSkirt ? skirtDampSS     : hairDampSS;
                const float grav    = isSkirt ? cfg.skirtGravity : cfg.gravity;
                const float maxV    = isLastSub
                    ? (isSkirt ? cfg.skirtMaxVelocity : cfg.maxVelocity)
                    : 0.f;

                XMFLOAT3 gravity = { 0.f, grav, 0.f };
                IntegrateStrand(data, strand, subDt, damp, gravity, maxV);
            }

            for (int iter = 0; iter < cfg.iterations; ++iter)
            {
                for (int g = 0; g < data.groupCount; ++g)
                {
                    const float gs = (g < 2) ? cfg.stiffness : cfg.skirtStiffness;
                    const bool isHoriz = (g >= 2);
                    for (auto& c : data.constraintGroups[g])
                    {
                        Particle& pA = data.particles[c.particleA];
                        Particle& pB = data.particles[c.particleB];

                        float dx = pB.position.x - pA.position.x;
                        float dy = pB.position.y - pA.position.y;
                        float dz = pB.position.z - pA.position.z;
                        float dist = std::sqrtf(dx*dx + dy*dy + dz*dz);
                        if (dist < 1e-6f) continue;

                        float error = (dist - c.restLength) / dist;
                        float stiffness = c.stiffness * gs * invN;

                        if (isHoriz)
                            stiffness *= 1.f - c.segmentT * (1.f - cfg.skirtHorizDecay);

                        float cx = dx * error * 0.5f * stiffness;
                        float cy = dy * error * 0.5f * stiffness;
                        float cz = dz * error * 0.5f * stiffness;

                        if (pA.invMass > 0.f) { pA.position.x += cx; pA.position.y += cy; pA.position.z += cz; }
                        if (pB.invMass > 0.f) { pB.position.x -= cx; pB.position.y -= cy; pB.position.z -= cz; }
                    }
                }
            }

            if (hasCapsules)
                SolveCapsuleCollision(data, capsules, capsuleCount);
        }

        // Local space pull — use cached positions (no chain walk).
        for (auto& strand : data.strands)
        {
            const bool isSkirt = (strand.type == StrandType::Skirt);
            const float ls = isSkirt ? cfg.skirtLocalStiffness : cfg.localStiffness;
            if (ls <= 0.f) continue;

            const float chainLen = static_cast<float>(strand.particleCount);
            for (uint32_t i = 1; i < strand.particleCount; ++i)
            {
                Particle& p = data.particles[strand.particleOffset + i];
                XMFLOAT3 restWorld = cachedBonePos(p.boneIndex);
                p.restPosition = restWorld;

                float t = static_cast<float>(i) / (chainLen - 1.f);
                float falloff = (1.f - t) * (1.f - t);
                float blend = ls * falloff;

                p.position.x += (restWorld.x - p.position.x) * blend;
                p.position.y += (restWorld.y - p.position.y) * blend;
                p.position.z += (restWorld.z - p.position.z) * blend;
            }
        }

        WriteBonePoses(data, poses, skel);
    }

    // ---- Spring bones (jiggle physics) ----
    if (!data.springBones.empty())
    {
        SimulateSpringBones(data, cfg, cdt, poses, skel);
        WriteSpringBonePoses(data, poses, skel);
    }
}

// ===========================================================================
// Update  -  main entry point, called each frame
// Two-phase: single-threaded init, then parallel simulate.
// ===========================================================================
void ChainPhysicsSystem::Update(World& world, float dt,
                                 const std::unordered_set<Entity>* activeSet)
{
    // Phase 1 (single-threaded): collect + init entities.
    // Must be serial because InitEntity inserts into m_entityData (unordered_map).
    struct PhysJob {
        Entity e;
        ChainPhysicsComponent* phys;
        AnimationSystem::LocalPose* poses;
        const SkeletonAsset* skel;
        EntityData* data;
        WorldCapsule capsules[CapsuleColliderComponent::MAX_CAPSULES];
        int capsuleCount = 0;
    };
    std::vector<PhysJob> jobs;

    // Iterate ChainPhysicsComponent pool directly — only a handful of
    // entities have physics chains, vs 22k total. Kills the biggest serial
    // scan in the frame.
    auto* pPhys = world.GetPool<ChainPhysicsComponent>();
    auto* pSkel = world.GetPool<SkeletonComponent>();
    const size_t physN = pPhys ? pPhys->Data().size() : 0;
    const auto&  physEnts = pPhys ? pPhys->Entities() : std::vector<Entity>{};
    for (size_t pi = 0; pi < physN; ++pi)
    {
        const Entity e = physEnts[pi];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        auto* phys = &pPhys->Data()[pi];
        if (!phys->enabled) continue;

        auto* skelComp = pSkel ? pSkel->Get(e) : nullptr;
        if (!skelComp || skelComp->assetIndex == kInvalidAnimHandle) continue;

        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        const SkeletonAsset& skel = m_skeletons.Get(skelComp->assetIndex);

        auto it = m_entityData.find(e);
        if (it == m_entityData.end() || !it->second.initialized)
        {
            InitEntity(e, skel, poses);
            EntityData& d = m_entityData[e];
            LOG_INFO("ChainPhysics: RUNNING entity %u — damping=%.3f gravity=%.1f maxVel=%.2f stiffness=%.2f iter=%d strands=%zu particles=%zu springs=%zu",
                     e, phys->damping, phys->gravity, phys->maxVelocity,
                     phys->stiffness, phys->iterations,
                     d.strands.size(), d.particles.size(), d.springBones.size());
        }

        EntityData& data = m_entityData[e];
        if (data.strands.empty() && data.springBones.empty()) continue;

        // Pre-compute bone world matrices (needed for capsule endpoints).
        data.boneWorldMatCache.resize(skel.boneCount);
        for (uint32_t b = 0; b < skel.boneCount; ++b)
        {
            const auto& lp = poses[b];
            XMMATRIX local = XMMatrixScaling(lp.scl.x, lp.scl.y, lp.scl.z)
                           * XMMatrixRotationQuaternion(XMLoadFloat4(&lp.rot))
                           * XMMatrixTranslation(lp.pos.x, lp.pos.y, lp.pos.z);
            int32_t parent = skel.parentIndex[b];
            if (parent >= 0)
                local = local * XMLoadFloat4x4(&data.boneWorldMatCache[parent]);
            XMStoreFloat4x4(&data.boneWorldMatCache[b], local);
        }

        // Build capsule world positions (reads World components — must be single-threaded).
        PhysJob job;
        job.e     = e;
        job.phys  = phys;
        job.poses = poses;
        job.skel  = &skel;
        job.data  = &data;
        job.capsuleCount = 0;

        const auto* capComp = world.GetComponent<CapsuleColliderComponent>(e);
        if (!capComp)
        {
            const Children* ch = world.GetComponent<Children>(e);
            if (ch)
                for (Entity child : ch->entities)
                {
                    capComp = world.GetComponent<CapsuleColliderComponent>(child);
                    if (capComp && capComp->count > 0) break;
                    capComp = nullptr;
                }
        }

        if (capComp && capComp->count > 0)
        {
            for (int ci = 0; ci < capComp->count; ++ci)
            {
                const auto& def = capComp->capsules[ci];
                if (!def.enabled) continue;
                if (def.boneA >= skel.boneCount || def.boneB >= skel.boneCount) continue;

                XMMATRIX wA = XMLoadFloat4x4(&data.boneWorldMatCache[def.boneA]);
                XMMATRIX wB = XMLoadFloat4x4(&data.boneWorldMatCache[def.boneB]);
                XMVECTOR posA = XMVectorAdd(wA.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetA), wA));
                XMVECTOR posB = XMVectorAdd(wB.r[3], XMVector3TransformNormal(XMLoadFloat3(&def.offsetB), wB));

                XMStoreFloat3(&job.capsules[job.capsuleCount].a, posA);
                XMStoreFloat3(&job.capsules[job.capsuleCount].b, posB);
                job.capsules[job.capsuleCount].radius = def.radius;
                job.capsuleCount++;
            }
        }

        jobs.push_back(job);
    }

    // Phase 2 (parallel): simulate all collected entities.
    // Each job's data (EntityData, LocalPose) is per-entity — no conflicts.
    if (jobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(jobs.size()),
            [&](uint32_t i)
            {
                auto& j = jobs[i];
                Simulate(*j.data, *j.phys, dt, j.poses, *j.skel,
                         j.capsules, j.capsuleCount);
            });
    }
    else if (!jobs.empty())
    {
        auto& j = jobs[0];
        Simulate(*j.data, *j.phys, dt, j.poses, *j.skel,
                 j.capsules, j.capsuleCount);
    }
}

// ===========================================================================
// Invalidate  -  force re-detection
// ===========================================================================
void ChainPhysicsSystem::Invalidate(Entity e)
{
    m_entityData.erase(e);
}
