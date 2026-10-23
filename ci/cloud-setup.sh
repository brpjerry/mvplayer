#!/usr/bin/env bash
# Setup script for a Claude Code cloud environment (Ubuntu 24.04, run as root):
# installs what building and ci/smoke.sh need. Paste it into the environment's
# Setup script, or run it by hand; running it twice does no harm.
#
# Ubuntu's own Qt (6.4) and TagLib (1.13) are older than the build needs
# (Qt 6.5, TagLib 2 for DSF and its VariantMap), so Qt comes from Qt's own
# mirrors through aqtinstall and TagLib is built from its release tarball.
# Afterwards:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ci/smoke.sh build
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

QT_VERSION=6.11.3
QT_DIR=/opt/qt/$QT_VERSION/gcc_64
TAGLIB_VERSION=2.1.1

apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    python3 python3-pip ffmpeg \
    libssl-dev libmpv-dev libpulse-dev libchromaprint-dev zlib1g-dev libutfcpp-dev \
    libgl-dev libegl-dev libvulkan-dev libxkbcommon-dev libfontconfig1-dev \
    libfreetype-dev libdbus-1-dev \
    libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1

# Qt, with Quick, Controls and Test (in the base download), ShaderTools, and
# TaskTree, which a QML plugin of Qt 6.11 looks for when the build configures.
if [[ ! -f $QT_DIR/lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake || ! -d $QT_DIR/lib/cmake/Qt6TaskTree ]]; then
    pip install --quiet --break-system-packages aqtinstall
    aqt install-qt --outputdir /opt/qt linux desktop "$QT_VERSION" linux_gcc_64 -m qtshadertools qttasktree
fi

# TagLib 2.
if [[ $(pkg-config --modversion taglib 2>/dev/null) != "$TAGLIB_VERSION" ]]; then
    src=$(mktemp -d)
    curl -fsSL "https://github.com/taglib/taglib/releases/download/v$TAGLIB_VERSION/taglib-$TAGLIB_VERSION.tar.gz" \
        | tar xz -C "$src" --strip-components=1
    cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
    cmake --build "$src/build"
    cmake --install "$src/build"
    ldconfig
    rm -rf "$src"
fi

# The current yt-dlp (Ubuntu's is from 2024; YouTube has moved on since).
curl -fsSL https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp -o /usr/local/bin/yt-dlp
chmod +x /usr/local/bin/yt-dlp

# Let CMake find this Qt in every shell, and give Qt the UTF-8 locale it wants.
cat > /etc/profile.d/mvplayer-qt.sh <<EOF
export CMAKE_PREFIX_PATH=$QT_DIR\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}
export PATH=$QT_DIR/bin:\$PATH
export LANG=C.UTF-8
EOF
grep -qxF '. /etc/profile.d/mvplayer-qt.sh' ~/.bashrc 2>/dev/null \
    || echo '. /etc/profile.d/mvplayer-qt.sh' >> ~/.bashrc

echo "Qt $QT_VERSION in $QT_DIR, TagLib $(pkg-config --modversion taglib), yt-dlp $(/usr/local/bin/yt-dlp --version)"
