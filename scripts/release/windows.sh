#!/usr/bin/env bash
set -eo pipefail
cd "$(dirname "$0")/../.."
: "${CONDA:?setup-miniconda must provide CONDA}"
# Initialize the MSYS2 shell explicitly; do not depend on login profiles.
# Capture first so a failed hook cannot be hidden by eval's exit status.
conda_hook="$("$(cygpath -u "$CONDA")/Scripts/conda.exe" shell.bash hook)"
eval "$conda_hook"
conda activate release
set -u
: "${RELEASE_VERSION:?RELEASE_VERSION is required}"
: "${MSYS_ROOT:?MSYS_ROOT is required}"
root="$PWD"
build="$root/build/release-windows"
stage="$root/build/release-windows-stage"
output="$root/dist/release"
mkdir -p "$output" "$stage" build/release-logs

conda install --yes --override-channels --channel conda-forge 'qt6-main=6.8.3' openssl
prefix="$(cygpath -u "$CONDA_PREFIX")/Library"
plist="$(cygpath -u "$MSYS_ROOT")/clang64"
asio="$plist/include/asiosdk"
export PATH="$prefix/bin:$prefix/lib/qt6/bin:$PATH"
asio_package="$(pacman -Q mingw-w64-clang-x86_64-asiosdk)"
if [[ ! "$asio_package" =~ ^mingw-w64-clang-x86_64-asiosdk\ 2\.3\.4-[0-9]+$ ]]; then
  printf 'Expected MSYS2 ASIO SDK 2.3.4, got: %s\n' "$asio_package" >&2
  exit 1
fi
test -f "$asio/common/iasiodrv.h"
{
  conda list --explicit
  pacman -Q mingw-w64-clang-x86_64-libplist mingw-w64-clang-x86_64-asiosdk
  printf '%s\n' 'MSVC v143, Visual Studio 2026 x64'
  cmake --version
} > build/release-logs/dependencies.txt

# Use native Windows paths for CMake cache entries; /c/... paths must not
# leak into MSVC's generated project files or the deployment scripts.
cmake -S "$(cygpath -m "$root")" -B "$(cygpath -m "$build")" -G 'Visual Studio 18 2026' -A x64 -T v143 -DCMAKE_CONFIGURATION_TYPES=Release "-DCMAKE_PREFIX_PATH=$(cygpath -m "$prefix")" "-DOPENSSL_ROOT_DIR=$(cygpath -m "$prefix")" "-DLIBPLIST_ROOT=$(cygpath -m "$plist")" "-DASIO_SDK_ROOT=$(cygpath -m "$asio")" "-DCMAKE_IGNORE_PREFIX_PATH=$(cygpath -m "$plist")" -DBUILD_TESTING=OFF -DSMTG_RUN_VST_VALIDATOR=OFF -DAIRPLAY_BUILD_STANDALONE=ON -DAIRPLAY_BUILD_VST3=ON "-DAIRPLAY_RELEASE_VERSION=$RELEASE_VERSION"
cmake --build "$build" --config Release --parallel 4

# The existing runtime manifest is the application payload allowlist. Avoid
# copying import libraries, intermediate files or the separate VST3 engine.
app="$stage/AirPlayQt"
mkdir -p "$app"
(
  cd "$build/Release"
  sha256sum --check runtime-sha256.txt
  while IFS= read -r line; do
    relative="${line#*  }"
    mkdir -p "$app/$(dirname "$relative")"
    cp "$relative" "$app/$relative"
  done < runtime-sha256.txt
  cp qt.conf runtime-sha256.txt runtime-dependencies.txt "$app/"
)
plugin="$stage/AirPlayQt.vst3"
cp -R "$build/VST3/Release/AirPlayQt.vst3" "$plugin"
runtime="$plugin/Contents/x86_64-win/runtime"
(
  cd "$runtime"
  sha256sum --check runtime-sha256.txt
)
for file in "$app/AirPlayQt.exe" "$app/platforms/qwindows.dll" "$runtime/AirPlayQtEngine.dll" "$runtime/platforms/qwindows.dll" "$plugin/Contents/x86_64-win/AirPlayQt.vst3"; do
  test -s "$file"
done
python scripts/release/check_version.py windows "$stage" "$RELEASE_VERSION"

licenses="$app/ThirdPartyLicenses"
mkdir -p "$licenses"
# Use each installed Conda package's recorded source cache for its notices.
python - "$licenses" "$(cygpath -m "$CONDA_PREFIX")" <<'PY'
import json
from pathlib import Path
import shutil
import sys

destination = Path(sys.argv[1])
for metadata_path in (Path(sys.argv[2]) / 'conda-meta').glob('*.json'):
    metadata = json.loads(metadata_path.read_text(encoding='utf-8'))
    package = destination / (metadata['name'] + '-' + metadata['version'])
    package.mkdir()
    shutil.copy2(metadata_path, package)
    source = metadata.get('link', {}).get('source')
    if source and (Path(source) / 'info/licenses').is_dir():
        shutil.copytree(Path(source) / 'info/licenses', package / 'licenses')
PY
# MSYS2's libplist package does not install notices. Obtain them from the
# upstream source tag matching the installed C library, not a moving branch.
plist_version="$(sed -n 's/^Version: //p' "$plist/lib/pkgconfig/libplist-2.0.pc")"
[[ "$plist_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
mkdir -p "$licenses/libplist-$plist_version"
for name in COPYING COPYING.LESSER; do
  curl --fail --location --retry 2 --proto '=https' "https://raw.githubusercontent.com/libimobiledevice/libplist/$plist_version/$name" --output "$licenses/libplist-$plist_version/$name"
done
cp "$plist/share/licenses/mingw-w64-clang-x86_64-asiosdk/LICENSE" "$licenses/ASIO-SDK-2.3.4.txt"
cp -R "$licenses" "$plugin/Contents/Resources/ThirdPartyLicenses"
(
  cd "$stage"
  7z a -t7z "$output/AirPlayQt-windows.7z" AirPlayQt
  7z a -t7z "$output/AirPlayQt.vst3-windows.7z" AirPlayQt.vst3
)
7z t "$output/AirPlayQt-windows.7z"
7z t "$output/AirPlayQt.vst3-windows.7z"
python scripts/release/release.py record windows --directory "$output" --dependencies build/release-logs/dependencies.txt
