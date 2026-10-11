# QuickShare for Linux

## Native application logs

The QuickShare GUI writes its native Abseil diagnostics to:

```text
$XDG_STATE_HOME/quickshare/logs/quickshare.log
```

If `XDG_STATE_HOME` is not set to an absolute path, the directory defaults to
`$HOME/.local/state/quickshare/logs`.

Records are stored exactly as Abseil emits them. The active log rotates at
10 MiB, with `quickshare.log.1` through `quickshare.log.4` retaining the four
previous files. The directory and its files are accessible only to the current
user.

After file logging initializes, native Abseil diagnostics are not duplicated
to stderr. If the file cannot be initialized or written, diagnostics fall back
to stderr. Qt and QML messages keep their existing behavior and are not written
to these files.

## Packaging

Packages are built in CI by `.github/workflows/package.yml` (manual dry run: `build.yml`,
tagged release: `release.yml`). Recipes live in `app/packaging/`. Locally:

```sh
# Plain build (Bazel for libquickshare.so, CMake for the Qt app)
sharing/linux/build.sh

# Flatpak (needs org.kde.Sdk//6.10 and the llvm20 SDK extension from Flathub)
flatpak-builder --user --force-clean --install-deps-from=flathub \
  --repo=/tmp/qs-repo /tmp/qs-build \
  sharing/linux/app/packaging/flatpak/io.github.quickshare_linux.QuickShare.yml

# Arch (builds the branch given by _commit, default: linux)
cd sharing/linux/app/packaging/arch && makepkg -si

# AppImage: CI only (Qt 6.10 from aqt + linuxdeploy; see the appimage job in package.yml)
```
