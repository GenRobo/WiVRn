#!/usr/bin/env bash
# Rerun the WSL APK build and print only the errors. Run from WSL.
set -uo pipefail
root="$HOME/wivrn-android"
export ANDROID_HOME="$root/sdk" ANDROID_SDK_ROOT="$root/sdk" GRADLE_USER_HOME="$root/gradle-home"
export PATH="$root/env/bin:$PATH"
cd "$root/WiVRn-sensors-finish"
./gradlew --no-daemon -Pwivrn_version="${1:-dev}" assembleDebug 2>&1 | grep -E "error:|error C|FAILED|What went wrong|undefined|cannot find" -A3 | head -60
