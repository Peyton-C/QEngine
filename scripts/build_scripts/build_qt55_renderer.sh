#!/bin/bash
# Build the raster backend for SC5000 1.0.1-1.0.3 without replacing their Qt.
# Qt 5.5 has no VNC platform, and airQuick uses private APIs that rule out a Qt
# 5.9 runtime swap. Adapt Qt's GPL 5.7.1 software plugin to the firmware's 5.5
# headers, keep the backend name Engine selects, and expose offscreen captures.
#
# The caller extracts the target headers and libraries into <stage>/legacy.
# Host Qt is used only to build the original 5.5 moc generator. Neither host Qt
# nor the cross compiler's libc/libstdc++ is linked into the guest artifacts.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STAGE="${1:?stage directory required}"
STAGE="$(cd "$STAGE" && pwd)"
CACHE="$REPO_ROOT/build/qt55-renderer-armhf"
mkdir -p "$CACHE"

# Include the shim, adaptation and target ABI in the cache identity. --force on
# an instance still rebuilds anything affected by a source or firmware change.
FINGERPRINT="$(python3 -I - "$0" "$SCRIPT_DIR/patches/qt55-softwarecontext.patch" \
    "$REPO_ROOT/shims/rk3288/qt55vnc/qt55vnc.cpp" "$STAGE/legacy" <<'PY'
import hashlib, pathlib, sys
h = hashlib.sha256()
for name in sys.argv[1:-1]:
    h.update(pathlib.Path(name).read_bytes())
base = pathlib.Path(sys.argv[-1])
for group in ('lib', 'qt'):
    for path in sorted((base / group).glob('*.so')):
        h.update(path.name.encode())
        h.update(path.read_bytes())
for name in ('QtCore/qconfig.h', 'QtCore/qglobal.h'):
    h.update((base / 'include' / name).read_bytes())
print(h.hexdigest()[:24])
PY
)"
OUT="$CACHE/$FINGERPRINT"
if [ ! -s "$OUT/libsoftwarecontext.so" ] || [ ! -s "$OUT/qt55vnc.so" ]; then
    case "$(uname -m)" in
        arm64|aarch64) PLATFORM=linux/arm64 ;;
        *) PLATFORM=linux/amd64 ;;
    esac
    docker pull -q --platform "$PLATFORM" debian:bookworm >/dev/null
    docker run --rm -i --platform "$PLATFORM" -e FINGERPRINT="$FINGERPRINT" \
        -v "$STAGE:/stage:ro" -v "$CACHE:/cache" \
        -v "$SCRIPT_DIR:/scripts:ro" -v "$REPO_ROOT/shims:/shims:ro" \
        debian:bookworm bash -euo pipefail <<'BUILD'
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends g++ g++-arm-linux-gnueabihf make \
    qtbase5-dev libgles2-mesa-dev python3 curl ca-certificates xz-utils patch >/dev/null 2>&1
mkdir -p /cache/sources
cd /cache/sources
fetch() {
    local file="$1" digest="$2" url="$3"
    if ! printf '%s  %s\n' "$digest" "$file" | sha256sum -c - >/dev/null 2>&1; then
        curl -fL --retry 2 "$url" -o "$file.part"
        printf '%s  %s\n' "$digest" "$file.part" | sha256sum -c -
        mv "$file.part" "$file"
    fi
}
fetch qtbase-5.5.1.tar.xz dfa4e8a4d7e4c6b69285e7e8833eeecd819987e1bdbe5baa6b6facd4420de916 \
    https://download.qt.io/archive/qt/5.5/5.5.1/submodules/qtbase-opensource-src-5.5.1.tar.xz
fetch render2d-5.7.1.tar.xz 831913488bb887993ae8701e5966f53875667a774c0230fc5dc39d6077828c7f \
    https://download.qt.io/archive/qt/5.7/5.7.1/submodules/qtdeclarative-render2d-opensource-src-5.7.1.tar.xz
work="$(mktemp -d /tmp/qengine-qt55.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cd "$work"
tar -xf /cache/sources/qtbase-5.5.1.tar.xz
tar -xf /cache/sources/render2d-5.7.1.tar.xz
base="$work/qtbase-opensource-src-5.5.1"
renderer="$work/qtdeclarative-render2d-opensource-src-5.7.1"
patch -d "$renderer" -p1 < /scripts/patches/qt55-softwarecontext.patch
mkdir -p moc/private objects include
cp -r /usr/include/GLES2 /usr/include/KHR include/

# moc 5.15 emits a different plugin-metadata format. Build the 5.5 generator
# against host QtCore, retaining 5.5's flags/revision and target version stamp.
python3 - "$base" "$work/moc" <<'PY'
import pathlib, sys
base, out = map(pathlib.Path, sys.argv[1:])
p = base / 'src/tools/moc/generator.cpp'
p.write_text(p.read_text().replace('(int)QT_VERSION', '(int)0x050501'))
p = base / 'src/corelib/kernel/qmetaobject_p.h'
s = p.read_text().replace('#ifndef QT_NO_QOBJECT\n#include <private/qobject_p.h> // For QObjectPrivate::Connection\n#endif', '')
s = s[:s.index('extern int qMetaTypeTypeInternal')]
s += 'enum { MetaObjectPrivateFieldCount = 14 };\nstruct QMetaObjectPrivate { enum { OutputRevision = 7 }; };\nQT_END_NAMESPACE\n#endif\n'
(out / 'private/qmetaobject_p.h').write_text(s)
(out / 'private/qmetaobject_moc_p.h').write_bytes((base / 'src/corelib/kernel/qmetaobject_moc_p.h').read_bytes())
PY
cat > moc/moc55.pro <<EOF
QT = core
CONFIG += console c++11 release
CONFIG -= app_bundle
TARGET = moc55
INCLUDEPATH += .
include($base/src/tools/moc/moc.pri)
SOURCES += $base/src/tools/moc/main.cpp
EOF
(cd moc; qmake moc55.pro; make -j4)
inc=/stage/legacy/include
src="$renderer/src/plugins/scenegraph/softwarecontext"
flags=(-fPIC -O2 -std=c++11 -include limits -I"$inc" -I"$src" -I"$work/include" \
       -I"$work/objects" -I"$base/mkspecs/linux-g++")
for module in QtCore QtGui QtQuick QtQml QtNetwork; do
    flags+=(-I"$inc/$module" -I"$inc/$module/5.5.1" -I"$inc/$module/5.5.1/$module")
done
mocflags=()
for flag in "${flags[@]}"; do
    if [[ "$flag" = -I* ]]; then mocflags+=("$flag"); fi
done
cd objects
for header in context renderloop pixmaptexture softwarelayer pluginmain; do
    "$work/moc/moc55" "${mocflags[@]}" "$src/$header.h" -o "moc_$header.cpp"
done
for file in "$src"/*.cpp; do
    [[ "$file" = *threadedrenderloop.cpp ]] && continue
    arm-linux-gnueabihf-g++ "${flags[@]}" -w -c "$file" -o "$(basename "$file" .cpp).o"
done
for file in moc_*.cpp; do
    arm-linux-gnueabihf-g++ "${flags[@]}" -w -c "$file" -o "${file%.cpp}.o"
done
crtbegin="$(arm-linux-gnueabihf-gcc -print-file-name=crtbeginS.o)"
crtend="$(arm-linux-gnueabihf-gcc -print-file-name=crtendS.o)"
libs=(/stage/legacy/qt/libQt5Quick.so /stage/legacy/qt/libQt5Qml.so \
      /stage/legacy/qt/libQt5Gui.so /stage/legacy/qt/libQt5Core.so \
      /stage/legacy/lib/libstdc++.so /stage/legacy/lib/libc.so \
      /stage/legacy/lib/libm.so /stage/legacy/lib/libgcc_s.so)
arm-linux-gnueabihf-g++ -shared -nostdlib -Wl,-z,defs -o libsoftwarecontext.so \
    "$crtbegin" ./*.o "${libs[@]}" -lgcc "$crtend"
arm-linux-gnueabihf-g++ "${flags[@]}" -Wall -Wextra -c /shims/rk3288/qt55vnc/qt55vnc.cpp -o qt55vnc.o
arm-linux-gnueabihf-g++ -shared -nostdlib -Wl,-z,defs -o qt55vnc.so "$crtbegin" qt55vnc.o \
    "${libs[@]}" /stage/legacy/lib/libdl.so /stage/legacy/lib/libpthread.so -lgcc "$crtend"
mkdir -p "/cache/$FINGERPRINT"
cp libsoftwarecontext.so qt55vnc.so "/cache/$FINGERPRINT/"
BUILD
fi
cp "$OUT/libsoftwarecontext.so" "$OUT/qt55vnc.so" "$STAGE/legacy/"
