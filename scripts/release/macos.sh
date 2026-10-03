#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${RELEASE_VERSION:?RELEASE_VERSION is required}"
[[ "$(uname -m)" == arm64 ]]
[[ "$(sw_vers -productVersion)" == 27.* ]]
brew install cmake ninja pkg-config qt openssl@3 libplist sevenzip
mkdir -p build/release-logs dist/release
{
  sw_vers
  xcodebuild -version
  brew list --versions
} > build/release-logs/dependencies.txt
cmake --preset macos-release -DBUILD_TESTING=OFF -DSMTG_RUN_VST_VALIDATOR=OFF -DAIRPLAY_CLI_TEST=OFF -DAIRPLAY_RELEASE_VERSION="$RELEASE_VERSION"
cmake --build --preset macos-release --target package-macos
python3 scripts/release/check_version.py macos dist/macos-arm64 "$RELEASE_VERSION"
root="$PWD"
(
  cd dist/macos-arm64
  # Store symlinks as symlinks; framework signatures rely on that structure.
  7zz a -t7z -snl "$root/dist/release/AirPlayQt.app-macos.7z" AirPlayQt.app
  7zz a -t7z -snl "$root/dist/release/AirPlayQt.vst3-macos.7z" AirPlayQt.vst3
)
for archive in dist/release/*.7z; do
  7zz t "$archive"
  7zz x "$archive" "-o$root/build/release-macos-archive-check" -y
done
for bundle in AirPlayQt.app AirPlayQt.vst3; do
  codesign --verify --deep --strict "$root/build/release-macos-archive-check/$bundle"
  cmake "-DAUDIT_ROOT=$root/build/release-macos-archive-check/$bundle" "-DREPORT=$root/build/release-logs/archive-audit.txt" -P cmake/MacBundle.cmake
done
python3 scripts/release/release.py record macos --directory dist/release --dependencies build/release-logs/dependencies.txt
