// Intentionally empty after the P1-P6 rewrite. .imshpack archives were a
// batch-load optimisation for the legacy per-.imsh pool; the new MeshLibrary
// uploads the whole mesh pool from one .meshlib file in one I/O, so the pack
// is no longer needed. File kept as a stub to keep incremental-build .obj
// files from conflicting with the linker — delete after a clean build.
