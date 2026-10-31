#!/usr/bin/env bash
# Build the headset APK from this checkout inside the WSL Android toolchain at ~/wivrn-android.
# Run from WSL: bash /mnt/d/github/WiVRn/tools/windows/build-apk-wsl.sh [debug|release] [version]
# The WSL tree (~/wivrn-android/WiVRn-sensors-finish) is a plain copy, not a git checkout: this
# script syncs the tracked source files of the current git branch into it and runs Gradle there.
# Line endings are pinned by .gitattributes (eol=lf for scripts and build files, binary for assets),
# so the copy needs no translation.
set -euo pipefail
variant="${1:-debug}"
version="${2:-$(cd /mnt/d/github/WiVRn && git describe --always --dirty 2>/dev/null || echo dev)}"
src=/mnt/d/github/WiVRn
dst="$HOME/wivrn-android/WiVRn-sensors-finish"
root="$HOME/wivrn-android"

cd "$src"
# Sync tracked files only; keep the WSL tree's build directories and local.properties.
git ls-files -z | rsync -a --files-from=- --from0 "$src/" "$dst/"
chmod +x "$dst/gradlew"

export ANDROID_HOME="$root/sdk" ANDROID_SDK_ROOT="$root/sdk"
export GRADLE_USER_HOME="$root/gradle-home"
export PATH="$root/env/bin:$root/sdk/cmdline-tools/latest/bin:$PATH"
cd "$dst"
./gradlew --no-daemon -Pwivrn_version="$version" "assemble${variant^}" 2>&1 | tail -5
ls -la --time-style=long-iso "build/outputs/apk/$variant/"*.apk
