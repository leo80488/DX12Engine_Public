// Intentionally empty — runtime Assimp-based scene loading was removed in
// the P1-P6 mesh-library rewrite. The remaining `SceneLoader` symbol is a
// header-only holder for the `SkinnedMeshPending` struct; see
// include/Scene/SceneLoader.h.
//
// This .cpp is kept so incremental builds with the old .obj file don't
// complain about a missing translation unit. It can be deleted once every
// developer has done a clean build.
