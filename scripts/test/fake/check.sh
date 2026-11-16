#!/bin/bash
# check.sh [-H HOST] [-g] [-s] [-b] [-p PROBES] [NAME ...] : run the fake Engine
# in instances and report what the shims gave it.
#
#   -H HOST    run on that ssh destination, such as peyton@qengine-1.local
#   -g         run in the booted guest, not in a chroot of its rootfs image
#   -s         use shims built from the working tree, not the instance's own
#   -b         save the results as the baseline later runs are compared with
#   -p a,b     only these probes; `Engine --list` names them
#   NAME ...   instances; with none, every instance on the machine
#
# The fake runs inside the instance's own rootfs, so the guest's loader, libc,
# libasound and the shims as the builder installed them are what is tested, and
# with the environment of the unit the builder put LD_PRELOAD on. Only the
# application is replaced. See fake/main.c for what it does there.
#
# Two ways in. The default mounts rootfs.img read-only under a tmpfs overlay and
# chroots into it, which takes a few seconds, needs Linux and sudo, and leaves
# the image untouched. The kernel is then the host's, so /proc/interrupts and
# the sound cards are too. -g runs in a guest that boot.sh and sshd.sh have
# brought up, in place of the application's service, which is stopped for the
# run and started again after: slower to set up, and everything is as Engine
# finds it, QEMU's sound card included.
#
# -s is for trying a shim edit without rebuilding instances. build_shims.sh
# compiles the working tree's shims, and each one named in the unit's LD_PRELOAD
# is run from /run in place of the installed copy. Nothing in the rootfs changes.
#
# With -H the scripts, the fake binaries and any shims are copied to the host
# first, as deploy.sh copies an instance, and the results come back to
# $OUT/fake/results/HOST/. Everything is built here, where Docker is.
set -eu
source "$(dirname "$0")/../common.sh"
FAKE="$TEST_SCRIPTS/fake"

host=; guest_mode=0; tree_shims=0; save=; probes=; inner=
while [ $# -gt 0 ]; do
    case "$1" in
        -H) host=$2; shift 2 ;;
        -g) guest_mode=1; shift ;;
        -s) tree_shims=1; shift ;;
        -b) save=--save; shift ;;
        -p) probes=$2; shift 2 ;;
        # Set by this script on the far side of -H.
        --inner-run|--inner-libs) inner=$1; shift ;;
        -*) sed -n '2,10p' "$0" >&2; exit 2 ;;
        *) break ;;
    esac
done

# Copies out the libc an instance's shims have to be linked against, when it is
# older than glibc 2.34 and so not one bookworm can link for. As the legacy
# builders do it: debugfs does not follow symlinks, so the versioned names are
# picked, and glibc 2.34 stopped shipping them, which is also the test.
fetch_libs() {
    local name=$1 img="$ROOT/build/instances/$1/rootfs.img" dest="$OUT/fake/guestlib/$1"
    local debugfs lib file
    debugfs="$(command -v debugfs || ls /usr/sbin/debugfs /sbin/debugfs \
        /opt/homebrew/opt/e2fsprogs/sbin/debugfs 2>/dev/null | head -n 1)"
    rm -rf "$dest"
    [ -n "$debugfs" ] && [ -f "$img" ] || return 0
    for lib in libc libdl libpthread librt ld; do
        file=$("$debugfs" -R 'ls -p /usr/lib' "$img" 2>/dev/null |
            awk -F/ '$3 ~ /^100/ {print $6}' | grep -E "^$lib-2\.[0-9.]+\.so$" | head -n 1 || true)
        [ -n "$file" ] || { [ "$lib" != libc ] || return 0; continue; }
        mkdir -p "$dest"
        "$debugfs" -R "dump /usr/lib/$file $dest/$lib.so" "$img" >/dev/null 2>&1
    done
    cp "$ROOT/build/instances/$name/instance.env" "$dest/"
}

# One instance. Results go to stdout as TAP, everything else to stderr.
run_one() (
    name=$1; inst="$ROOT/build/instances/$name"
    [ -f "$inst/instance.env" ] || { echo "# no instance named $name"; exit 1; }
    arch=$(instance_var "$name" ARCH); device=$(instance_var "$name" DEVICE)
    code=$(instance_var "$name" PRODUCT_CODE)
    device=${device:-engine}
    bin="$OUT/fake/bin/$arch/Engine"
    [ -x "$bin" ] || { echo "# no fake Engine built for $arch; run fake/build.sh"; exit 1; }
    # Which dtshim table the builder compiled in for this family and architecture.
    case "$device:$arch" in
        mpc-sample:*) soc=rk3566 ;;
        *:arm64)      soc=rk3588 ;;
        *)            soc=rk3288 ;;
    esac

    work=; unit=; stopped=0; surface_pid=; dummy_loaded=0
    cleanup() {
        [ -z "$surface_pid" ] || sudo kill "$surface_pid" 2>/dev/null || true
        [ "$stopped" = 0 ] || T "systemctl start $unit" || true
        # The card stays busy for a moment after its last user exits.
        if [ "$dummy_loaded" = 1 ]; then
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                sudo modprobe -r snd-dummy 2>/dev/null && break
                sleep 0.3
            done
        fi
        if [ -n "$work" ]; then
            sudo umount -R "$work/root" 2>/dev/null || true
            sudo umount "$work/rw" "$work/ro" 2>/dev/null || true
            sudo rmdir "$work/root" "$work/rw" "$work/ro" "$work" 2>/dev/null || true
        fi
    }
    trap cleanup EXIT

    if [ "$guest_mode" = 1 ]; then
        port=$(instance_var "$name" SSH_PORT)
        T() { guest "$port" "$1"; }
        T true || { echo "# no guest on ssh port $port; boot.sh it and run sshd.sh"; exit 1; }
        mode="booted guest"
    else
        [ "$(uname -s)" = Linux ] || {
            echo "# a chroot needs Linux: pass -H HOST, or -g for a booted guest"; exit 1; }
        # A mounted filesystem QEMU is writing to reads back inconsistent.
        if pgrep -f "$inst/rootfs.img" >/dev/null; then
            echo "# $name is running; stop it, or pass -g to test inside it"; exit 1
        fi
        work=$(mktemp -d /tmp/qengine-fake.XXXXXX)
        mkdir "$work/ro" "$work/rw" "$work/root"
        # noload: a read-only mount would otherwise still replay the journal
        # into the image.
        sudo mount -o loop,ro,noload "$inst/rootfs.img" "$work/ro"
        sudo mount -t tmpfs tmpfs "$work/rw"
        sudo mkdir "$work/rw/upper" "$work/rw/work"
        sudo mount -t overlay overlay \
            -o "lowerdir=$work/ro,upperdir=$work/rw/upper,workdir=$work/rw/work" "$work/root"
        sudo mount -t proc proc "$work/root/proc"
        # Plain binds, not recursive ones, so unmounting cannot reach the
        # host's own mounts under /dev and /sys.
        sudo mount --bind /sys "$work/root/sys"
        sudo mount --bind /dev "$work/root/dev"
        sudo mount -t tmpfs tmpfs "$work/root/run"
        sudo mount -t tmpfs tmpfs "$work/root/tmp"
        T() { sudo chroot "$work/root" /bin/sh -c "$1"; }
        mode="chroot of rootfs.img"
    fi

    # The unit the builder put the shims on, whatever the application is:
    # engine, jp07, inmusic-mpc, acvs, evil and so on.
    conf=$(T 'grep -ls "^Environment=.*LD_PRELOAD" /etc/systemd/system/*.service \
        /etc/systemd/system/*.service.d/*.conf 2>/dev/null | head -n 1' || true)
    exports=
    if [ -n "$conf" ]; then
        case "$conf" in
            *.conf) unit=$(basename "$(dirname "$conf")" .d) ;;
            *)      unit=$(basename "$conf") ;;
        esac
        # An /etc unit replaces the vendor's; drop-ins add to whichever it is.
        exports=$(T "u=/etc/systemd/system/$unit; [ -f \$u ] || u=/usr/lib/systemd/system/$unit
            [ -f \$u ] || u=/lib/systemd/system/$unit
            cat \$u /etc/systemd/system/$unit.d/*.conf 2>/dev/null" | python3 "$FAKE/unitenv.py")
    fi
    T 'mkdir -p /run/qengine-fake/shims'

    # The working tree's shims, one for each the unit preloads that
    # build_shims.sh built: linked against this guest's libc where it needed
    # that, for its architecture and SoC otherwise.
    surface_bin=/root/midisurface
    swapped=
    if [ "$tree_shims" = 1 ]; then
        built="$OUT/fake/shims/guest/$name"
        [ -d "$built" ] || built="$OUT/fake/shims/$arch-$soc"
        for path in $(echo "$exports" | sed -n "s/^export LD_PRELOAD='\{0,1\}\([^']*\)'\{0,1\}$/\1/p" |
                tr ':' ' '); do
            file=$(basename "$path")
            [ -f "$built/$file" ] || continue
            T "cat > /run/qengine-fake/shims/$file && chmod 755 /run/qengine-fake/shims/$file" \
                < "$built/$file"
            exports=$(echo "$exports" | sed "/^export LD_PRELOAD=/s#$path#/run/qengine-fake/shims/$file#")
            swapped="$swapped $file"
        done
        if [ "$guest_mode" = 0 ] && [ -f "$built/midisurface" ]; then
            T 'cat > /run/qengine-fake/shims/midisurface &&
                chmod 755 /run/qengine-fake/shims/midisurface' < "$built/midisurface"
            surface_bin=/run/qengine-fake/shims/midisurface
            swapped="$swapped midisurface"
        fi
    fi

    # The application the fake stands in for, read as data and never run.
    case "$device" in
        engine)     candidates="/usr/Engine/Engine /usr/Engine/[A-Z][A-Z][0-9][0-9] /usr/JP07/JP07" ;;
        mpc)        candidates="/usr/bin/MPC" ;;
        drummodule) candidates="/usr/bin/DrumModule" ;;
        headrush)   candidates="/usr/Evil/Evil /usr/Looper/Looper /usr/bin/HV03" ;;
        *)          candidates="" ;;
    esac
    real=$(T "for f in $candidates; do [ -f \"\$f\" ] && { echo \"\$f\"; break; }; done" || true)
    surface=0
    if [ "$device" = engine ] && T 'test -x /root/midisurface'; then surface=1; fi

    # A card that grants Engine's 128-frame period, which the host's own will
    # not. The shim is pointed at it wherever the unit pointed it at card 0.
    if [ "$guest_mode" = 0 ] && [ "$device" = engine ]; then
        if ! grep -q '\[Dummy' /proc/asound/cards 2>/dev/null; then
            sudo modprobe snd-dummy 2>/dev/null && dummy_loaded=1
        fi
        dummy=$(awk '/\[Dummy/ {print $1; exit}' /proc/asound/cards 2>/dev/null || true)
        if [ -n "$dummy" ]; then
            case "$exports" in *ALSASHIM_CARD=*) exports="$exports
export ALSASHIM_CARD=$dummy" ;; esac
            exports="$exports
export FAKE_ALSA_CARD=$dummy"
        fi
    fi

    echo "# instance $name: $mode, unit ${unit:-none found}, $arch"
    echo "# shims: ${swapped:+from the working tree:$swapped; }the rest as installed"
    T 'cat > /run/qengine-fake/Engine && chmod 755 /run/qengine-fake/Engine' < "$bin"
    {
        echo "$exports"
        echo "export FAKE_EXPECT_SURFACE=$surface"
        echo "cd /"
        echo "exec /run/qengine-fake/Engine --app '$device' --soc '$soc' --code '$code'" \
             "--real '$real' --probes '$probes'"
    } | T 'cat > /run/qengine-fake/run.sh'

    if [ "$guest_mode" = 1 ]; then
        # The application holds the sound card and is what the surface is bound to.
        if [ -n "$unit" ]; then T "systemctl stop $unit"; stopped=1; fi
    elif [ "$surface" = 1 ]; then
        # What midisurface.service does. The fifo is held open so the surface
        # never reads end of file.
        T "mkfifo /run/midisurface.fifo
           (exec 3<>/run/midisurface.fifo
            exec $surface_bin --motor-off < /run/midisurface.fifo) \
               > /run/qengine-fake/midisurface.log 2>&1 &
           echo \$! > /run/qengine-fake/midisurface.pid"
        surface_pid=$(T 'cat /run/qengine-fake/midisurface.pid')
        sleep 1
    fi
    T 'sh /run/qengine-fake/run.sh'
)

case "$inner" in
    --inner-libs)
        for name in "$@"; do fetch_libs "$name"; done
        exit 0 ;;
    --inner-run)
        results="$OUT/fake/results"
        mkdir -p "$results"
        for name in "$@"; do
            echo "--- $name ---" >&2
            run_one "$name" > "$results/$name.tap" 2> "$results/$name.log" || true
        done
        exit 0 ;;
esac

if command -v docker >/dev/null; then "$FAKE/build.sh"; fi
flags=()
[ "$guest_mode" = 0 ] || flags+=(-g)
[ "$tree_shims" = 0 ] || flags+=(-s)
[ -z "$probes" ] || flags+=(-p "$probes")
remote="${QENGINE_REMOTE:-qengine}"
there="$remote/build/test/fake"
# on ARGS... : this script again, here or on the host.
if [ -n "$host" ]; then
    tag=${host#*@}
    ssh "$host" "mkdir -p $there/bin $there/results $there/shims $there/guestlib"
    rsync -a --delete "$ROOT/scripts/" "$host:$remote/scripts/"
    rsync -a "$OUT/fake/bin/" "$host:$there/bin/"
    # shellcheck disable=SC2029  # the arguments are meant to expand here
    on() { ssh "$host" "cd $remote && scripts/test/fake/check.sh $*"; }
    # shellcheck disable=SC2046,SC2029
    [ $# -gt 0 ] || set -- $(ssh "$host" "ls $remote/build/instances")
else
    tag=local
    on() { "$0" "$@"; }
    # shellcheck disable=SC2046
    [ $# -gt 0 ] || set -- $(ls "$ROOT/build/instances")
fi

if [ "$tree_shims" = 1 ]; then
    # Shims for an old glibc are linked against the instance's own, which has
    # to come from where the instance is. Keyed by name alone, so two hosts'
    # instances of one name are taken to hold the same firmware.
    on --inner-libs "$@"
    if [ -n "$host" ]; then rsync -a "$host:$there/guestlib/" "$OUT/fake/guestlib/"; fi
    "$FAKE/build_shims.sh"
    if [ -n "$host" ]; then rsync -a --delete "$OUT/fake/shims/" "$host:$there/shims/"; fi
fi

on --inner-run ${flags[@]+"${flags[@]}"} "$@"
mkdir -p "$OUT/fake/results/$tag"
for name in "$@"; do
    if [ -n "$host" ]; then
        rsync -a "$host:$there/results/$name.tap" "$host:$there/results/$name.log" \
            "$OUT/fake/results/$tag/"
    else
        mv "$OUT/fake/results/$name.tap" "$OUT/fake/results/$name.log" "$OUT/fake/results/$tag/"
    fi
done
python3 "$FAKE/report.py" "$OUT/fake/results/$tag" "$OUT/fake/baseline/$tag" $save "$@"
