#!/usr/bin/env bash
# Builds the Windows installer from the current tree. Run it in an MSYS2
# UCRT64 shell with the build dependencies and Inno Setup installed:
#   ci/winpkg.sh [version]
# The result is packaging/windows/out/mvplayer-<version>-setup.exe.
set -euo pipefail

cd "$(dirname "$0")/.."
VER=${1:-$(sed -n 's/^set(MV_VERSION "\(.*\)" CACHE.*/\1/p' CMakeLists.txt)}
BUILD=${BUILD:-build}
STAGE=packaging/windows/stage

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMV_VERSION="$VER"
cmake --build "$BUILD"

rm -rf "$STAGE" packaging/windows/out
mkdir -p "$STAGE"
cp "$BUILD/mvplayer.exe" "$BUILD/mvplayer-import.exe" "$STAGE/"
# ffmpeg shares its libraries with libmpv, so the programs themselves are small.
cp "$MINGW_PREFIX/bin/ffmpeg.exe" "$MINGW_PREFIX/bin/ffprobe.exe" "$STAGE/"

# Qt's libraries, plugins and QML modules.
windeployqt6 --release --no-translations --qmldir qml "$STAGE/mvplayer.exe"

# MSYS2's Qt looks for QML modules under share/qt6; they are deployed to "qml".
printf '[Paths]
QmlImports = qml
' > "$STAGE/qt.conf"

# Everything else the staged binaries load from the MSYS2 prefix: libmpv and
# its dependencies, TagLib, Chromaprint, the compiler runtime.
PREFIX_WIN=$(cygpath -w "$MINGW_PREFIX")
find "$STAGE" \( -name '*.exe' -o -name '*.dll' \) -print0 | xargs -0 ntldd -R \
    | awk '$2 == "=>" { print $3 }' | grep -iF "$PREFIX_WIN" | sort -u \
    | while read -r dll; do
        cp -n "$(cygpath -u "$dll")" "$STAGE/"
    done

ISCC=$(command -v iscc || true)
for dir in "$(cygpath -F 28)/Programs/Inno Setup 6" "/c/Program Files (x86)/Inno Setup 6" "/c/Program Files/Inno Setup 6"; do
    [ -n "$ISCC" ] || { [ -x "$dir/ISCC.exe" ] && ISCC="$dir/ISCC.exe"; } || true
done
[ -n "$ISCC" ] || { echo "Inno Setup (ISCC.exe) not found" >&2; exit 1; }

# Keep MSYS from rewriting the /D switch as a path.
MSYS2_ARG_CONV_EXCL='*' "$ISCC" "/DAppVersion=$VER" packaging/windows/mvplayer.iss
ls -l packaging/windows/out
