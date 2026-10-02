# AUR packaging and first submission

Start with **cast-git**, using [the AUR recipe](../packaging/aur/cast-git/PKGBUILD)
and [its generated metadata](../packaging/aur/cast-git/.SRCINFO). It builds GitHub
`main` with Xorg, Wayland and the SDL3/Clay control panel enabled. The existing
`v0.1` release predates the panel; publishing that tag as a new panel release would
be misleading. A stable `cast` package can follow a future tagged release.
[Arch's VCS guidelines](https://wiki.archlinux.org/title/VCS_package_guidelines)
require the `-git` suffix for a moving Git source.

An [official AUR RPC lookup](https://aur.archlinux.org/rpc/v5/info?arg%5B%5D=cast&arg%5B%5D=cast-git)
on 2026-10-02 returned no packages named `cast` or `cast-git`. That is a point-in-time
check, not a reservation. Recheck before the first push.

## What the package contains

The package installs `/usr/bin/cast`, its manual, documentation, example configuration
and shortcuts, and license notices. It does not install a kernel module, create a
virtual camera, change a user's configuration, start a daemon or add a service.
Virtual camera use needs an existing v4l2loopback device; the DKMS module and matching
kernel headers are optional dependencies. Wayland screen sharing needs the desktop's
portal implementation. Runtime includes the distribution's dynamic `sdl3_ttf` library;
a local development SDK is not bundled into this package.

The recipe lists direct runtime libraries and enables `make check` with Python and
D-Bus as check dependencies. Its `pkgver()` turns a Git description such as
`v0.1-12-gabcdef0` into `0.1.r12.abcdef0`; commits after a tag increase the revision,
and the hash identifies the source. Package conflicts prevent simultaneous installation
with a future stable `cast`. The application still reports its upstream version;
the package's Git revision identifies the development build.

Source code is MIT, bundled inih is BSD-3-Clause, Clay is MIT, Inter is OFL-1.1 and
SDL3/SDL3_ttf use Zlib. The [Arch FFmpeg package](https://archlinux.org/packages/extra/x86_64/ffmpeg/)
currently declares GPL-3.0-only, so the recipe includes that conservative effective
binary license alongside the source/font notices. The project source remains MIT.
See [dependency and redistribution details](dependencies.md); the AUR publishes
build instructions and source references, rather than a binary distribution.

## Build and review before submitting

Use an ordinary user account on an up-to-date Arch system with `base-devel` installed.
Clone or copy the recipe into a separate working directory. Review `PKGBUILD`, then
let `makepkg` install missing declared dependencies and build/test/package:

```sh
mkdir -p ~/build/cast-git
cp /path/to/cast/packaging/aur/cast-git/PKGBUILD ~/build/cast-git/
cd ~/build/cast-git
makepkg --syncdeps --cleanbuild
makepkg --printsrcinfo > .SRCINFO
for package in cast-git-*.pkg.tar.zst; do bsdtar -tf "$package"; done
```

Dependency installation may ask for administrator access; compilation runs as your
normal user. To build without installing dependencies, preinstall them and omit
`--syncdeps`. Install the resulting package separately with `pacman -U` if desired.
Use `namcap PKGBUILD cast-git-*.pkg.tar.zst` when namcap is installed, and preferably
repeat the build in a clean Arch chroot before distribution. Arch's
[makepkg manual](https://man.archlinux.org/man/makepkg.8.en) describes these options.

Before publishing, ensure GitHub `main` contains the complete tested panel changes
and repeat the build against that public source. A local checkout or SDK validation
alone cannot show that another user's remote build will fetch those changes.

## Validation performed locally

On 2026-10-02, `makepkg` completed build, all `check()` tests and packaging for
committed source `3427382`, producing `cast-git-0.1.r8.3427382-1-x86_64.pkg.tar.zst`
and its debug package. The build used normal Arch makepkg hardening flags, including
Fortify 3 and LTO. Binary inspection confirmed dynamic `libSDL3_ttf.so.0` linkage,
no SDK RPATH and the expected manual, examples, documentation and license files.
`cast --version` and `--help` worked from the extracted package.

This was an isolated checkout on the development host, not a clean chroot. Since
SDL3_ttf was absent from the installed package database, the official Arch
`sdl3_ttf-3.2.2-3` package was signature-checked against the installed distribution's
public keyring and extracted into a private SDK prefix. `makepkg --noextract --nodeps`
used that prefix and a clone of the local committed source; it installed nothing
system-wide. The distributed recipe retains the public GitHub source and normal
system dependency. A public-source build with declared dependencies installed is
still required before the first AUR push. The aarch64 build was not exercised.

## Create the account and authorize SSH

Register at [AUR](https://aur.archlinux.org/register), verify the account's email,
then sign in. Add an SSH **public** key in the account's SSH Public Key field.
Keep the private key on your machine. If you need a dedicated key:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/aur -C 'AUR package maintenance'
cat ~/.ssh/aur.pub
```

Copy the `.pub` contents into the AUR account. Configure this host in `~/.ssh/config`,
using the key you registered:

```sshconfig
Host aur.archlinux.org
    User aur
    IdentityFile ~/.ssh/aur
    IdentitiesOnly yes
```

On first SSH connection, verify AUR's host fingerprint against the official
[submission instructions](https://wiki.archlinux.org/title/AUR_submission_guidelines#Authentication)
before accepting it. The SSH user is always `aur`; your public key identifies your
AUR account. A GitHub SSH login does not authorize AUR access.

## First package push

Keep the AUR packaging repository separate from the application repository. After
registering the key, clone the package's AUR repository; an empty-repository warning
is expected for a new package. AUR accepts package updates on `master`:

```sh
mkdir -p ~/aur
cd ~/aur
git -c init.defaultBranch=master clone ssh://aur@aur.archlinux.org/cast-git.git
cd cast-git
cp /path/to/cast/packaging/aur/cast-git/PKGBUILD .
cp /path/to/cast/packaging/aur/cast-git/.gitignore .
makepkg --syncdeps --cleanbuild
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO .gitignore
git diff --cached
git commit -m 'Add cast-git with Xorg, Wayland and control panel'
git push -u origin master
```

Check the resulting [package page](https://aur.archlinux.org/packages/cast-git),
its dependencies, version and GitHub link. The push publishes the recipe; neither
source code, built packages, SDK files nor your private SSH key belong in this
repository. The included `.gitignore` limits ordinary additions to recipe metadata.

If an agent helps publish, provide the AUR username and the local SSH identity path,
confirm that its public key is registered, and approve the final recipe and push.
Do not send the private key or account password in chat. Account registration and
email confirmation require the account owner.

## Maintenance

Keep this project's recipe and the AUR packaging repository aligned. Rebuild and
check after dependency or build changes. Update `pkgrel` for a packaging change
when `pkgver` stays the same; a new package version resets `pkgrel` to 1. A Git
package fetches new commits when users rebuild it, so ordinary upstream commits do
not each require a packaging push. Monitor AUR comments for build failures.

Regenerate `.SRCINFO` after metadata or generated version changes, stage both files,
review the diff, commit and push `master`. AUR reads the generated metadata;
editing only `PKGBUILD` leaves the web page stale. See the official
[.SRCINFO guide](https://wiki.archlinux.org/title/.SRCINFO).
