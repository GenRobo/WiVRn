#!/usr/bin/env bash
# Build the headset APK from this checkout inside the WSL Android toolchain at ~/wivrn-android.
# Run from WSL: bash /mnt/d/github/WiVRn/tools/windows/build-apk-wsl.sh [debug|release] [version]
# WIVRN_SRC overrides the checkout (a worktree under /mnt/d/github/WiVRn.worktrees/<name>).
# The WSL tree (~/wivrn-android/WiVRn-sensors-finish) is a plain copy, not a git checkout: this
# script syncs the tracked source files of the current git branch into it and runs Gradle there.
# Line endings are pinned by .gitattributes (eol=lf for scripts and build files, binary for assets),
# so the copy needs no translation.
set -euo pipefail
variant="${1:-debug}"
src="${WIVRN_SRC:-/mnt/d/github/WiVRn}"
dst="$HOME/wivrn-android/WiVRn-sensors-finish"
root="$HOME/wivrn-android"

cd "$src"
# A worktree's .git file names its git dir with a Windows path, which only the Windows git resolves.
git rev-parse --git-dir >/dev/null 2>&1 && git=git || git=git.exe
version="${2:-$($git describe --always --dirty 2>/dev/null || echo dev)}"
# Sync tracked files only; keep the WSL tree's build directories and local.properties.
$git ls-files -z | rsync -a --files-from=- --from0 "$src/" "$dst/"
chmod +x "$dst/gradlew"

export ANDROID_HOME="$root/sdk" ANDROID_SDK_ROOT="$root/sdk"
export GRADLE_USER_HOME="$root/gradle-home"
export PATH="$root/env/bin:$root/sdk/cmdline-tools/latest/bin:$PATH"
cd "$dst"
./gradlew --no-daemon -Pwivrn_version="$version" "assemble${variant^}" 2>&1 | tail -5
ls -la --time-style=long-iso "build/outputs/apk/$variant/"*.apk
