#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")/../.."
: "${RELEASE_VERSION:?RELEASE_VERSION is required}"

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config meson autoconf automake libtool curl xz-utils bzip2 libssl-dev libdbus-1-dev libglib2.0-dev libexpat1-dev libfontconfig1-dev libfreetype-dev libgl-dev libegl-dev libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev libsm-dev libice-dev libxcb1-dev libxcb-cursor-dev libxcb-glx0-dev libxcb-keysyms1-dev libxcb-image0-dev libxcb-shm0-dev libxcb-icccm4-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-shape0-dev libxcb-randr0-dev libxcb-render-util0-dev libxcb-xinerama0-dev libxcb-xkb-dev libxkbcommon-dev libxkbcommon-x11-dev

root="$PWD"
sources="$root/build/release-sources"
deps="$root/build/release-deps"
mkdir -p "$sources" "$deps" build/release-logs dist/release
fetch() {
  local url="$1" name="$2" checksum="$3"
  curl --fail --location --retry 2 --proto '=https' --output "$sources/$name" "$url"
  printf '%s  %s\n' "$checksum" "$sources/$name" | sha256sum --check -
}

# Official Qt/libplist releases and the upstream PipeWire GitHub mirror,
# pinned to the commit named by upstream tag 1.4.9.
fetch https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz qtbase.tar.xz 56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80
fetch https://github.com/libimobiledevice/libplist/releases/download/2.7.0/libplist-2.7.0.tar.bz2 libplist.tar.bz2 7ac42301e896b1ebe3c654634780c82baa7cb70df8554e683ff89f7c2643eb8b
fetch https://github.com/PipeWire/pipewire/archive/fd60e04525f3a04d90bf50085222e0cc9139b4a4.tar.gz pipewire.tar.gz b8bf4d425ed53f6b36ee91f8aa14aa146de85f92d256faf008fe176df154fd81
tar -xf "$sources/qtbase.tar.xz" -C "$sources"
tar -xf "$sources/libplist.tar.bz2" -C "$sources"
tar -xf "$sources/pipewire.tar.gz" -C "$sources"

# Build only the required Qt module, with system OpenSSL and X11 support.
# /usr/local is a CI-only dependency installation, never part of the payload.
cmake -S "$sources/qtbase-everywhere-src-6.8.3" -B "$deps/qt" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF -DFEATURE_testlib=OFF -DFEATURE_sql=OFF -DFEATURE_openssl_linked=ON -DFEATURE_xcb=ON -DFEATURE_dbus=ON
cmake --build "$deps/qt" --parallel 4
sudo cmake --install "$deps/qt"
(
  cd "$sources/libplist-2.7.0"
  ./configure --prefix=/usr/local --without-cython --without-tools --without-tests
  make -j4
  sudo make install
)
meson setup "$deps/pipewire" "$sources/pipewire-fd60e04525f3a04d90bf50085222e0cc9139b4a4" --prefix=/usr/local --libdir=lib --buildtype=release -Dauto_features=disabled -Dtests=disabled -Dinstalled_tests=disabled -Dexamples=disabled -Dsession-managers=[] -Dpipewire-jack=disabled -Dpipewire-v4l2=disabled -Dsystemd-user-service=disabled -Drlimits-install=false
meson compile -C "$deps/pipewire" -j4
sudo meson install -C "$deps/pipewire"
sudo ldconfig
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig"

{
  cat /etc/os-release
  c++ --version
  cmake --version
  pkg-config --modversion Qt6Core openssl libplist-2.0 libpipewire-0.3
  dpkg-query -W libstdc++6 libc6 libssl3t64
} > build/release-logs/dependencies.txt
cmake --preset linux-release -DBUILD_TESTING=OFF -DSMTG_RUN_VST_VALIDATOR=OFF -DAIRPLAY_RELEASE_VERSION="$RELEASE_VERSION" -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_INSTALL_RPATH= -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
cmake --build --preset linux-release --parallel 4
stage="$root/build/release-linux-stage"
cmake --install build/linux-release --prefix "$stage/install"
mkdir -p "$stage/AirPlayQt"
cp "$stage/install/bin/AirPlayQt" "$stage/AirPlayQt/AirPlayQt"
strip "$stage/AirPlayQt/AirPlayQt"
python3 scripts/release/linux_readme.py "$stage/AirPlayQt"
tar -czf dist/release/AirPlayQt-linux-NEED_DYLIB.tar.gz -C "$stage" AirPlayQt
tar -tzf dist/release/AirPlayQt-linux-NEED_DYLIB.tar.gz
python3 scripts/release/release.py record linux --directory dist/release --dependencies build/release-logs/dependencies.txt
