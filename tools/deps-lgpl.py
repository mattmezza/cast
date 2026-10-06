#!/usr/bin/env python3
"""Pinned Linux shared media build. Downloads only when explicitly invoked."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "packaging/media-lgpl-lock.json"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(argv, directory, environment, log):
    with log.open("a") as output:
        output.write(json.dumps(argv) + "\n")
        output.flush()
        subprocess.run(argv, cwd=directory, env=environment, stdout=output,
                       stderr=subprocess.STDOUT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="require verified archives in cache")
    parser.add_argument("--root", type=Path, default=ROOT / "build/deps-lgpl")
    parser.add_argument("--cache", type=Path, default=Path(os.environ.get("CAST_DEPS_CACHE", ROOT / "build/deps-cache")))
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 128:
        parser.error("jobs must be 1..128")
    lock = json.loads(LOCK.read_text())
    base, cache = args.root.resolve(), args.cache.resolve()
    prefix = base / "prefix"
    cache.mkdir(parents=True, exist_ok=True)
    # Validate/download everything before touching a previously usable prefix.
    for source in lock["sources"]:
        archive = cache / source["archive"]
        if not archive.exists():
            if args.offline:
                raise RuntimeError(f"offline cache missing {archive}; expected SHA256 {source['sha256']}")
            partial = archive.with_suffix(archive.suffix + ".partial")
            try:
                with urllib.request.urlopen(source["url"], timeout=60) as response, partial.open("wb") as target:
                    shutil.copyfileobj(response, target)
                if digest(partial) != source["sha256"]:
                    raise RuntimeError(f"download SHA256 mismatch for {source['name']}")
                partial.replace(archive)
            finally:
                partial.unlink(missing_ok=True)
        if digest(archive) != source["sha256"]:
            raise RuntimeError(f"cache SHA256 mismatch: {archive}; cached files are never silently replaced")
    if os.uname().machine in ("x86_64", "i386", "i686") and not shutil.which("nasm"):
        raise RuntimeError("optimized x86 media build requires the build-only NASM assembler; "
                           "portable no-assembly candidates are not cleared for 1080p30 release")
    base.mkdir(parents=True, exist_ok=True)
    (base / "profile.json").unlink(missing_ok=True)
    if prefix.is_symlink():
        raise RuntimeError("dependency prefix must not be a symlink")
    if prefix.exists():
        shutil.rmtree(prefix)
    prefix.mkdir(parents=True, exist_ok=True)
    source_root = base / "sources"
    materials = base / "build-materials"
    materials.mkdir(exist_ok=True)
    shutil.copy2(LOCK, materials / LOCK.name)
    shutil.copy2(Path(__file__), materials / "deps-lgpl.py")
    tools = {}
    for tool in ("cc", "c++", "make", "pkg-config", "nasm", "readelf"):
        if shutil.which(tool):
            tools[tool] = subprocess.check_output([tool, "--version"], text=True).splitlines()[0]
    (materials / "tools.json").write_text(json.dumps(tools, indent=2) + "\n")
    commands = []
    environment = os.environ.copy()
    environment.update(PKG_CONFIG_PATH=str(prefix / "lib/pkgconfig"),
                       PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"),
                       LD_LIBRARY_PATH=str(prefix / "lib"))
    environment.pop("PKG_CONFIG_SYSROOT_DIR", None)
    # Origin-based dependency runpaths retain relocatability and LGPL replacement.
    environment["LDFLAGS"] = "-Wl,-rpath,'$$ORIGIN'"
    for source in lock["sources"]:
        name, version = source["name"], source["version"]
        destination = source_root / f"{name}-{version}"
        if destination.exists():
            shutil.rmtree(destination)
        source_root.mkdir(exist_ok=True)
        with tarfile.open(cache / source["archive"]) as tar:
            tar.extractall(source_root, filter="data")
        if not destination.is_dir():
            raise RuntimeError(f"archive has unexpected source root for {name}")
        notices = materials / "licenses" / name
        notices.mkdir(parents=True, exist_ok=True)
        for license_file in source["license_files"]:
            shutil.copy2(destination / license_file, notices / Path(license_file).name)
        if name == "ffmpeg":
            (notices / "IJG.txt").write_text(
                "This software is based in part on the work of the Independent JPEG Group.\n"
                "FFmpeg libavcodec/jfdctfst.c, jfdctint_template.c and jrevdct.c are used "
                "without local modifications; see their original notices in the corresponding sources.\n")
            # Build after all three pinned external shared dependencies.
            continue
        if name == "zlib":
            steps = [["./configure", "--shared", f"--prefix={prefix}"],
                     ["make", f"-j{args.jobs}"], ["make", "install"]]
        elif name == "openssl":
            steps = [["./config", "shared", "no-tests", "no-module", "no-comp", "no-zlib",
                      f"--prefix={prefix}", "--libdir=lib", "--openssldir=/etc/ssl",
                      "-Wl,-rpath,'$$ORIGIN'"], ["make", f"-j{args.jobs}"], ["make", "install_sw"]]
        elif name == "openh264":
            settings = [f"PREFIX={prefix}", "USE_ASM=Yes", "HAVE_GMP_API=No", "HAVE_GTEST=No",
                        "LDFLAGS=-Wl,-rpath,'$$ORIGIN'"]
            steps = [["make", f"-j{args.jobs}", "libraries", *settings],
                     ["make", "install-shared", *settings]]
        for argv in steps:
            commands.append({"source": name, "argv": argv})
            run(argv, destination, environment, materials / f"{name}-build.log")
    ffmpeg = source_root / "ffmpeg-9.0.2"
    flags = [f"--prefix={prefix}", *lock["ffmpeg_flags"],
             f"--extra-cflags=-I{prefix}/include", f"--extra-ldflags=-L{prefix}/lib -Wl,-rpath,'\\$\\$\\$\\$ORIGIN'"]
    for argv in [["./configure", *flags], ["make", f"-j{args.jobs}"], ["make", "install"]]:
        commands.append({"source": "ffmpeg", "argv": argv})
        run(argv, ffmpeg, environment, materials / "ffmpeg-build.log")
    shutil.copy2(ffmpeg / "ffbuild/config.mak", materials / "ffmpeg-config.mak")
    shutil.copy2(ffmpeg / "config.h", materials / "ffmpeg-config.h")
    (materials / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    (materials / "patches.json").write_text('{"schema":1,"patches":[]}\n')
    # Archive verified corresponding sources; original LGPL library code is public.
    bundle = base / "corresponding-sources.tar.gz"
    with tarfile.open(bundle, "w:gz") as tar:
        for source in lock["sources"]:
            tar.add(cache / source["archive"], arcname="upstream/" + source["archive"])
        tar.add(materials, arcname="build-materials")
    lib_files = [path for path in sorted((prefix / "lib").glob("*.so*"))
                 if path.is_file() and not path.is_symlink()]
    loader_environment = os.environ.copy()
    loader_environment.pop("LD_LIBRARY_PATH", None)
    for library in lib_files:
        dynamic = subprocess.check_output(["readelf", "-d", str(library)], text=True)
        if not any("$ORIGIN" in line and ("RUNPATH" in line or "RPATH" in line)
                   for line in dynamic.splitlines()):
            raise RuntimeError(f"missing relocatable origin runpath: {library}")
        closure = subprocess.check_output(["ldd", str(library)], text=True, env=loader_environment)
        if "not found" in closure:
            raise RuntimeError(f"unresolved dependency in {library}")
        for line in closure.splitlines():
            if any(name in line for name in ("libavcodec", "libavformat", "libavutil", "libswscale",
                                            "libswresample", "libopenh264", "libssl", "libcrypto", "libz.so")):
                if "=>" in line:
                    resolved = Path(line.split("=>", 1)[1].strip().split()[0]).resolve()
                    if not resolved.is_relative_to(prefix / "lib"):
                        raise RuntimeError(f"media dependency escaped controlled tree: {line.strip()}")
    libs = {str(path.relative_to(base)): digest(path) for path in lib_files}
    profile = {"schema": 1, "profile": "lgpl", "release_eligible": True,
               "configure_flags": flags, "libraries": libs, "sources": lock["sources"],
               "build_materials": "build-materials", "corresponding_sources": bundle.name,
               "corresponding_sources_sha256": digest(bundle), "patches": []}
    (base / "profile.json").write_text(json.dumps(profile, indent=2) + "\n")
    print(f"Pinned shared LGPL media tree: {prefix}")
    print("Run actual recording/stream/TLS checks and dependency audit before staging any release.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"deps-lgpl: {error}")
