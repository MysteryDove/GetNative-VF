#!/usr/bin/env python3
"""Build an isolated, pinned VVL with the video recording lifetime workaround."""

import argparse
import json
import os
from pathlib import Path
import subprocess

REVISION = "f4874eee15c78d7bdb2b7e60659d539f14741500"
TAG = "vulkan-sdk-1.4.357.0"


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--sdk", type=Path, required=True,
                        help="SDK root containing bin/glslang and bin/spirv-opt")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    source, build, prefix = (value.resolve() for value in (args.source, args.build, args.prefix))
    sdk = args.sdk.resolve()
    tools = [sdk / "bin" / name for name in ("glslang", "spirv-opt")]
    if not all(tool.is_file() for tool in tools):
        parser.error("SDK must contain bin/glslang and bin/spirv-opt")
    if not source.exists():
        run(["git", "clone", "--depth", "1", "--branch", TAG,
             "https://github.com/KhronosGroup/Vulkan-ValidationLayers.git", source])
    revision = run(["git", "-C", source, "rev-parse", "HEAD"],
                   capture_output=True, text=True).stdout.strip()
    if revision != REVISION:
        parser.error(f"source must be pinned to {REVISION}; found {revision}")
    patch = Path(__file__).with_name("vvl-1.4.357-video-parameter-lifetime.patch")
    check = subprocess.run(["git", "-C", str(source), "apply", "--check", str(patch)],
                           capture_output=True)
    if check.returncode == 0:
        run(["git", "-C", source, "apply", patch])
    else:
        # Allow repeated builds of the same patch, without reverting other work.
        run(["git", "-C", source, "apply", "--reverse", "--check", patch])
    run(["cmake", "-S", source, "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DUPDATE_DEPS=ON",
         "-DBUILD_TESTS=OFF", "-DBUILD_WERROR=OFF"])
    run(["python3", source / "scripts/generate_spirv.py",
         "--glslang", tools[0], "--spirv-opt", tools[1]])
    run(["cmake", "--build", build, "-j", args.jobs])
    run(["cmake", "--install", build, "--prefix", prefix])
    manifest_path = prefix / "share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
    manifest = json.loads(manifest_path.read_text())
    libraries = list(prefix.rglob("libVkLayer_khronos_validation.so"))
    if len(libraries) != 1:
        raise RuntimeError("Cannot identify the isolated validation layer library")
    manifest["layer"]["library_path"] = os.path.relpath(libraries[0], manifest_path.parent)
    manifest_path.write_text(json.dumps(manifest, indent=4) + "\n")
    print(f"VK_LAYER_PATH={prefix / 'share/vulkan/explicit_layer.d'}")


if __name__ == "__main__":
    main()
