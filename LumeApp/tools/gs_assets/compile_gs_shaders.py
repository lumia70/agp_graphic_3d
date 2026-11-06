"""Compile the LumeGS GLSL sources to SPIR-V for the standalone LumeApp build.

The upstream GN build uses LumeShaderCompiler, which resolves GLSL #includes and
writes .spv (and .lsb) next to the sources. This script reproduces the .spv part
with the SDK's glslang validator, which is enough for the Vulkan backend:

  * every .shader JSON under lume/LumeGS/assets/app/shaders references
    gsshaders://<path>.spv, so the .spv files must sit next to the sources
  * glslang needs GL_GOOGLE_include_directive to honour the #include directives,
    which the compiler enables implicitly; we inject the extension into a
    temporary copy instead of touching the generated assets

Usage:
  python compile_gs_shaders.py [--repo <repo root>] [--out <staging dir>]
"""

import argparse
import os
import subprocess
import sys
import tempfile

INCLUDE_ROOTS = [
    ("lume", "LumeGS", "assets", "app", "shaders"),
    ("lume", "Lume_3D", "api"),
    ("lume", "Lume_3D", "assets"),
    ("lume", "LumeRender", "api"),
    ("lume", "LumeRender", "assets"),
]

GLSLANG = r"D:\Coding\DevEcoStudio\sdk\default\openharmony\toolchains\glslang_validator.exe"


def compile_one(src, dst, includes, glslang):
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    patched = []
    injected = False
    for line in lines:
        patched.append(line)
        if not injected and line.startswith("#version"):
            patched.append("#extension GL_GOOGLE_include_directive : enable")
            injected = True
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fd, tmp = tempfile.mkstemp(suffix=os.path.splitext(src)[1])
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(patched) + "\n")
        cmd = [glslang, "-V", "--target-env", "vulkan1.1"]
        cmd += ["-I" + inc for inc in includes]
        cmd += [tmp, "-o", dst]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            return result.stdout + result.stderr
        return None
    finally:
        os.unlink(tmp)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..")))
    parser.add_argument("--out", default=None)
    parser.add_argument("--glslang", default=GLSLANG)
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    shader_root = os.path.join(repo, "lume", "LumeGS", "assets", "app", "shaders")
    out_root = os.path.abspath(args.out) if args.out else os.path.join(
        repo, "LumeApp", "entry", ".cxx", "gs_assets")
    includes = [os.path.join(repo, *parts) for parts in INCLUDE_ROOTS]

    failures = []
    count = 0
    # NOTE: gs3d_compute_spherical_harmonics(.pack).comp do not compile with a
    # plain glslang invocation (the include chain hides uPc); they are only used
    # for dynamic per-frame SH recompute, so they are skipped for now.
    SKIP = {"computeshader\\gs3d_compute_spherical_harmonics.comp",
            "computeshader\\gs3d_compute_spherical_harmonics_pack.comp"}
    for dirpath, _dirs, files in os.walk(shader_root):
        for name in files:
            ext = os.path.splitext(name)[1].lower()
            if ext not in (".vert", ".frag", ".comp"):
                continue
            src = os.path.join(dirpath, name)
            rel = os.path.relpath(src, shader_root)
            if rel in SKIP:
                print("skip " + rel)
                continue
            dst = os.path.join(out_root, rel + ".spv")
            err = compile_one(src, dst, includes, args.glslang)
            count += 1
            if err:
                failures.append((rel, err))
            else:
                print("ok   " + rel)
    print("compiled %d shader stages, %d failed" % (count, len(failures)))
    for rel, err in failures:
        print("FAIL " + rel)
        print(err)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
