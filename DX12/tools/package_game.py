#!/usr/bin/env python3
"""Package a standalone Game build (no editor) into build/<config>/.

Requires Phase 2 project split: Game.vcxproj (exe) links EngineCore.vcxproj (lib).

Modes:
  --cook       (default)  Walk game.json -> startup_scene -> referenced assets,
                          and copy only those files under asset/.
  --no-cook               Copy the entire asset/ tree (legacy behaviour).

Usage (from the project root, F:\\C++\\DX12\\DX12Engine\\DX12):
    python tools/package_game.py
    python tools/package_game.py --config Debug
    python tools/package_game.py --no-cook
    python tools/package_game.py --skip-build
    python tools/package_game.py --name MyGame
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Re-use pack_assets' builder so we don't have to shell out.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_assets


PROJECT_ROOT = Path(__file__).resolve().parent.parent
VCXPROJ = PROJECT_ROOT / "Game.vcxproj"

# Isolated build tree for packaging. The .vcxproj OutDir/IntDir are prefixed with
# $(BuildRoot); we pass /p:BuildRoot=pkgbuild/ so this CLI build outputs to
# PROJECT_ROOT/pkgbuild/x64/<cfg>/ instead of the shared PROJECT_ROOT/x64/<cfg>/
# tree that the Visual Studio IDE uses. That keeps packaging from ever touching the
# IDE's MSBuild incremental/tracker state — otherwise every `package_game.py` run
# made the next F5/build in VS recompile all of EngineCore. Safe to delete anytime
# (and worth git-ignoring). MSBuild accepts the forward slash on Windows; a trailing
# backslash here would escape the next arg quote, so keep it '/'.
PKG_BUILD_ROOT_NAME = "pkgbuild"
PKG_BUILD_ROOT_PROP = PKG_BUILD_ROOT_NAME + "/"


def build_out_dir(config: str) -> Path:
    """Directory the isolated packaging build writes Game.exe + runtime DLLs to."""
    return PROJECT_ROOT / PKG_BUILD_ROOT_NAME / "x64" / config

# File extensions that carry asset references. When we find one of these in a
# blob of bytes, we walk backwards from the dot to the start of the path token.
ASSET_EXTS = (
    ".itex", ".imsh", ".imat", ".ianim", ".iscn",
    ".iskel", ".imorph", ".meshlib", ".ippc", ".lua", ".iscene",
    ".inav",   # Recast/Detour navmesh — without it AI can't path (BT NavReady
               # fails → enemies fall back to idle, looking "frozen").
    ".ivfx",   # notify/script-driven VFX prefab. Referenced from .ianim notify
               # tracks (and Lua), so it must be a recognised anchor to be cooked;
               # without it notify-spawned VFX silently never appear in the pack.
)

# Extensions whose files we re-scan for transitive references. .lua is included
# so a Logic/BT script's referenced clips (e.g. EnemyStates.lua's *.ianim) get
# cooked too; the scripts themselves are also bundled wholesale (see
# collect_script_paths) because most are startup- or directory-loaded.
SCANNABLE_EXTS = {".iscene", ".imat", ".iscn", ".ippc", ".iskel", ".meshlib", ".lua",
                  # .ianim is scanned so anim-notify tracks' .ivfx prefab refs are
                  # discovered; .ivfx is scanned for the emitter textures/meshes it
                  # references.
                  ".ianim", ".ivfx"}

# Characters that terminate a path token when walking backwards from an
# extension match. .iscene percent-encodes spaces (%20) and is space-free in
# path values; .iscn/.imat/.iskel store raw spaces inside filenames and use
# newline/tab/= as delimiters — so we DO NOT include space here.
#
# ':' IS a stop byte: project-root-relative asset paths never contain a colon,
# but Lua/prose comments do (e.g. EnemyStates.lua's "-- Pairs with: asset/...").
# Without it the backward walk over-captures the whole comment prefix
# ("-- Pairs with: asset/ai/enemy.bt.lua"), which (a) never resolves and (b) on
# Windows is an illegal filename — Path.exists() then raises ERROR_INVALID_NAME.
# Stopping at ':' cleanly recovers the real trailing path instead.
_PATH_STOP_BYTES = set(b"\t\r\n\0\"'=<>{}[]|,;:")


_PERCENT_RE = re.compile(r"%([0-9a-fA-F]{2})")


def percent_decode(s: str) -> str:
    """Decode %XX sequences (matches SceneSerializer::PercentEncode)."""
    return _PERCENT_RE.sub(lambda m: chr(int(m.group(1), 16)), s)


def safe_exists(path) -> bool:
    """Path.exists() that treats a syntactically-invalid path as 'does not
    exist' instead of raising. extract_paths() is a byte-scanning heuristic, so
    a captured token may contain characters that are illegal in a filename
    (e.g. ':' on Windows). Path.exists() raises OSError(ERROR_INVALID_NAME) on
    those (Python <3.8 even for embedded prose), which would abort the whole
    cook over one bogus reference. Swallow it — the token simply isn't a real
    file."""
    try:
        return path.exists()
    except (OSError, ValueError):
        return False


def normalize(p: str) -> str:
    """Normalize a path for dedup: trim whitespace, forward slashes, strip leading ./."""
    # .imat text format writes "key = value" with spaces around '=', so
    # extract_paths can leave a leading space on the captured path. Strip it.
    p = p.strip(" \t")
    p = percent_decode(p)
    p = p.replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def extract_paths(blob: bytes):
    """Walk every extension anchor in the blob, returning the list of unique
       raw (un-resolved) path strings found."""
    raws = set()
    for ext in ASSET_EXTS:
        ext_b = ext.encode("ascii")
        start = 0
        while True:
            pos = blob.find(ext_b, start)
            if pos < 0:
                break
            end = pos + len(ext_b)
            ps = pos
            while ps > 0 and blob[ps - 1] not in _PATH_STOP_BYTES:
                ps -= 1
            try:
                raws.add(blob[ps:end].decode("utf-8", errors="replace"))
            except Exception:
                pass
            start = end
    return raws


def resolve(raw: str, base_dir: str) -> str:
    """Try to resolve a raw extracted reference to a project-root-relative path
       that actually exists. Returns '' if no candidate exists."""
    norm = normalize(raw)
    # (a) already project-root-relative
    if safe_exists(PROJECT_ROOT / norm):
        return norm
    # (b) relative to base_dir (the directory of the file we extracted it from)
    if base_dir:
        joined = os.path.normpath(os.path.join(base_dir, norm)).replace("\\", "/")
        if safe_exists(PROJECT_ROOT / joined):
            return joined
    return ""


def cook_scene(startup_scene):
    """Walk .iscene and transitively scan any file whose extension is in
       SCANNABLE_EXTS. Paths extracted from a file are first tried as
       project-root-relative, then as relative to that file's directory.
       Returns (set_of_resolved_paths, list_of_unresolved_refs)."""
    start = normalize(startup_scene)
    found = {start}
    queue = [start]
    missing = []

    while queue:
        cur = queue.pop()
        full = PROJECT_ROOT / cur
        if not safe_exists(full):
            missing.append(cur)
            continue
        try:
            blob = full.read_bytes()
        except OSError as e:
            missing.append(f"{cur}  ({e})")
            continue

        base_dir = os.path.dirname(cur)
        for raw in extract_paths(blob):
            resolved = resolve(raw, base_dir)
            if not resolved:
                missing.append(normalize(raw))
                continue
            if resolved in found:
                continue
            found.add(resolved)
            sub_ext = os.path.splitext(resolved)[1].lower()
            if sub_ext in SCANNABLE_EXTS:
                queue.append(resolved)

    return found, missing


def find_msbuild() -> str:
    exe = shutil.which("msbuild")
    if exe:
        return exe
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
              "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        out = subprocess.check_output([
            str(vswhere), "-latest", "-requires",
            "Microsoft.Component.MSBuild",
            "-find", r"MSBuild\**\Bin\MSBuild.exe",
        ], text=True).strip().splitlines()
        if out:
            return out[0]
    sys.exit("msbuild.exe not found; run from a Developer Command Prompt, or install vswhere.")


def run_build(config: str) -> None:
    msbuild = find_msbuild()
    print(f"[build] {VCXPROJ.name} /p:Configuration={config} /p:Platform=x64 "
          f"/p:BuildRoot={PKG_BUILD_ROOT_PROP}  (isolated from the IDE's x64\\ tree)")
    subprocess.check_call([
        msbuild, str(VCXPROJ),
        f"/p:Configuration={config}",
        "/p:Platform=x64",
        f"/p:BuildRoot={PKG_BUILD_ROOT_PROP}",
        "/m", "/nologo", "/v:minimal",
    ])


def copy_tree(src: Path, dst: Path, *, skip_names=()) -> None:
    if not src.exists():
        print(f"[skip] {src} does not exist")
        return
    print(f"[copy-tree] {src} -> {dst}")
    for root, dirs, files in os.walk(src):
        dirs[:] = [d for d in dirs if d not in skip_names]
        rel = Path(root).relative_to(src)
        out_dir = dst / rel
        out_dir.mkdir(parents=True, exist_ok=True)
        for f in files:
            if f in skip_names:
                continue
            shutil.copy2(Path(root) / f, out_dir / f)


def copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        print(f"[skip-file] {src} does not exist")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    print(f"[copy]      {src.name} -> {dst}")
    shutil.copy2(src, dst)


def copy_cooked(paths: set, out_dir: Path) -> None:
    print(f"[cook] copying {len(paths)} referenced assets")
    for rel in sorted(paths):
        src = PROJECT_ROOT / rel
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def read_manifest() -> dict:
    manifest_path = PROJECT_ROOT / "game.json"
    if not manifest_path.exists():
        return {}
    try:
        return json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"[warn] failed to parse game.json: {e}")
        return {}


def collect_shader_paths() -> set:
    """Return all root-relative paths under shaders/ (both .hlsl and cached .ishdr)."""
    root = PROJECT_ROOT / "shaders"
    out = set()
    if not root.is_dir():
        return out
    for f in root.rglob("*"):
        if f.is_file():
            out.add(pack_assets.normalize(str(f.relative_to(PROJECT_ROOT))))
    return out


def collect_runtime_dlls(config: str) -> dict:
    """Find the runtime DLLs Game.exe needs, keyed by lowercase filename.

    The build leaves them across a couple of locations (project- and
    solution-level x64/<config>, plus a few loose at the project root):
    freetype, dxcompiler/dxil, the ffmpeg av*/sw* set, and assimp. We union
    those dirs (first match wins) so the package is self-contained — Game.exe
    fails to start with 0xC0000135 (DLL not found) if any load-time dep is
    missing.
    """
    out = {}
    for d in (build_out_dir(config),
              PROJECT_ROOT / "x64" / config,
              PROJECT_ROOT.parent / "x64" / config,
              PROJECT_ROOT):
        if not d.is_dir():
            continue
        for f in d.glob("*.dll"):
            out.setdefault(f.name.lower(), f)
    return out


def collect_script_paths() -> set:
    """Return all root-relative paths under asset/ai/ and asset/scripts/.

    Lua scripts are bundled wholesale (like shaders/) because the cook only
    walks STATIC asset references in the scene graph, while most scripts are
    reached at runtime in ways the cook can't see:
      * App.cpp loads a hardcoded BT-action list (asset/ai/*.lua),
      * ScriptSystem directory-scans asset/scripts/{services,systems,ui}/*.lua,
      * Logic/BT scripts referenced by entities live under these trees too.
    Missing any of them makes a packed Game build fail to load the scene's AI.
    """
    out = set()
    for sub in ("asset/ai", "asset/scripts"):
        root = PROJECT_ROOT / sub
        if not root.is_dir():
            continue
        for f in root.rglob("*"):
            if f.is_file():
                out.add(pack_assets.normalize(str(f.relative_to(PROJECT_ROOT))))
    return out


# Engine-global resources loaded at runtime by HARDCODED path, never referenced
# by any scene — so the dependency cook can't discover them (same situation as
# shaders/ and scripts/, which are bundled wholesale above).
#
# CRITICAL: asset/IBL/BRDF_LUT.itex is the split-sum IBL BRDF integration LUT. If
# it's absent, the environment-specular term evaluates to ~0 and every METAL
# renders solid BLACK in a packed build (diffuse surfaces barely change) — while
# everything looks fine in the editor, which loads loose files off disk.
#
# Keep in sync with the hardcoded loads:
#   src/Graphics/Renderer_IBL.cpp -> "asset/IBL/BRDF_LUT.itex", "asset/Default_Texture/moon.itex"
#   src/App.cpp                   -> "asset/font/FGMiraiRen.ttf"  (UI text)
# Default_Texture/ and font/ are bundled WHOLESALE (both are small) so other
# hardcoded defaults/symbols/fonts come along too.
ENGINE_RESOURCE_DIRS  = ("asset/Default_Texture", "asset/font")
ENGINE_RESOURCE_FILES = ("asset/IBL/BRDF_LUT.itex",)


def collect_engine_resources() -> set:
    out = set()
    for sub in ENGINE_RESOURCE_DIRS:
        root = PROJECT_ROOT / sub
        if not root.is_dir():
            print(f"[warn] engine resource dir missing on disk: {sub}")
            continue
        for f in root.rglob("*"):
            if f.is_file():
                out.add(pack_assets.normalize(str(f.relative_to(PROJECT_ROOT))))
    for rel in ENGINE_RESOURCE_FILES:
        if (PROJECT_ROOT / rel).is_file():
            out.add(pack_assets.normalize(rel))
        else:
            print(f"[warn] engine resource missing on disk: {rel}")
    return out


def package(config: str, out_dir: Path, exe_name: str, cook: bool, pack: bool) -> None:
    if out_dir.exists():
        print(f"[clean] removing {out_dir}")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    # 1. Executable — prefer the isolated packaging build (build_out_dir), but fall
    #    back to the IDE's x64\<cfg>\ tree so `--skip-build` works right after a
    #    Visual Studio build.
    exe_candidates = [
        build_out_dir(config) / "Game.exe",
        PROJECT_ROOT / "x64" / config / "Game.exe",
        PROJECT_ROOT.parent / "x64" / config / "Game.exe",
    ]
    built_exe = next((p for p in exe_candidates if p.exists()), None)
    if built_exe is None:
        searched = "\n  ".join(str(p) for p in exe_candidates)
        sys.exit("Missing built Game.exe. Looked in:\n  " + searched +
                 "\nThe build step must have failed (or build first, then --skip-build).")
    print(f"[exe] using {built_exe}")
    copy_file(built_exe, out_dir / f"{exe_name}.exe")

    # 2. Runtime DLLs — copy whatever the build produced next to the exe so the
    #    package is self-contained. Game.exe has load-time deps (freetype for UI
    #    text, dxcompiler/dxil for runtime shader compile, ffmpeg av*/sw* for
    #    video, assimp); a missing one aborts startup with 0xC0000135.
    dlls = collect_runtime_dlls(config)
    print(f"[dll] copying {len(dlls)} runtime DLL(s)")
    for src in sorted(dlls.values(), key=lambda p: p.name.lower()):
        copy_file(src, out_dir / src.name)

    # 3. Assets — two axes:
    #      cook=True  → walk .iscene deps; False → include whole asset/ tree
    #      pack=True  → single game.ipak bundle; False → loose files on disk
    manifest = read_manifest()
    startup_scene = manifest.get("startup_scene", "") if cook else ""

    asset_paths = set()

    if cook and startup_scene:
        print(f"[cook] walking '{startup_scene}'")
        cooked, missing = cook_scene(startup_scene)
        asset_paths |= cooked
        if missing:
            print(f"[cook] {len(missing)} referenced path(s) do not exist on disk:")
            for m in missing[:20]:
                print(f"       - {m}")
            if len(missing) > 20:
                print(f"       ... and {len(missing) - 20} more")
    else:
        reason = "no startup_scene in game.json" if cook else "cook disabled"
        print(f"[asset] {reason}; including entire asset/ tree")
        for f in (PROJECT_ROOT / "asset").rglob("*"):
            if f.is_file():
                asset_paths.add(pack_assets.normalize(str(f.relative_to(PROJECT_ROOT))))

    # shaders/ is always bundled wholesale — shader permutation selection is
    # runtime-driven and not trivially discoverable statically.
    asset_paths |= collect_shader_paths()

    # Lua scripts are always bundled wholesale too — they are startup-loaded or
    # directory-scanned, so the scene-reference cook can't discover them all.
    asset_paths |= collect_script_paths()

    # Engine-global resources loaded by hardcoded path (BRDF LUT, default
    # textures, fonts). Invisible to the scene cook; without the BRDF LUT every
    # metal renders BLACK in the packed build.
    engine_res = collect_engine_resources()
    asset_paths |= engine_res
    print(f"[engine] bundling {len(engine_res)} engine-global resource(s) "
          f"(BRDF LUT / default textures / fonts)")

    if pack:
        ipak_path = out_dir / "game.ipak"
        pack_assets.build_pak(PROJECT_ROOT, sorted(asset_paths), ipak_path)
    else:
        print(f"[loose] copying {len(asset_paths)} files as loose tree")
        for rel in sorted(asset_paths):
            src = PROJECT_ROOT / rel
            dst = out_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    # 4. PSO cache — kept loose on disk (Game.exe reads it directly, not via AssetFS)
    copy_file(PROJECT_ROOT / "pso_cache.bin", out_dir / "pso_cache.bin")

    # 5. Boot manifest
    manifest_src = PROJECT_ROOT / "game.json"
    manifest_dst = out_dir / "game.json"
    if manifest_src.exists():
        copy_file(manifest_src, manifest_dst)
    else:
        print(f"[manifest] no project game.json; writing template to {manifest_dst}")
        manifest_dst.write_text(
            '{\n'
            '  "_comment": "Edit startup_scene to the .iscene you want to boot.",\n'
            '  "startup_scene": ""\n'
            '}\n',
            encoding="utf-8")

    # 6. Summary
    total = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    print()
    print(f"[done] {out_dir}")
    print(f"       size: {total / (1024*1024):.1f} MiB")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="Release", choices=["Release", "Debug"])
    ap.add_argument("--skip-build", action="store_true",
                    help="Reuse the existing binary in x64/<config>/")
    ap.add_argument("--name", default="Game", help="Output .exe base name")
    ap.add_argument("--out", default=None,
                    help="Output directory (default: build/<config>/)")
    ap.add_argument("--no-cook", action="store_true",
                    help="Include entire asset/ tree instead of cooking dependencies")
    ap.add_argument("--no-pack", action="store_true",
                    help="Ship loose asset files instead of bundling into game.ipak")
    args = ap.parse_args()

    out_dir = Path(args.out) if args.out else PROJECT_ROOT / "build" / args.config

    if not args.skip_build:
        run_build(args.config)

    package(args.config, out_dir, args.name,
            cook=not args.no_cook, pack=not args.no_pack)
    return 0


if __name__ == "__main__":
    sys.exit(main())
