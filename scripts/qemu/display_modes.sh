# Sourced by run_qemu.sh — not executed on its own.
#
# Turns DISPLAY_MODE into QEMU display arguments, and says whether the mode needs a
# GL-capable GPU. Adding a display backend means adding a case arm here rather than
# copying a launcher.
#
# A GL mode this QEMU cannot serve is downgraded to its non-GL equivalent with a
# warning, so virgl is used wherever it is available and software rendering is what
# happens otherwise -- never a failure to start, and never a silent downgrade.
#
# The audio backend is picked from the host OS rather than being part of the mode.
# It had been part of the per-launcher copies this replaced: the plain VNC launcher
# asked for coreaudio and HVF while the virgl one asked for pipewire and KVM, so on
# Linux only one of the two could ever have worked. That was drift between copies, not
# a real difference between VNC modes.

# VIRGL says what to do about GL, independently of which display backend is used:
#
#   auto  (default) use GL if this QEMU can serve it. A mode named explicitly is
#         honoured as named -- auto decides the *default* mode, and downgrades a GL
#         mode the binary cannot serve, but never promotes a mode that was asked for.
#   on    use the GL variant of whatever mode is in play, promoting a non-GL one.
#   off   use the non-GL variant, so nothing touches the host GPU. This is the escape
#         hatch when GL is the thing being ruled out.
#
# Set by run_instance.sh's --gl / --no-gl, or in the environment for run_qemu.sh.
VIRGL="${VIRGL:-auto}"
case "$VIRGL" in
    auto|on|off) ;;
    *) echo "ERROR: VIRGL must be auto, on or off (got '$VIRGL')." >&2; exit 1 ;;
esac

# What this QEMU can actually serve. Asked twice -- once to pick the default mode,
# once to check the mode that was asked for -- and each answer costs a QEMU process,
# so the help text is read once and kept. Queried into variables and matched with a
# case glob rather than piped into grep: a `grep -q` at the end of a pipeline exits
# at the first match and kills the producer with SIGPIPE, which reports failure under
# `set -o pipefail`, so a match reads as a miss.
_qemu_help_read=0
_qemu_dev_help=""
_qemu_dpy_help=""
_qemu_read_help () {
    [ "$_qemu_help_read" -eq 1 ] && return 0
    _qemu_help_read=1
    [ -n "${QEMU_BIN:-}" ] || return 0
    _qemu_dev_help="$("$QEMU_BIN" -device help 2>/dev/null || true)"
    _qemu_dpy_help="$("$QEMU_BIN" -display help 2>/dev/null || true)"
}
# $1 is the -display backend a GL mode needs. Both halves have to be there: the
# device that hands guest GL to virglrenderer, and the backend that puts the result
# somewhere.
_qemu_serves_gl () {
    _qemu_read_help
    case "$_qemu_dev_help" in *virtio-gpu-gl-pci*) ;; *) return 1 ;; esac
    case "$_qemu_dpy_help" in *"$1"*) ;; *) return 1 ;; esac
    return 0
}

# The default mode is GL where the platform has one, because rendering on the host
# GPU is worth two orders of magnitude of frame time and the probe below turns it
# back off on a host that cannot do it. An instance.env that records a mode overrides
# this, which is why old instances keep whatever they were created with.
if [ -z "${DISPLAY_MODE:-}" ]; then
    case "$(uname -s)" in
        # macOS gets its GL through egl-vnc, not cocoa: UTM's ui/cocoa.m renders
        # virgl correctly but sizes the GL layer from the guest scanout divided by a
        # backingScaleFactor that can still read 1.0, so the image comes out
        # magnified and cropped on a Retina display and updateScale declines to
        # re-fit it. egl-vnc sidesteps that entirely -- the VNC client owns the
        # window. It needs the QEMU that scripts/build_scripts/build_virgl_qemu_macos.sh
        # builds, so a machine without it still gets a native cocoa window rather
        # than being pushed into a VNC-only session it gains nothing from.
        Darwin) if _qemu_serves_gl egl-headless; then
                    DISPLAY_MODE=egl-vnc
                else
                    DISPLAY_MODE=cocoa
                fi ;;
        *)      DISPLAY_MODE=sdl-gl ;;
    esac
fi

# egl-headless renders on a specific DRM node. renderD128 is not always the card you
# want — on a machine with an integrated and a discrete GPU it is usually the
# integrated one — so it is overridable.
#
# macOS has no DRM and no render nodes: there, egl-headless brings up ANGLE's EGL
# display, which is the GPU. QEMU still accepts a rendernode= it will ignore, but
# handing it a Linux device path that does not exist says something false about what
# the build is doing, so the option is left off entirely.
case "$(uname -s)" in
    Darwin) RENDERNODE="${RENDERNODE:-}" ;;
    *)      RENDERNODE="${RENDERNODE:-/dev/dri/renderD128}" ;;
esac

# Each mode carries the pair it belongs to, so switching GL on or off is a lookup
# rather than a second table: GL_MODE is the GL member of the pair (empty when the
# platform has none) and NOGL_MODE the other. GL_BACKEND is the -display backend
# QEMU must have been built with to serve the GL member.
_resolve_display_mode() {
    GL_BACKEND=""
    GL_MODE=""
    NOGL_MODE=""
    case "$DISPLAY_MODE" in
        sdl)     DISPLAY_ARGS="-display sdl,show-cursor=on";       NEEDS_GL=0
                 GL_MODE=sdl-gl;  NOGL_MODE=sdl ;;
        sdl-gl)  DISPLAY_ARGS="-display sdl,gl=on,show-cursor=on"; NEEDS_GL=1
                 GL_MODE=sdl-gl;  NOGL_MODE=sdl;  GL_BACKEND=sdl ;;
        # GL_MODE=egl-vnc is what gives --gl somewhere to go on macOS. It is not
        # cocoa's own GL path (-display cocoa,gl=es), which is the one that
        # mis-scales; the pair here is "native window, software" against "VNC,
        # virgl". --no-gl from egl-vnc lands on vnc rather than back on cocoa,
        # because both halves of that pair are reached the same way.
        cocoa)   DISPLAY_ARGS="-display cocoa,show-cursor=on";     NEEDS_GL=0
                 GL_MODE=egl-vnc; NOGL_MODE=cocoa ;;
        vnc)     DISPLAY_ARGS="-vnc :${VNC_DISPLAY:-1}";           NEEDS_GL=0
                 GL_MODE=egl-vnc; NOGL_MODE=vnc ;;
        # Renders with GL off-screen and serves the result over VNC. Note that HMP
        # `screendump` reports "no surface" under egl-headless, so take screenshots
        # with a VNC client rather than the monitor.
        egl-vnc) DISPLAY_ARGS="-display egl-headless${RENDERNODE:+,rendernode=$RENDERNODE} -vnc :${VNC_DISPLAY:-1}"
                 NEEDS_GL=1
                 GL_MODE=egl-vnc; NOGL_MODE=vnc; GL_BACKEND=egl-headless ;;
        none)    DISPLAY_ARGS="-display none";                     NEEDS_GL=0
                 NOGL_MODE=none ;;
        *)
            echo "ERROR: unknown DISPLAY_MODE '$DISPLAY_MODE'." >&2
            echo "       Known modes: sdl sdl-gl cocoa vnc egl-vnc none" >&2
            exit 1 ;;
    esac
}
_resolve_display_mode

# VIRGL=on/off moves within the pair before the capability probe runs, so an
# explicitly requested GL mode is still checked against what the binary can do.
if [ "$VIRGL" = off ] && [ "$NEEDS_GL" -eq 1 ]; then
    echo "--- VIRGL=off: using $NOGL_MODE instead of $DISPLAY_MODE, so GL stays on the guest CPU ---"
    DISPLAY_MODE="$NOGL_MODE"
    _resolve_display_mode
elif [ "$VIRGL" = on ] && [ "$NEEDS_GL" -eq 0 ]; then
    if [ -n "$GL_MODE" ]; then
        echo "--- VIRGL=on: using $GL_MODE instead of $DISPLAY_MODE ---"
        DISPLAY_MODE="$GL_MODE"
        _resolve_display_mode
    else
        echo "WARNING: VIRGL=on, but DISPLAY_MODE=$DISPLAY_MODE has no GL variant." >&2
        echo "         Use sdl-gl or egl-vnc to render on the host GPU." >&2
    fi
fi

# Whether this QEMU can serve a GL mode at all. Asking for one it cannot used to
# fail at startup with QEMU's own message about an unknown device, which does not
# say that the *binary* is the problem: Debian's arm64 package has neither
# virtio-gpu-gl-pci nor egl-headless, so the GL modes are simply unavailable on such
# a host however the instance is configured. Rather than stop, drop to the
# equivalent non-GL mode and say so -- loudly, because this project has lost enough
# time to software rendering that looked like hardware rendering.
#
# The help text itself is read by _qemu_serves_gl above, which also picked the
# default mode; this repeats the check only to name which half is missing.
if [ "$NEEDS_GL" -eq 1 ] && [ -n "${QEMU_BIN:-}" ] && ! _qemu_serves_gl "$GL_BACKEND"; then
    _missing=""
    case "$_qemu_dev_help" in *virtio-gpu-gl-pci*) ;; *) _missing="virtio-gpu-gl-pci" ;; esac
    case "$_qemu_dpy_help" in
        *"$GL_BACKEND"*) ;;
        *) _missing="${_missing:+$_missing or }$GL_BACKEND" ;;
    esac
    echo "WARNING: $QEMU_BIN has no $_missing, so DISPLAY_MODE=$DISPLAY_MODE" >&2
    echo "         cannot render on the host GPU. Falling back to" >&2
    echo "         DISPLAY_MODE=$NOGL_MODE, which rasterizes every frame on the" >&2
    echo "         *guest* CPU -- correct, and slower by orders of magnitude." >&2
    # The fix differs by host, and pointing macOS at BUILDING.md sent people to a
    # page that does not mention any of what they need.
    case "$(uname -s)" in
        Darwin)
            echo "         Build one with:  scripts/build_scripts/build_virgl_qemu_macos.sh" >&2
            echo "         (see docs/MACOS_VIRGL.md). Pass --no-gl to silence this." >&2 ;;
        *)
            echo "         Build QEMU with virglrenderer and OpenGL to get GL here; see" >&2
            echo "         docs/BUILDING.md. Pass --no-gl to silence this." >&2 ;;
    esac
    DISPLAY_MODE="$NOGL_MODE"
    _resolve_display_mode
fi

# Overridable, because the host's own audio stack is not something this can
# infer: a headless build host has no pipewire, and QEMU there refuses to start
# at all rather than falling back ("Unknown audio driver `pipewire'"). Set
# AUDIODEV_BACKEND=none to run without a host sink -- Engine still gets its card,
# so the guest side is unchanged.
case "$(uname -s)" in
    Darwin) AUDIODEV_BACKEND="${AUDIODEV_BACKEND:-coreaudio}"; AUDIODEV_ID="mac" ;;
    *)      AUDIODEV_BACKEND="${AUDIODEV_BACKEND:-pipewire}";  AUDIODEV_ID="host" ;;
esac
