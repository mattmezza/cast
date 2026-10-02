#!/usr/bin/env python3
"""Check output-only guides and the actual bundled completion programs."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


PROJECT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("CAST_HELP_TEST_BINARY", PROJECT / "cast")).resolve()
SCRIPTS = {"bash": "cast.bash", "zsh": "_cast", "fish": "cast.fish"}


def run(args, *, env=None, cwd=None, success=True):
    result = subprocess.run(args, env=env, cwd=cwd, text=True, capture_output=True, timeout=10)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="cast-help-test-") as directory:
    root = Path(directory)
    env = os.environ.copy()
    env.pop("XDG_RUNTIME_DIR", None)
    env.update(HOME=str(root / "missing-home"), XDG_CONFIG_HOME="relative-invalid",
               PATH=str(BINARY.parent) + os.pathsep + env["PATH"])

    def cast(*args, success=True, binary=BINARY):
        return run([str(binary), *args], env=env, cwd=root, success=success)

    initial_files = set(root.iterdir())
    setup = cast("setup").stdout
    assert "v4l2loopback-dkms" in setup and "linux-headers" in setup
    assert "exclusive_caps=1" in setup and "cast config defaults" in setup
    assert "Google Meet self-view" in setup and "cast camera mirror off" in setup
    assert "Arch Linux x86_64" in setup and "cast run" not in setup
    assert "packaging/aur/cast-git" in setup and "makepkg -si" in setup
    assert "AUR package" not in setup
    assert "normal user" in setup and "physical mic" in setup
    all_guides = cast("completions").stdout
    assert all(name in all_guides for name in ("Bash", "Zsh", "Fish"))
    assert "compinit" in all_guides and "fpath=" in all_guides
    assert "source <(cast completions --script bash)" in all_guides
    assert "$__fish_config_dir/completions" in all_guides
    for shell, filename in SCRIPTS.items():
        guide = cast("completions", shell).stdout
        assert f"--script {shell}" in guide and "Usage:" not in guide
        script = cast("completions", "--script", shell).stdout
        assert script == (PROJECT / "completions" / filename).read_text(), shell
    for args in (("setup", "extra"), ("completions", "powershell"),
                 ("completions", "bash", "extra"), ("completions", "--script"),
                 ("completions", "--script", "nope"),
                 ("completions", "--script", "bash", "extra")):
        rejected = cast(*args, success=False)
        assert not rejected.stdout and rejected.stderr, args
    # Local guides do not read a broken config or try to reach a daemon.
    cast("--config", str(root / "absent.conf"), "setup")
    cast("--socket", str(root / "absent.sock"), "completions", "bash")
    assert set(root.iterdir()) == initial_files, "guides wrote files"

    relocated = root / "relocated-cast"
    shutil.copy2(BINARY, relocated)
    for shell, filename in SCRIPTS.items():
        assert cast("completions", "--script", shell, binary=relocated).stdout == \
            (PROJECT / "completions" / filename).read_text()

    def bash_complete(*words):
        script = PROJECT / "completions" / "cast.bash"
        program = "source " + shlex.quote(str(script)) + "\n"
        program += "COMP_WORDS=(" + " ".join(shlex.quote(word) for word in words) + ")\n"
        program += f"COMP_CWORD={len(words) - 1}\n_cast_complete\n"
        program += 'printf "%s\\n" "${COMPREPLY[@]}"\n'
        return set(run(["bash", "--noprofile", "--norc", "-c", program], env=env,
                       cwd=root).stdout.splitlines()) - {""}

    run(["bash", "-n", str(PROJECT / "completions" / "cast.bash")])
    assert {"setup", "update", "completions", "panel", "--config"} <= bash_complete("cast", "")
    assert bash_complete("cast", "--backend", "") == {"xorg", "wayland", "synthetic"}
    assert {"pause", "resume", "freeze", "unfreeze", "message"} <= \
        bash_complete("cast", "--backend", "xorg", "live", "")
    assert {"top", "bottom", "left", "right"} <= bash_complete("cast", "camera", "anchor", "")
    assert {"cut", "freeze", "unfreeze", "blur"} <= bash_complete("cast", "record", "")
    assert bash_complete("cast", "live", "blur", "") == {"on", "off", "toggle"}
    assert bash_complete("cast", "record", "blur", "") == {"on", "off", "toggle"}
    assert bash_complete("cast", "camera", "mirror", "") == {"on", "off", "toggle"}
    assert bash_complete("cast", "camera", "aspect", "4", ":", "") == {"3"}
    assert bash_complete("cast", "annotations", "record", "keys", "") == {"on", "off"}
    assert bash_complete("cast", "preview", "target", "") == {"live", "record"}
    assert bash_complete("cast", "audio", "virtual", "") == {"on", "off", "toggle"}
    assert bash_complete("cast", "completions", "--script", "") == set(SCRIPTS)
    keys = bash_complete("cast", "settings", "")
    assert {"output.pause_title", "output.pause_subtitle", "output.blur_radius", "camera.border_color", "record.queue"} <= keys
    assert not any(key.startswith("preset.") for key in keys)
    assert bash_complete("cast", "settings", "camera.mirror", "") == {"true", "false"}
    assert bash_complete("cast", "--width", "") == set()
    assert bash_complete("cast", "camera", "position", "") == set()
    spaced = root / "config with space.conf"
    spaced.write_text("# completion file fixture\n")
    assert str(spaced) in bash_complete("cast", "--config", str(root / "config"))
    downloads = root / "downloads"
    downloads.mkdir()
    assert str(downloads) in bash_complete("cast", "update", "--download-only", str(root / "down"))

    if shutil.which("zsh"):
        script = PROJECT / "completions" / "_cast"
        run(["zsh", "-n", str(script)])
        # Stub the shell's candidate sink, preserving the real function's command parsing.
        def zsh_complete(*words):
            program = 'compadd() { shift; print -l -- "$@"; }\n'
            program += 'words=(' + " ".join(shlex.quote(word) for word in words) + ')\n'
            program += f"CURRENT={len(words)}\nsource " + shlex.quote(str(script)) + '\n'
            return set(run(["zsh", "-f", "-c", program], env=env,
                           cwd=root).stdout.splitlines()) - {""}

        assert {"setup", "update", "completions"} <= zsh_complete("cast", "")
        assert zsh_complete("cast", "--socket", "/tmp/local.sock", "capture", "fit", "") == \
            {"contain", "cover"}
        assert zsh_complete("cast", "camera", "aspect", "") == {"native", "16:9", "4:3", "1:1"}
        assert zsh_complete("cast", "completions", "--script", "") == set(SCRIPTS)
        init = 'fpath=(' + shlex.quote(str(PROJECT / "completions")) + ' $fpath)\n'
        init += 'autoload -Uz compinit\ncompinit -D -d ' + shlex.quote(str(root / "zcompdump"))
        init += '\n[[ $_comps[cast] == _cast ]]\n'
        run(["zsh", "-f", "-c", init], env=env, cwd=root)

    if shutil.which("fish"):
        script = PROJECT / "completions" / "cast.fish"
        run(["fish", "--no-execute", str(script)])
        program = 'source ' + shlex.quote(str(script)) + "; complete -C 'cast camera mirror '"
        candidates = run(["fish", "--no-config", "-c", program], env=env,
                         cwd=root).stdout.splitlines()
        assert {"on", "off", "toggle"} <= {line.split("\t")[0] for line in candidates}
    else:
        print("fish unavailable: syntax and runtime checks skipped")

print("help commands: strict grammar, local guides, relocated scripts, bash/zsh completion passed")
