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

# File extensions that carry asset references. When we find one of these in a
# blob of bytes, we walk backwards from the dot to the start of the path token.
ASSET_EXTS = (
    ".itex", ".imsh", ".imat", ".ianim", ".iscn",
    ".iskel", ".imorph", ".meshlib", ".ippc", ".lua", ".iscene",
)

# Extensions whose files we re-scan for transitive references.
SCANNABLE_EXTS = {".iscene", ".imat", ".iscn", ".ippc", ".iskel", ".meshlib"}

# Characters that terminate a path token when walking backwards from an
# extension match. .iscene percent-encodes spaces (%20) and is space-free in
# path values; .iscn/.imat/.iskel store raw spaces inside filenames and use
# newline/tab/= as delimiters — so we DO NOT include space here.
_PATH_STOP_BYTES = set(b"\t\r\n\0\"'=<>{}[]|,;")


_PERCENT_RE = re.compile(r"%([0-9a-fA-F]{2})")


def percent_decode(s: str) -> str:
    """Decode %XX sequences (matches SceneSerializer::PercentEncode)."""
    return _PERCENT_RE.sub(lambda m: chr(int(m.group(1), 16)), s)


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
    if (PROJECT_ROOT / norm).exists():
        return norm
    # (b) relative to base_dir (the directory of the file we extracted it from)
    if base_dir:
        joined = os.path.normpath(os.path.join(base_dir, norm)).replace("\\", "/")
        if (PROJECT_ROOT / joined).exists():
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
        if not full.exists():
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
    print(f"[build] {VCXPROJ.name} /p:Configuration={config} /p:Platform=x64")
    subprocess.check_call([
        msbuild, str(VCXPROJ),
        f"/p:Configuration={config}",
        "/p:Platform=x64",
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


def package(config: str, out_dir: Path, exe_name: str, cook: bool, pack: bool) -> None:
    if out_dir.exists():
        print(f"[clean] removing {out_dir}")
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    # 1. Executable
    built_exe = PROJECT_ROOT / "x64" / config / "Game.exe"
    if not built_exe.exists():
        sys.exit(f"Missing built exe: {built_exe}\nBuild step must have failed.")
    copy_file(built_exe, out_dir / f"{exe_name}.exe")

    # 2. Runtime DLLs — intentionally none. All model imports are editor-time;
    # Game.exe only reads .imsh/.itex/.ianim internal formats.

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
