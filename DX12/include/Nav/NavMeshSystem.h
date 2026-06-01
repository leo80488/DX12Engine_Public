#pragma once

// NavMeshSystem — Recast (build) + Detour (runtime query) wrapper for the
// engine. One singleton-style system owns the active navmesh and exposes:
//
//   - Build(...)         — bake from a triangle soup (typically the collision
//                          .meshlib produced by CollisionMeshBaker)
//   - Save(path) / Load(path)
//                        — .inav binary asset: AssetHeader + raw Detour tile
//   - FindPath(from, to) — straight-path waypoints for a query
//   - Raycast(from, to)  — short-circuit visibility check along the mesh
//
// Coord convention: Y-up, right-handed, world space — matches Jolt + the
// rest of the engine.

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;
class DebugWirePass;
class World;   // global ::World — used by BuildSoupFromWorldColliders below

namespace Nav
{

// File format: .inav
inline constexpr uint32_t MAGIC_NAVMESH = 'RNAV';           // "RNAV" 4CC

struct BuildParams
{
    // Voxelisation cell size. Smaller = more accurate / slower. Default fine
    // for human-scale geometry.
    float cellSize          = 0.30f;
    float cellHeight        = 0.20f;

    // Agent capsule footprint. Anything taller / wider needs a custom build.
    float agentHeight       = 2.00f;
    float agentRadius       = 0.60f;
    float agentMaxClimb     = 0.90f;
    float agentMaxSlopeDeg  = 45.0f;

    // Mesh simplification.
    int   maxEdgeLen        = 12;       // cells
    float maxSimplifyError  = 1.30f;
    int   minRegionArea     = 8 * 8;    // cells^2
    int   mergeRegionArea   = 20 * 20;  // cells^2

    int   maxVertsPerPoly   = 6;
    float detailSampleDist  = 6.0f;     // multiples of cellSize
    float detailSampleMaxError = 1.0f;  // multiples of cellHeight

    // Tile size in cells. 0 or 1 = single-tile (one tile covers the entire
    // world). 32 / 64 / 128 = tile-based — Recast slices the world into a
    // grid of (tileSize * cellSize) world units. Recommended >= 64 cells
    // for >1 km² worlds; 0 for small levels (faster, less per-tile overhead).
    int   tileSize          = 0;
};

struct BuildStats
{
    bool     success      = false;
    uint32_t inputVerts   = 0;
    uint32_t inputTris    = 0;
    uint32_t polyCount    = 0;   // summed across every successfully-built tile
    uint32_t vertCount    = 0;   // summed across every successfully-built tile
    uint32_t tilesTotal   = 0;   // tilesX × tilesY (whole grid, including empty)
    uint32_t tilesBuilt   = 0;   // tiles that produced non-empty geometry
    float    buildMs      = 0.0f;
    std::string error;
};

// Soup of triangles to feed Recast — caller assembles from any source.
struct TriangleSoup
{
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;  // length must be a multiple of 3
};

// Helper — walk every entity with a Mesh ColliderComponent, decode the slice
// from its .meshlib, transform by GlobalTransform, and concatenate into one
// world-space soup. This is what the "Build NavMesh" editor button feeds to
// NavMeshSystem::Build. Each unique .meshlib path is read once.
bool BuildSoupFromWorldColliders(::World& world, TriangleSoup& outSoup);

// Path returned by FindPath.
struct PathResult
{
    std::vector<DirectX::XMFLOAT3> waypoints;
    bool partial = false;   // true if endRef wasn't reached (path truncated)
    bool valid   = false;   // false if start or end couldn't be projected onto mesh
};

class NavMeshSystem
{
public:
    NavMeshSystem();
    ~NavMeshSystem();

    NavMeshSystem(const NavMeshSystem&)            = delete;
    NavMeshSystem& operator=(const NavMeshSystem&) = delete;

    // Build a fresh single-tile navmesh from the given triangle soup.
    // Replaces any previously-loaded mesh on success. Synchronous; expect
    // tens of ms to a few seconds for room-scale to building-scale input.
    BuildStats Build(const TriangleSoup& soup, const BuildParams& params);

    // Serialise the active navmesh to <path>. Updates SourcePath() on success
    // so subsequent SaveScene calls can persist the .inav reference inside the
    // .iscene's W line.
    bool Save(const std::string& path);

    // Replace the active navmesh from <path>. Returns false if the file is
    // missing / malformed.
    bool Load(const std::string& path);

    void Clear();

    bool IsReady() const { return m_navMesh && m_navQuery; }

    // Path query. Both `from` and `to` are projected onto the nearest mesh
    // polygon within `searchExtents` (per-axis half-extents). Returns the
    // straight-path waypoints; first one is `from` projected, last is `to`
    // projected (or wherever the path truncates if unreachable).
    PathResult FindPath(const DirectX::XMFLOAT3& from,
                        const DirectX::XMFLOAT3& to,
                        const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // Quick "is `to` reachable in a straight walk from `from`" — wraps
    // dtNavMeshQuery::raycast. Returns true if no obstruction was hit.
    bool Raycast(const DirectX::XMFLOAT3& from,
                 const DirectX::XMFLOAT3& to,
                 DirectX::XMFLOAT3* outHit = nullptr,
                 const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // Project a world-space point onto the navmesh; returns true if a
    // valid polygon was found within `searchExtents`. Used by AI to
    // snap agents to the surface on spawn.
    bool ProjectToMesh(const DirectX::XMFLOAT3& worldPos,
                       DirectX::XMFLOAT3& outOnMesh,
                       const DirectX::XMFLOAT3& searchExtents = { 2.0f, 4.0f, 2.0f }) const;

    // Source-file path the last Load/Save used. Empty if the mesh was Built
    // and never saved.
    const std::string& SourcePath() const { return m_sourcePath; }

    // Returns the Detour navmesh for advanced callers (Crowd manager, debug
    // viz). Owned by the system; do not delete.
    const dtNavMesh* GetNavMesh() const { return m_navMesh; }

    // Tile grid dimensions — 1×1 for single-tile builds, larger for tile-based.
    int  TilesX() const { return m_tilesX; }
    int  TilesY() const { return m_tilesY; }

    // Push polygon-boundary line segments for every tile into @p dbg. Caller
    // is responsible for clearing & re-issuing each frame (DebugWirePass
    // already does that automatically). `yOffset` lifts the wire above the
    // collision surface so it doesn't z-fight.
    void EmitDebugLines(DebugWirePass& dbg,
                        uint32_t edgeColor = 0xff20c0ffu,
                        uint32_t boundaryColor = 0xff0080ffu,
                        float    yOffset = 0.05f) const;

    // Editor toggle for the per-frame debug emission. Reads from EditorLayer
    // Settings → Show NavMesh. Free public field — flip from anywhere.
    bool debugDraw = false;

private:
    bool InitQuery();
    void FreeAll();

    // Build one tile worth of Detour data. Used both by single-tile builds
    // (tilesX = tilesY = 1, borderSize = 0) and by tiled builds (per-tile
    // bounds expanded by borderSize cells for agent-radius overlap).
    // Returns true if a non-empty tile was produced; otherwise *outNavData
    // is left null. Caller transfers ownership of *outNavData to Detour via
    // DT_TILE_FREE_DATA.
    bool BuildOneTile(const TriangleSoup& soup,
                       const BuildParams& bp,
                       int tx, int ty,
                       const float* tileBmin, const float* tileBmax,
                       int borderSize,
                       unsigned char** outNavData, int* outNavDataSize,
                       BuildStats& running) const;

    // Owned, raw pointers because Detour uses its own allocator (dtAlloc/Free).
    dtNavMesh*       m_navMesh  = nullptr;
    dtNavMeshQuery*  m_navQuery = nullptr;
    std::string      m_sourcePath;

    // We hand ownership of the tile bytes to Detour via DT_TILE_FREE_DATA, so
    // we can't read them back later from the navmesh (getTile is private).
    // Keep our own copy at Build / Load time so Save() can serialise without
    // needing to walk Detour internals. For tile-based builds this stores
    // one tile's worth at a time during construction; the final .inav file
    // is assembled directly from per-tile vectors below.
    std::vector<uint8_t> m_navTileData;

    // Tile grid metadata — set at Build / Load. 1×1 for legacy single-tile.
    int               m_tilesX     = 0;
    int               m_tilesY     = 0;
    DirectX::XMFLOAT3 m_gridOrigin = { 0, 0, 0 };
    float             m_tileWidth  = 0.f;
    float             m_tileHeight = 0.f;
};

} // namespace Nav
