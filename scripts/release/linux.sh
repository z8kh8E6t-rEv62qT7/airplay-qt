#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
umask 022
cd "$(dirname "$0")/../.."
source scripts/release/logging.sh
: "${RELEASE_VERSION:?RELEASE_VERSION is required}"
: "${ARCH_IMAGE:?ARCH_IMAGE is required}"

release_phase dependencies
pacman -Syu --noconfirm --needed gcc binutils cmake ninja pkgconf git python qt6-base openssl libplist libpipewire
# The checkout belongs to the Ubuntu runner, while this disposable container runs as root.
git config --global --add safe.directory /workspace

root="$PWD"
mkdir -p build/release-logs dist/release

{
  printf 'Arch image: %s\n' "$ARCH_IMAGE"
  cat /etc/os-release
  c++ --version
  cmake --version
  pkg-config --modversion Qt6Core openssl libplist-2.0 libpipewire-0.3
  pacman -Q
} > build/release-logs/dependencies.txt
release_phase configure
cmake --preset linux-release -DBUILD_TESTING=OFF -DSMTG_RUN_VST_VALIDATOR=OFF -DAIRPLAY_RELEASE_VERSION="$RELEASE_VERSION" -DCMAKE_INSTALL_RPATH= -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
release_phase build
cmake --build --preset linux-release --parallel 4 --verbose
release_phase package
stage="$root/build/release-linux-stage"
cmake --install build/linux-release --prefix "$stage/install"
mkdir -p "$stage/AirPlayQt"
cp "$stage/install/bin/AirPlayQt" "$stage/AirPlayQt/AirPlayQt"
strip "$stage/AirPlayQt/AirPlayQt"
python3 scripts/release/linux_readme.py "$stage/AirPlayQt"
tar -czf dist/release/AirPlayQt-linux-NEED_DYLIB.tar.gz -C "$stage" AirPlayQt
release_phase verify-package
tar -tzf dist/release/AirPlayQt-linux-NEED_DYLIB.tar.gz
release_phase manifest
python3 scripts/release/release.py record linux --directory dist/release --dependencies build/release-logs/dependencies.txt
