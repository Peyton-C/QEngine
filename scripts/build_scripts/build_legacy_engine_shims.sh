#!/bin/bash
# Build the early Engine runtime against the libraries extracted from this firmware.
# Bookworm provides the compiler, not the guest ABI: linking its glibc into a
# preload makes SC5000 1.2.0's loader reject it before any interposition runs.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMG="${1:?rootfs image required}"
STAGE="${2:?stage directory required}"
DEBUGFS="$(command -v debugfs || echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs)"
mkdir -p "$STAGE/legacy/lib" "$STAGE/legacy/qt"

# debugfs dump does not follow symlinks. Select the real library names rather
# than accidentally passing the text of a libc.so.6 symlink to the linker.
dump_library() {
    local dir="$1" pattern="$2" dest="$3" name
    name="$("$DEBUGFS" -R "ls -p $dir" "$IMG" 2>/dev/null |
        awk -F/ '$3 ~ /^100/ {print $6}' | grep -E "$pattern" | sed -n '1p' || true)"
    [ -n "$name" ] || { echo "ERROR: no $pattern in $dir" >&2; exit 1; }
    "$DEBUGFS" -R "dump $dir/$name $dest" "$IMG" >/dev/null 2>&1
    [ -s "$dest" ] || { echo "ERROR: could not extract $dir/$name" >&2; exit 1; }
}
for lib in libc libdl libpthread; do
    dump_library /usr/lib "^$lib-[0-9.]+\\.so$" "$STAGE/legacy/lib/$lib.so"
done
dump_library /usr/lib '^libstdc\+\+\.so\.6\.[0-9.]+$' "$STAGE/legacy/lib/libstdc++.so"
for lib in Core Gui; do
    dump_library /usr/qt/lib "^libQt5$lib\\.so\\.5\\.[0-9.]+$" "$STAGE/legacy/qt/libQt5$lib.so"
done
# The SC media players, SC5000, SC5000M, SC6000 and SC6000M, switched to the
# ALSA device manager in 1.5.2/1.6.x. Their loopback capture stream and exact
# hardware-rate requests need the normal ALSA shim, linked to this guest's
# older libc rather than the modern builder's ABI; without it 1.6.x aborts on
# snd_pcm_state. Prime 2 and Prime 4 1.6.x run the same Engine binary but open
# their audio differently and start without it.
LEGACY_ALSA=0
if "$DEBUGFS" -R 'ls /usr/Engine' "$IMG" 2>/dev/null |
        tr -s ' ' '\n' | grep -Eq '^JP(07|08|13|14)$' &&
        "$DEBUGFS" -R 'stat /usr/qt/lib/libQt5Core.so.5.12.6' "$IMG" 2>/dev/null | grep -q 'Inode:'; then
    LEGACY_ALSA=1
    dump_library /usr/lib '^librt-[0-9.]+\.so$' "$STAGE/legacy/lib/librt.so"
fi
"$DEBUGFS" -R "rdump /usr/qt/include $STAGE/legacy" "$IMG" >/dev/null 2>&1
# Engine 1.5.2 and 1.6.0 omit their Qt 5.12.6 headers. SC5000 1.4.1's
# Qt 5.12.4 headers work with these guest libraries; no host Qt is linked.
if [ ! -s "$STAGE/legacy/include/QtCore/qglobal.h" ] && [ -n "${LEGACY_QT_HEADERS:-}" ]; then
    header_version=$(sed -n 's/^#define QT_VERSION_STR "\([^"]*\)"/\1/p' "$LEGACY_QT_HEADERS/QtCore/qconfig.h")
    [ -n "$header_version" ] || { echo "ERROR: supplied Qt headers have no version." >&2; exit 1; }
    qt_minor=${header_version%.*}
    "$DEBUGFS" -R "stat /usr/qt/lib/libQt5Core.so.$qt_minor" "$IMG" 2>/dev/null | grep -q 'Inode:' || {
        echo "ERROR: supplied Qt $header_version headers do not match the guest Qt minor version." >&2; exit 1; }
    mkdir -p "$STAGE/legacy/include"
    cp -R "$LEGACY_QT_HEADERS/." "$STAGE/legacy/include/"
fi
[ -s "$STAGE/legacy/include/QtCore/qglobal.h" ] || {
    echo "ERROR: this legacy firmware has no Qt development headers." >&2; exit 1; }

docker pull -q --platform linux/arm/v7 debian:bookworm >/dev/null
docker run --rm --platform linux/arm/v7 \
    -e LEGACY_ALSA="$LEGACY_ALSA" \
    -v "$REPO_ROOT/shims:/shims:ro" -v "$STAGE:/stage" debian:bookworm bash -c '
    set -euo pipefail
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev >/dev/null 2>&1
    gcc -fPIC -O2 -Wall -DSOC_RK3288 -c /shims/dtshim/dtshim.c -o /stage/legacy/dtshim.o
    gcc -fPIC -O2 -Wall -c /shims/dtshim/legacy_compat.c -o /stage/legacy/compat.o
    gcc -shared -nostdlib -Wl,-z,defs -o /stage/legacy/dtshim.so \
        /stage/legacy/dtshim.o /stage/legacy/compat.o -lgcc \
        /stage/legacy/lib/libc.so /stage/legacy/lib/libdl.so /stage/legacy/lib/libpthread.so
    g++ -fPIC -O2 -Wall -std=c++17 \
        -I/stage/legacy/include -I/stage/legacy/include/QtCore -I/stage/legacy/include/QtGui \
        -c /shims/qmlrotate/qmlrotate.cpp -o /stage/legacy/qmlrotate.o
    g++ -shared -nostdlib -Wl,-z,defs -o /stage/legacy/qmlrotate.so \
        /stage/legacy/qmlrotate.o /stage/legacy/qt/libQt5Gui.so /stage/legacy/qt/libQt5Core.so \
        /stage/legacy/lib/libstdc++.so /stage/legacy/lib/libc.so -lgcc
    # This helper does not link Qt or ALSA. Static linkage keeps the host
    # toolchain runtime out of a guest with glibc 2.25.
    gcc -static -O2 -Wall -pthread -o /stage/legacy/vnctouchbridge \
        /shims/rk3288/vnctouchbridge/vnctouchbridge.c
    if [ "$LEGACY_ALSA" = 1 ]; then
        gcc -fPIC -O2 -Wall -c /shims/alsashim/alsashim.c -o /stage/legacy/alsashim.o
        gcc -shared -nostdlib -Wl,-z,defs -o /stage/legacy/alsashim.so \
            /stage/legacy/alsashim.o /stage/legacy/lib/libc.so \
            /stage/legacy/lib/libdl.so /stage/legacy/lib/libpthread.so \
            /stage/legacy/lib/librt.so -lgcc
    fi
    '

# SC5000 1.0.x predates Qt's VNC platform and integrated software renderer.
# Keep its private Qt/airQuick ABI intact and add a matching raster plugin.
if "$DEBUGFS" -R 'stat /usr/qt/lib/libQt5Core.so.5.5.1' "$IMG" 2>/dev/null | grep -q 'Inode:'; then
    for lib in Quick Qml; do
        dump_library /usr/qt/lib "^libQt5$lib\\.so\\.5\\.5\\.1$" "$STAGE/legacy/qt/libQt5$lib.so"
    done
    dump_library /usr/lib '^libm-[0-9.]+\.so$' "$STAGE/legacy/lib/libm.so"
    dump_library /usr/lib '^libgcc_s\.so\.1$' "$STAGE/legacy/lib/libgcc_s.so"
    "$SCRIPT_DIR/build_qt55_renderer.sh" "$STAGE"
fi
