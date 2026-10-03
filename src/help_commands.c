#include "help_commands.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char cast_completion_bash_begin[], cast_completion_bash_end[];
extern const unsigned char cast_completion_zsh_begin[], cast_completion_zsh_end[];
extern const unsigned char cast_completion_fish_begin[], cast_completion_fish_end[];

static int fail(char *error, size_t size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
    return 1;
}
static int output(const char *text, char *error, size_t size)
{
    if (fputs(text, stdout) == EOF || fflush(stdout) == EOF) {
        return fail(error, size, "cannot write guide to standard output");
    }
    return 0;
}
static const char completion_usage[] =
    "Usage: cast completions [bash|zsh|fish]\n"
    "       cast completions --script bash|zsh|fish\n"
    "The default prints installation instructions for all three shells.\n"
    "--script prints the corresponding bundled completion script.\n";
static const char bash_guide[] =
    "Bash\n"
    "  For this terminal:\n"
    "    source <(cast completions --script bash)\n"
    "  For future terminals:\n"
    "    mkdir -p \"${XDG_DATA_HOME:-$HOME/.local/share}/cast\"\n"
    "    cast completions --script bash > \"${XDG_DATA_HOME:-$HOME/.local/share}/cast/cast.bash\"\n"
    "  Add this line to ~/.bashrc:\n"
    "    source \"${XDG_DATA_HOME:-$HOME/.local/share}/cast/cast.bash\"\n"
    "  Open a new Bash terminal, or run that source line now.\n\n";
static const char zsh_guide[] =
    "Zsh\n"
    "  Install the autoload file:\n"
    "    mkdir -p \"${XDG_DATA_HOME:-$HOME/.local/share}/zsh/site-functions\"\n"
    "    cast completions --script zsh > "
    "\"${XDG_DATA_HOME:-$HOME/.local/share}/zsh/site-functions/_cast\"\n"
    "  Add these lines to ~/.zshrc before your existing compinit call:\n"
    "    fpath=(\"${XDG_DATA_HOME:-$HOME/.local/share}/zsh/site-functions\" $fpath)\n"
    "  If your configuration does not initialize completion yet, also add:\n"
    "    autoload -Uz compinit\n"
    "    compinit\n"
    "  Open a new Zsh terminal. After adding this file to an existing completion\n"
    "  cache, run compinit once without its -C option to refresh the definitions.\n\n";
static const char fish_guide[] =
    "Fish\n"
    "  Run these commands in Fish:\n"
    "    mkdir -p \"$__fish_config_dir/completions\"\n"
    "    cast completions --script fish > \"$__fish_config_dir/completions/cast.fish\"\n"
    "    source \"$__fish_config_dir/completions/cast.fish\"\n"
    "  Fish automatically loads this file in future terminals.\n\n";
static const char setup_guide[] =
    "cast setup guide\n"
    "This command prints instructions; it changes no devices, modules or files.\n\n"
    "Install cast\n"
    "  Published binary packages target Arch Linux x86_64. Install the release\n"
    "  .pkg.tar.zst with pacman. The checked-in cast-git recipe is available at\n"
    "  packaging/aur/cast-git; from that directory, run makepkg -si.\n"
    "  Release packages: https://github.com/mattmezza/cast/releases\n"
    "  After downloading a release package, install its actual filename:\n"
    "    sudo pacman -U ./cast-VERSION-archlinux-x86_64.pkg.tar.zst\n"
    "  After cloning the source, a typical Arch build is:\n"
    "    sudo pacman -S --needed base-devel pkgconf ffmpeg pipewire libx11 libxext libxrandr libxi "
    "libxfixes libxcomposite glib2 sdl3 sdl3_ttf fontconfig freetype2 noto-fonts\n"
    "    make X11=1 WAYLAND=1 PANEL=1\n"
    "    sudo make X11=1 WAYLAND=1 PANEL=1 install\n"
    "  Other distributions need a source build with matching FFmpeg, PipeWire,\n"
    "  X11 and optional GLib/SDL3/SDL3_ttf development packages. Arch binaries\n"
    "  are not portable Debian/Ubuntu packages. PANEL=0 builds without SDL.\n\n"
    "Create your config without replacing existing edits\n"
    "  In Bash or Zsh:\n"
    "    cast_config_dir=\"${XDG_CONFIG_HOME:-$HOME/.config}/cast\"\n"
    "    mkdir -p \"$cast_config_dir\"\n"
    "    (set -C; cast config defaults > \"$cast_config_dir/cast.conf\")\n"
    "  If that file already exists, keep and edit it. Set [camera] device to\n"
    "  your physical camera and [output] device to a different loopback device.\n"
    "    cast config check\n\n"
    "Prepare a virtual camera\n"
    "  Install v4l2loopback and headers matching the running kernel exactly.\n"
    "  Arch with the linux kernel:\n"
    "    sudo pacman -S --needed v4l2loopback-dkms linux-headers v4l-utils\n"
    "  For linux-lts use linux-lts-headers; custom kernels need their own headers.\n"
    "  Debian/Ubuntu source-build prerequisite example:\n"
    "    sudo apt install v4l2loopback-dkms \"linux-headers-$(uname -r)\" v4l-utils\n"
    "  Pick a free device number, then load the module yourself:\n"
    "    sudo modprobe v4l2loopback devices=1 video_nr=10 card_label=cast exclusive_caps=1\n"
    "  This example creates /dev/video10. Existing loaded-module options do not\n"
    "  change when repeating modprobe. Keep your configuration's device in sync.\n"
    "  If loading fails, compare uname -r, dkms status and installed headers.\n"
    "  After a kernel upgrade, reboot into the matching kernel. Secure Boot\n"
    "  systems may require signing the DKMS module through their distro's setup.\n"
    "  Module reference: https://github.com/v4l2loopback/v4l2loopback\n\n"
    "Check device access as your normal user\n"
    "    v4l2-ctl --list-devices\n"
    "    ls -l /dev/video0 /dev/video10\n"
    "    id\n"
    "    cast doctor\n"
    "  The input camera must be readable and the loopback output writable. Use\n"
    "  your distribution's device ACL/udev policy. If the device's video group\n"
    "  grants the required access, an administrator can add your user to it:\n"
    "    sudo usermod -aG video \"$USER\"\n"
    "  Log out and back in after a group change. Run cast as your normal user.\n"
    "  Wayland also requires PipeWire, xdg-desktop-portal and your desktop's\n"
    "  portal backend; selecting a screen asks for portal consent.\n\n"
    "Start and verify the actual output\n"
    "  Start the foreground daemon in one terminal:\n"
    "    cast\n"
    "  It starts privacy-paused. In another terminal:\n"
    "    cast panel                 # optional PANEL=1 build\n"
    "    cast preview on            # Xorg local output preview\n"
    "    cast virtual resume\n"
    "    cast status\n"
    "    cast pause                 # neutral video and cast virtual-audio silence\n"
    "    cast virtual message \"Back in five minutes\"\n"
    "    cast quit                  # stop daemon and finalize recordings\n\n"
    "Select cast in the conference\n"
    "  Start the producer before opening or refreshing the conference camera\n"
    "  selector: exclusive_caps advertises capture only after a producer opens.\n"
    "  Choose the camera named cast; allow the browser/app's camera permission.\n"
    "  Video and audio are separate choices. Select your physical microphone,\n"
    "  or explicitly enable/select cast's optional virtual microphone. Pausing\n"
    "  cast does not mute a physical mic selected directly by the call app.\n"
    "  If readable screen text matters, ask participants to enlarge or pin the\n"
    "  camera tile; conference resolution and compression still apply.\n\n"
    "Streaming safely\n"
    "  Configure [stream] server_url and an absolute stream-key file path in cast.conf.\n"
    "  Create a file without typing the secret into a shell command (Bash/Zsh):\n"
    "    cast_key_file=\"$HOME/.config/cast/stream-key\"\n"
    "    (set -C; umask 077; : > \"$cast_key_file\")\n"
    "  If it exists, keep it. Open the file in your editor, paste one stream key,\n"
    "  save it, and run chmod 600 on its path. The key is a broadcast password;\n"
    "  permissions protect local access and do not encrypt the file.\n"
    "    cast config check\n"
    "    cast stream start          # connection starts in solid pause\n"
    "    cast stream status\n"
    "    cast stream resume         # deliberately reveal the composition\n"
    "    cast stream pause          # solid screen and silence, connection retained\n"
    "    cast stream stop           # service buffers may delay public broadcast ending\n"
    "  Recording, streaming, and virtual camera are independent; streaming can\n"
    "  run with --no-virtual and no loopback device. Setup: docs/streaming.md.\n\n"
    "Google Meet self-view\n"
    "  Meet can mirror its local self-view. That tile alone does not establish\n"
    "  the orientation another participant receives. cast keeps screen content\n"
    "  unmirrored; only the camera layer defaults to mirrored. Check cast's\n"
    "  output preview or a recording, then verify with another participant.\n"
    "    cast camera mirror off     # disable camera-layer mirroring for this session\n"
    "  Capture an application or a monitor without Meet's self-view to avoid\n"
    "  feeding the conference window back into its own camera tile.\n\n"
    "Shell completion: cast completions bash, cast completions zsh, or\n"
    "cast completions fish prints the corresponding installation instructions.\n";

int help_command(int argc, char **argv, char *error, size_t size)
{
    if (argc < 1 || (strcmp(argv[0], "setup") && strcmp(argv[0], "completions"))) {
        return -1;
    }
    if (!strcmp(argv[0], "setup")) {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            return output(
                "Usage: cast setup\nPrint installation, device and conference guidance.\n", error,
                size);
        }
        if (argc != 1) {
            return fail(error, size, "usage: cast setup");
        }
        return output(setup_guide, error, size);
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        return output(completion_usage, error, size);
    }
    bool script = argc > 1 && !strcmp(argv[1], "--script");
    if ((script && argc != 3) || (!script && argc != 1 && argc != 2)) {
        return fail(error, size, "usage: cast completions [bash|zsh|fish] or --script SHELL");
    }
    const char *shell = argc > 1 ? argv[script ? 2 : 1] : NULL;
    const unsigned char *begin = NULL, *end = NULL;
    const char *guide = NULL;
    if (shell) {
        if (!strcmp(shell, "bash")) {
            guide = bash_guide;
            begin = cast_completion_bash_begin;
            end = cast_completion_bash_end;
        } else if (!strcmp(shell, "zsh")) {
            guide = zsh_guide;
            begin = cast_completion_zsh_begin;
            end = cast_completion_zsh_end;
        } else if (!strcmp(shell, "fish")) {
            guide = fish_guide;
            begin = cast_completion_fish_begin;
            end = cast_completion_fish_end;
        } else {
            return fail(error, size, "unsupported shell %s; choose bash, zsh or fish", shell);
        }
    }
    if (script) {
        size_t length = (size_t)(end - begin);
        if (fwrite(begin, 1, length, stdout) != length || fflush(stdout) == EOF) {
            return fail(error, size, "cannot write completion script to standard output");
        }
        return 0;
    }
    if (guide) {
        return output(guide, error, size);
    }
    if (output(completion_usage, error, size) || output("\n", error, size) ||
        output(bash_guide, error, size) || output(zsh_guide, error, size) ||
        output(fish_guide, error, size)) {
        return 1;
    }
    return 0;
}
