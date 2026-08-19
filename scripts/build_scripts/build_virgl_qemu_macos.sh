#!/bin/bash
# build_virgl_qemu_macos.sh — builds a macOS QEMU that can actually serve virgl, so
# an emulated Engine renders on the host GPU instead of rasterizing every frame on an
# emulated CPU. The host-side counterpart to build_virgl_mesa.sh, which builds the
# guest-side Mesa driver: both halves have to be present or the guest silently falls
# back to software.
#
# Usage: build_virgl_qemu_macos.sh [--stage <name>] [--targets <list>]
#                                  [--jobs <n>] [--force] [--skip-deps-check]
#   Everything is written under build/qemu-virgl/ — sources, build trees, logs and
#   the install prefix. Nothing is written to /opt/homebrew, no Homebrew formula is
#   shadowed, and the system qemu stays linked and untouched. Homebrew is used only
#   as a read-only source of ordinary libraries (glib, pixman, gnutls, libusb,
#   usbredir).
#
#   Stages, runnable individually to iterate:
#     deps fetch angle epoxy virgl qemu test env all   (default: all)
#   Each stage records what it was built from and is skipped on a re-run unless its
#   inputs changed or --force is given, the same way get_kernel.sh and
#   build_virgl_mesa.sh cache their artifacts. Budget 30-45 minutes cold, nearly all
#   of it ANGLE.
#
# Only aarch64-softmmu is built by default: arch_devices.sh prefers
# qemu-system-aarch64 for both arm64 and armhf guests (it offers cortex-a15/a7 and
# boots a 32-bit zImage), so one target covers every device family in this repo.
#
# The patches are tracked in patches/, not vendored source, and they are licensed
# separately from this repository — see patches/README.md
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES="$SCRIPT_DIR/patches"

# Under build/, like every other build artifact in this repo, and gitignored with it.
# A subdirectory rather than the root of build/ because this is a source tree and a
# prefix, not one output file: it grows to several GB.
OUT_DIR="$REPO_ROOT/build/qemu-virgl"
SRC="$OUT_DIR/src"
WORK="$OUT_DIR/work"
PREFIX="$OUT_DIR/prefix"
LOGS="$OUT_DIR/logs"
STAMPS="$OUT_DIR/stamps"

STAGE="all"
TARGETS="aarch64-softmmu"
JOBS="$(sysctl -n hw.ncpu)"
FORCE=0
SKIP_DEPS_CHECK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --stage) STAGE="$2"; shift 2 ;;
        --targets) TARGETS="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --skip-deps-check) SKIP_DEPS_CHECK=1; shift ;;
        *) echo "ERROR: unrecognized argument: $1" >&2; exit 1 ;;
    esac
done

# --- Pins ---------------------------------------------------------------------
# Commits, not branch names. utm-edition moves, and a build that tracks a moving
# branch cannot be reproduced later or matched to a binary that was shipped from it
# — which is also what "corresponding source" means if one ever is. Bump these
# deliberately, together, and re-run the test stage.
WEBKIT_REPO="https://github.com/utmapp/WebKit.git"
WEBKIT_COMMIT="ed78ab6e1a37f4f11583a0bd038f22ec91f3ff10"
# ANGLE is all this build wants out of a very large repository, so the checkout is
# sparse. Configurations and Tools/ccache are what its Xcode project reads.
WEBKIT_SUBDIRS="Source/ThirdParty/ANGLE Configurations Tools/ccache"
EPOXY_REPO="https://github.com/utmapp/libepoxy.git"
EPOXY_COMMIT="bf98587477fe68d07b93319ece7b40a7d0e2eabe"
VIRGL_REPO="https://github.com/utmapp/virglrenderer.git"
VIRGL_COMMIT="65cc14eb896f121ffc5130ce04815a923a03c41d"
QEMU_REPO="https://github.com/utmapp/qemu.git"
QEMU_COMMIT="7311c3651c3a2cbc3d32e6eae262c60339f28d79"   # utm-edition, QEMU 10.0.12

# ANGLE is built for a deployment target, and the floor moves with Xcode: UTM's own
# CI still says 11.0, which the macOS 27 SDK refuses outright ("The macOS deployment
# target is set to 11.0, but the range of supported deployment target versions is
# 12.0 to 27.0"). 12.0 is exactly that SDK's MinimumDeploymentTarget today, so it is
# the floor here — and rather than tabulate what each future SDK allows, ask the SDK
# and take whichever is higher. A newer Xcode that drops 12.0 then raises this on its
# own instead of failing 20 minutes into the angle stage.
MACOS_MIN="12.0"
resolve_macos_min () {
    local sdk_path sdk_min
    sdk_path="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null)" || return 0
    [ -f "$sdk_path/SDKSettings.plist" ] || return 0
    sdk_min="$(plutil -extract SupportedTargets.macosx.MinimumDeploymentTarget raw -o - \
               "$sdk_path/SDKSettings.plist" 2>/dev/null)" || return 0
    case "$sdk_min" in ''|*[!0-9.]*) return 0 ;; esac
    MACOS_MIN="$(printf '%s\n%s\n' "$MACOS_MIN" "$sdk_min" | sort -V | tail -1)"
}
resolve_macos_min

GREEN='\033[0;32m'; RED='\033[0;31m'; NC='\033[0m'
say () { printf "${GREEN}==> %s${NC}\n" "$*"; }
die () { printf "${RED}ERROR: %s${NC}\n" "$*" >&2; exit 1; }

# Our prefix must come first so the EGL-enabled libepoxy built here wins over
# Homebrew's libepoxy 1.5.10, which has no EGL support on macOS. Get this wrong and
# QEMU configures cleanly, builds, and reports "opengl is not available".
BREW="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$BREW/lib/pkgconfig:$BREW/share/pkgconfig"
export PATH="$BREW/bin:$PATH"

# --- Stage bookkeeping --------------------------------------------------------
# A stage's stamp holds a fingerprint of everything that would change its output —
# the pinned commit, the patch contents, and the prefix path where that path gets
# baked into a binary. Re-running is then cheap and re-running after a bump is not
# silently a no-op.
stamp_file () { echo "$STAMPS/$1"; }
stage_current () {
    local name="$1" id="$2" f
    f="$(stamp_file "$name")"
    [ "$FORCE" -eq 0 ] && [ -f "$f" ] && [ "$(cat "$f")" = "$id" ]
}
stage_record () { mkdir -p "$STAMPS"; printf '%s' "$2" > "$(stamp_file "$1")"; }
patch_sum () { shasum -a 256 "$1" | cut -c1-12; }

# --- Stage: deps --------------------------------------------------------------
stage_deps () {
    [ "$(uname -s)" = "Darwin" ] || die "this builds a macOS QEMU; on Linux use the distro's qemu, which already has virgl."
    [ "$(uname -m)" = "arm64" ] || die "Apple Silicon only — ANGLE is built -arch arm64 and hvf/virgl are untested elsewhere here."

    local missing_cmds="" missing_brew=""
    for c in git meson ninja pkg-config python3 rsync xcodebuild install_name_tool codesign; do
        command -v "$c" >/dev/null 2>&1 || missing_cmds="$missing_cmds $c"
    done
    for f in glib pixman gnutls libusb usbredir; do
        brew --prefix "$f" >/dev/null 2>&1 || missing_brew="$missing_brew $f"
    done
    if [ -n "$missing_brew" ]; then
        echo "ERROR: missing Homebrew libraries:$missing_brew" >&2
        echo "       brew install$missing_brew" >&2
        exit 1
    fi
    [ -z "$missing_cmds" ] || die "not on PATH:$missing_cmds (try: brew install meson ninja pkgconf, and xcode-select --install)"

    # Xcode 26+ splits the Metal compiler into a separately downloaded component, and
    # ANGLE's Xcode project needs it. Without this the failure arrives ~20 minutes
    # into the angle stage as an opaque shader-compilation error.
    xcrun --find metal >/dev/null 2>&1 || \
        die "the Metal toolchain is not installed. Run: xcodebuild -downloadComponent MetalToolchain"

    say "host looks fine: $(sw_vers -productVersion), $(xcodebuild -version | head -1), SDK $(xcrun --sdk macosx --show-sdk-version), deployment target $MACOS_MIN, $JOBS cpus"
}

# --- Stage: fetch -------------------------------------------------------------
clone_at () {
    local repo="$1" commit="$2" dir="$3" subdirs="${4:-}" skip_submodules="${5:-}"
    if [ ! -d "$dir/.git" ]; then
        say "Cloning $repo"
        if [ -n "$subdirs" ]; then
            git clone --filter=tree:0 --no-checkout "$repo" "$dir"
            git -C "$dir" sparse-checkout init
            git -C "$dir" sparse-checkout set $subdirs
        else
            git clone --filter=tree:0 "$repo" "$dir"
        fi
    fi
    say "Checking out ${commit:0:12} in $(basename "$dir")"
    # --hard, because the patch stages leave the tree dirty and a plain checkout of a
    # commit that is already HEAD would keep those edits. Each patch stage reverts
    # only the files it touches; this is the backstop for everything else.
    git -C "$dir" fetch --all --tags --filter=tree:0
    git -C "$dir" checkout --detach "$commit"
    git -C "$dir" reset --hard "$commit"
    # QEMU's roms/* submodules are firmware *sources* this never compiles (QEMU ships
    # prebuilt blobs in pc-bios/), and edk2 alone recurses into about a gigabyte of
    # nested submodules. configure still fetches the few subprojects it genuinely
    # needs.
    if [ -z "$skip_submodules" ]; then
        git -C "$dir" submodule update --init --recursive --filter=tree:0
    fi
}

stage_fetch () {
    local id="$WEBKIT_COMMIT $EPOXY_COMMIT $VIRGL_COMMIT $QEMU_COMMIT"
    if stage_current fetch "$id"; then say "fetch: already at the pinned commits"; return; fi
    mkdir -p "$SRC"
    clone_at "$WEBKIT_REPO" "$WEBKIT_COMMIT" "$SRC/WebKit" "$WEBKIT_SUBDIRS"
    clone_at "$EPOXY_REPO"  "$EPOXY_COMMIT"  "$SRC/libepoxy"
    clone_at "$VIRGL_REPO"  "$VIRGL_COMMIT"  "$SRC/virglrenderer"
    clone_at "$QEMU_REPO"   "$QEMU_COMMIT"   "$SRC/qemu" "" skip-submodules
    stage_record fetch "$id"
}

# --- Stage: angle -------------------------------------------------------------
# Built from WebKit's ThirdParty/ANGLE with xcodebuild, which is how UTM does it and
# avoids needing depot_tools/gn for upstream ANGLE.
stage_angle () {
    local patch="$PATCHES/angle-standalone-dylib-fallback.patch"
    local id="$WEBKIT_COMMIT $(patch_sum "$patch") $PREFIX $MACOS_MIN"
    if stage_current angle "$id"; then say "angle: up to date"; return; fi

    local angle_dir="$SRC/WebKit/Source/ThirdParty/ANGLE"
    [ -d "$angle_dir" ] || die "ANGLE source missing — run --stage fetch first"
    mkdir -p "$PREFIX/lib" "$PREFIX/include"

    say "Applying ANGLE standalone-dylib patch"
    apply_patch "$SRC/WebKit" "$patch"

    # env -i: xcodebuild inherits a surprising amount from an interactive shell
    # (SDKROOT and CPATH in particular) and misbuilds quietly when it does.
    # GCC_TREAT_WARNINGS_AS_ERRORS=NO: ANGLE builds with -Werror and clang 21 added
    # -Wunnecessary-virtual-specifier, which ANGLE trips over.
    say "Building ANGLE — this is the long one"
    ( cd "$angle_dir"
      env -i PATH="$PATH" xcodebuild archive \
          -archivePath "ANGLE" \
          -scheme "ANGLE" \
          -sdk macosx \
          -arch arm64 \
          -configuration Release \
          WEBCORE_LIBRARY_DIR="/usr/local/lib" \
          NORMAL_UMBRELLA_FRAMEWORKS_DIR="" \
          CODE_SIGNING_ALLOWED=NO \
          GCC_TREAT_WARNINGS_AS_ERRORS=NO \
          MACOSX_DEPLOYMENT_TARGET="$MACOS_MIN"
      rsync -a "ANGLE.xcarchive/Products/usr/local/lib/" "$PREFIX/lib"
      rsync -a "include/" "$PREFIX/include" )

    [ -f "$PREFIX/lib/libEGL.dylib" ] && [ -f "$PREFIX/lib/libGLESv2.dylib" ] || \
        die "ANGLE produced no libEGL.dylib/libGLESv2.dylib"

    # The archive ships install names that assume WebKit's framework layout
    # (@loader_path/../../../libEGL.dylib), which resolves to nothing once the
    # libraries sit in a plain lib directory.
    #
    # The obvious repair -- rewriting them to $PREFIX/lib/<name> -- is a trap.
    # install_name_tool can only grow a Mach-O's load commands into the headerpad
    # the linker happened to leave, so whether it succeeds depends on how long the
    # absolute path is. At 73 characters it fits; under this repo's build/qemu-virgl
    # prefix the same string is 81 and does not:
    #
    #   install_name_tool: changing install names or rpaths can't be redone for:
    #   .../prefix/lib/libEGL.dylib ... because larger updated load commands do not
    #   fit (the program must be relinked ...)
    #
    # -- which makes the build depend on where the repository is checked out. Every
    # rewrite below is to @rpath/<name>, which at 19 characters is shorter than
    # anything it replaces, so it fits by construction at any path depth. QEMU is
    # linked with -Wl,-rpath,$PREFIX/lib (see stage_qemu) and resolves them there;
    # libepoxy dlopen()s these by absolute path and never consults the id at all.
    say "Fixing ANGLE install names"
    for lib in "$PREFIX"/lib/libEGL.dylib "$PREFIX"/lib/libGLESv2.dylib; do
        [ -f "$lib" ] || continue
        install_name_tool -id "@rpath/$(basename "$lib")" "$lib"
        # Both shapes the archive can emit: absolute /usr/local/lib paths, and
        # framework-relative @loader_path/../.. ones.
        for dep in $(otool -L "$lib" | awk 'NR>1 {print $1}' |
                     grep -E '^(/usr/local/lib/|@loader_path/\.\.)' || true); do
            install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$lib"
        done
        # Editing a Mach-O invalidates its signature; macOS refuses to load it after.
        codesign --force --sign - "$lib" 2>/dev/null || true
    done

    # Prove the chain loads before spending half an hour on the layers above it.
    # "Error loading EGL entry points" is a *runtime* failure and nothing else in
    # this build can see it: QEMU's configure only checks headers and pkg-config, so
    # a libEGL that cannot find its GLES library still configures, builds and links
    # perfectly. eglGetProcAddress forces exactly the dlopen the standalone-dylib
    # patch repairs, through the same absolute-path entry libepoxy uses.
    say "Checking ANGLE loads standalone"
    mkdir -p "$WORK"
    cat > "$WORK/angle-probe.c" <<'PROBE'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    void *(*gpa)(const char *) = dlsym(h, "eglGetProcAddress");
    if (!gpa) { fprintf(stderr, "no eglGetProcAddress: %s\n", dlerror()); return 1; }
    return gpa("glGetString") ? 0 : 2;
}
PROBE
    cc -o "$WORK/angle-probe" "$WORK/angle-probe.c" || die "could not build the ANGLE probe"
    "$WORK/angle-probe" "$PREFIX/lib/libEGL.dylib" || \
        die "libEGL loaded but could not reach GLESv2 — the standalone-dylib patch did not take"
    stage_record angle "$id"
}

# --- Stage: epoxy -------------------------------------------------------------
stage_epoxy () {
    local id="$EPOXY_COMMIT $PREFIX"
    if stage_current epoxy "$id"; then say "epoxy: up to date"; return; fi

    # UTM's libepoxy fork adds EGL on macOS, but dlopen()s ANGLE as *frameworks*
    # (EGL.framework/Versions/Current/EGL) because UTM ships it inside an .app
    # bundle. We build plain dylibs for a CLI binary, so the dispatch table is
    # pointed at their absolute paths. Not a patch file: the path depends on where
    # this checkout lives, so there is nothing stable to diff against.
    say "Pointing libepoxy at our ANGLE dylibs"
    git -C "$SRC/libepoxy" checkout -- src/dispatch_common.c
    sed -i '' \
        -e "s|#define EGL_LIB \"EGL.framework/Versions/Current/EGL\"|#define EGL_LIB \"$PREFIX/lib/libEGL.dylib\"|" \
        -e "s|#define GLES2_LIB \"GLESv2.framework/Versions/Current/GLESv2\"|#define GLES2_LIB \"$PREFIX/lib/libGLESv2.dylib\"|" \
        -e "s|#define GLES1_LIB \"GLESv1_CM.framework/Versions/Current/GLESv1_CM\"|#define GLES1_LIB \"$PREFIX/lib/libGLESv2.dylib\"|" \
        "$SRC/libepoxy/src/dispatch_common.c"
    grep -q "define EGL_LIB \"$PREFIX/lib/libEGL.dylib\"" "$SRC/libepoxy/src/dispatch_common.c" \
        || die "failed to rewrite libepoxy's EGL_LIB path — did the fork change the macro?"

    say "Building libepoxy (EGL enabled)"
    rm -rf "$WORK/libepoxy"
    CFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
    meson setup "$WORK/libepoxy" "$SRC/libepoxy" \
        --prefix="$PREFIX" \
        --buildtype=release \
        -Dtests=false -Dglx=no -Degl=yes -Dx11=false
    meson compile -C "$WORK/libepoxy" -j "$JOBS"
    meson install -C "$WORK/libepoxy"

    # QEMU gates CONFIG_EGL on this header being reachable through epoxy's
    # pkg-config file. Its absence is the single most common cause of a QEMU that
    # builds fine and then says "opengl is not available".
    [ -f "$PREFIX/include/epoxy/egl.h" ] || die "epoxy/egl.h missing — EGL support did not build"
    say "epoxy $(pkg-config --modversion epoxy) from $(pkg-config --variable=prefix epoxy)"
    stage_record epoxy "$id"
}

# --- Stage: virgl -------------------------------------------------------------
stage_virgl () {
    local id="$VIRGL_COMMIT $PREFIX"
    if stage_current virgl "$id"; then say "virgl: up to date"; return; fi

    say "Building virglrenderer"
    rm -rf "$WORK/virglrenderer"
    # venus=false: Vulkan-on-Venus needs MoltenVK plus a loader and is alpha on
    # macOS. GL is what the guest Mesa driver from build_virgl_mesa.sh speaks.
    CFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
    meson setup "$WORK/virglrenderer" "$SRC/virglrenderer" \
        --prefix="$PREFIX" \
        --buildtype=release \
        -Dtests=false \
        -Dcheck-gl-errors=false \
        -Dvenus=false \
        -Drender-server-mode=process
    meson compile -C "$WORK/virglrenderer" -j "$JOBS"
    meson install -C "$WORK/virglrenderer"
    say "virglrenderer $(pkg-config --modversion virglrenderer)"
    stage_record virgl "$id"
}

# --- Stage: qemu --------------------------------------------------------------
# Reverts exactly the files a patch touches, then applies it. Idempotent across
# re-runs, and the file list comes from the patch itself so it cannot drift.
apply_patch () {
    local repo="$1" patch="$2" f found=0
    # Read one path per line rather than splitting a string on IFS: /bin/bash here is
    # 3.2, which has no mapfile, and a filename-splitting bug in a patch step is the
    # kind that shows up as a mysteriously stale build much later. Process
    # substitution, not a pipe, so `found` is set in this shell.
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        git -C "$repo" checkout -- "$f"
        found=1
    done < <(awk '/^\+\+\+ b\//{print substr($2,3)}' "$patch")
    [ "$found" -eq 1 ] || die "no target files found in $(basename "$patch")"
    git -C "$repo" apply "$patch" || die "$(basename "$patch") did not apply — the pinned commit may have moved"
}

stage_qemu () {
    local patch="$PATCHES/qemu-egl-headless-macos.patch"
    local id="$QEMU_COMMIT $(patch_sum "$patch") $TARGETS $PREFIX"
    if stage_current qemu "$id"; then say "qemu: up to date"; return; fi

    say "Applying QEMU egl-headless patch"
    apply_patch "$SRC/qemu" "$patch"

    # --disable-pvg: hw/display/apple-gfx.m uses ParavirtualizedGraphics PGTask_t,
    # which Apple obsoleted in the macOS 27 SDK, so it no longer compiles. It is a
    # different display device from virtio-gpu-gl and nothing here uses it. Guarded,
    # because the option only exists on QEMU builds that know about pvg.
    #
    # The help text goes to a file and grep reads that file, rather than the two
    # being piped together. A `grep -q` exits at its first match and SIGPIPEs
    # whatever feeds it, which under `set -o pipefail` reports the whole pipeline as
    # failed -- so a match can read as a miss. display_modes.sh and
    # build_virgl_mesa.sh both carry the same warning. Keeping the file also leaves
    # something to look at when the answer is surprising.
    #
    # Run from inside $WORK, because `configure --help` is not the read-only thing
    # it looks like: it probes the compiler first and drops config.log and
    # config-temp/ into the *current* directory. Invoked from the repo root -- which
    # is where anyone runs this script from -- that litters the top of the tree with
    # QEMU build artifacts that git then reports as untracked.
    local pvg_flag=""
    mkdir -p "$WORK/qemu-help"
    ( cd "$WORK/qemu-help" && "$SRC/qemu/configure" --help ) \
        > "$WORK/qemu-configure-help.txt" 2>/dev/null || true
    if grep -q '^  pvg' "$WORK/qemu-configure-help.txt"; then
        pvg_flag="--disable-pvg"
    fi
    say "pvg: ${pvg_flag:-not offered by this configure, leaving it alone}"

    say "Configuring QEMU ($TARGETS)"
    rm -rf "$WORK/qemu"
    mkdir -p "$WORK/qemu"
    # -Wl,-rpath: the binary has to find our libepoxy and libvirglrenderer at
    # runtime, and DYLD_LIBRARY_PATH cannot be used to do it. QEMU's meson
    # ad-hoc-signs qemu-system-* with com.apple.security.hypervisor so -accel hvf
    # works, and macOS strips DYLD_* from signed binaries — a wrapper script would
    # silently break either HVF or GL.
    ( cd "$WORK/qemu"
      "$SRC/qemu/configure" \
        --prefix="$PREFIX" \
        --target-list="$TARGETS" \
        --enable-cocoa \
        --enable-opengl \
        --enable-virglrenderer \
        --enable-hvf \
        --enable-slirp \
        --enable-libusb \
        --enable-usb-redir \
        --enable-curses \
        --disable-docs \
        --disable-gtk \
        --disable-sdl \
        $pvg_flag \
        --extra-cflags="-I$PREFIX/include" \
        --extra-ldflags="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" )

    # Fail here rather than 20 minutes later with a working binary that cannot do the
    # one thing it was built for. QEMU 10.x records these in config-host.h as bare
    # "#define CONFIG_FOO" with no trailing 1; virgl shows up as VIRGL_VERSION_MAJOR,
    # and the device itself is gated in hw/display/meson.build on virgl.found() and
    # opengl.found().
    say "Verifying feature detection"
    # Asked for and actually got are different questions. A build where --disable-pvg
    # did not take compiles ~1700 files before dying on apple-gfx.m, and the symbol
    # is absent from config-host.h either way, so the generated ninja graph is what
    # settles it.
    if [ -n "$pvg_flag" ] && grep -q "apple-gfx" "$WORK/qemu/build.ninja"; then
        die "--disable-pvg did not take: apple-gfx.m is still in the build graph, and it does not compile against the macOS 27 SDK"
    fi
    check_cfg () { grep -qE "^#define $1( |$)" "$WORK/qemu/config-host.h" 2>/dev/null; }
    check_cfg CONFIG_OPENGL       || die "OpenGL not enabled — our EGL-enabled libepoxy was not the one found; check PKG_CONFIG_PATH"
    check_cfg CONFIG_EGL          || die "CONFIG_EGL not set — epoxy/egl.h was not visible to QEMU"
    check_cfg VIRGL_VERSION_MAJOR || die "virglrenderer not detected"
    check_cfg CONFIG_METAL        || echo "WARNING: CONFIG_METAL not set — the Cocoa GL layer needs it" >&2
    check_cfg CONFIG_USB_LIBUSB   || echo "WARNING: libusb not found — USB passthrough unavailable" >&2
    check_cfg CONFIG_USB_REDIR    || echo "WARNING: usbredir not found — USB redirection unavailable" >&2

    say "Building QEMU with $JOBS jobs"
    ninja -C "$WORK/qemu" -j "$JOBS"
    ninja -C "$WORK/qemu" install
    stage_record qemu "$id"
}

# --- Stage: test --------------------------------------------------------------
# Assert what a working result must look like, in the same terms display_modes.sh
# probes for. A QEMU that lacks either of these does not fail loudly at boot: the
# launcher demotes the mode and the guest renders in software, which looks like
# success until frame times are measured.
stage_test () {
    local q="$PREFIX/bin/qemu-system-aarch64"
    [ -x "$q" ] || die "$q was not built"

    echo "--- $("$q" --version | head -1)"
    echo "--- linked GL libraries"
    otool -L "$q" | grep -iE "virgl|epoxy" | sed 's/^/    /' || \
        die "neither libvirglrenderer nor libepoxy is linked in"

    local dev_help dpy_help
    dev_help="$("$q" -device help 2>&1 || true)"
    dpy_help="$("$q" -display help 2>&1 || true)"
    case "$dev_help" in
        *virtio-gpu-gl-pci*) echo "--- virtio-gpu-gl-pci: present" ;;
        *) die "no virtio-gpu-gl-pci — the guest would get a plain virtio-gpu and render in software" ;;
    esac
    case "$dpy_help" in
        *egl-headless*) echo "--- egl-headless: present" ;;
        *) die "no egl-headless — the patch did not take, so DISPLAY_MODE=egl-vnc cannot work" ;;
    esac

    cat <<EOM

Built: $q

Use it with this repo's launchers by setting QEMU_BIN in the environment — it is
inherited through run_instance.sh's exec, which is why it works without editing the
launchers:

  QEMU_BIN=$q \\
    scripts/qemu/run_instance.sh <name> --display egl-vnc

To make it permanent for one instance, add an *exported* assignment to its
instance.env (run_instance.sh sources that file, so a bare assignment would not
reach run_qemu.sh):

  export QEMU_BIN=$q
  DISPLAY_MODE=egl-vnc

Then connect a VNC client to 127.0.0.1:590<VNC_DISPLAY>. Confirm the guest is
really on the GPU — glxinfo -B should name virgl, not llvmpipe or softpipe — and
compare against --no-gl: identical frame times mean nothing is reaching the host.
The guest also needs its own half of this, from build_virgl_mesa.sh.
EOM
}

show_env () {
    echo "REPO_ROOT=$REPO_ROOT"
    echo "OUT_DIR=$OUT_DIR"
    echo "PREFIX=$PREFIX"
    echo "PKG_CONFIG_PATH=$PKG_CONFIG_PATH"
    echo "TARGETS=$TARGETS"
    echo "MACOS_MIN=$MACOS_MIN"
    echo "JOBS=$JOBS"
    for s in fetch angle epoxy virgl qemu; do
        printf "stamp %-6s %s\n" "$s" "$(cat "$(stamp_file "$s")" 2>/dev/null || echo '-')"
    done
}

# --- Dispatch -----------------------------------------------------------------
mkdir -p "$OUT_DIR" "$LOGS"
case "$STAGE" in
    env) show_env; exit 0 ;;
esac

# Everything after this point is teed to a log, because the interesting failures in
# a 40-minute build scroll past long before anyone reads them.
LOG="$LOGS/$STAGE.log"
exec > >(tee "$LOG") 2>&1
say "logging to $LOG"

case "$STAGE" in
    deps)  stage_deps ;;
    fetch) stage_fetch ;;
    angle) stage_angle ;;
    epoxy) stage_epoxy ;;
    virgl) stage_virgl ;;
    qemu)  stage_qemu ;;
    test)  stage_test ;;
    all)
        [ "$SKIP_DEPS_CHECK" -eq 1 ] || stage_deps
        stage_fetch; stage_angle; stage_epoxy; stage_virgl; stage_qemu; stage_test ;;
    *) die "unknown stage: $STAGE (deps|fetch|angle|epoxy|virgl|qemu|test|env|all)" ;;
esac
