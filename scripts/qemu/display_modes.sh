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

# The default mode is GL where the platform has one, because rendering on the host
# GPU is worth two orders of magnitude of frame time and the probe below turns it
# back off on a host that cannot do it. An instance.env that records a mode overrides
# this, which is why old instances keep whatever they were created with.
if [ -z "${DISPLAY_MODE:-}" ]; then
    case "$(uname -s)" in
        # cocoa has no GL variant here, so macOS defaults to it unchanged.
        Darwin) DISPLAY_MODE=cocoa ;;
        *)      DISPLAY_MODE=sdl-gl ;;
    esac
fi

# egl-headless renders on a specific DRM node. renderD128 is not always the card you
# want — on a machine with an integrated and a discrete GPU it is usually the
# integrated one — so it is overridable.
RENDERNODE="${RENDERNODE:-/dev/dri/renderD128}"

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
        cocoa)   DISPLAY_ARGS="-display cocoa,show-cursor=on";     NEEDS_GL=0
                 NOGL_MODE=cocoa ;;
        vnc)     DISPLAY_ARGS="-vnc :${VNC_DISPLAY:-1}";           NEEDS_GL=0
                 GL_MODE=egl-vnc; NOGL_MODE=vnc ;;
        # Renders with GL off-screen and serves the result over VNC. Note that HMP
        # `screendump` reports "no surface" under egl-headless, so take screenshots
        # with a VNC client rather than the monitor.
        egl-vnc) DISPLAY_ARGS="-display egl-headless,rendernode=$RENDERNODE -vnc :${VNC_DISPLAY:-1}"
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
# Queried into variables and matched with a case glob rather than piped into grep:
# a `grep -q` at the end of a pipeline exits at the first match and kills the
# producer with SIGPIPE, which reports failure under `set -o pipefail`. run_qemu.sh
# does not set it today, and this way it does not matter if it ever does.
if [ "$NEEDS_GL" -eq 1 ] && [ -n "${QEMU_BIN:-}" ]; then
    _dev_help="$("$QEMU_BIN" -device help 2>/dev/null || true)"
    _dpy_help="$("$QEMU_BIN" -display help 2>/dev/null || true)"
    _missing=""
    case "$_dev_help" in *virtio-gpu-gl-pci*) ;; *) _missing="virtio-gpu-gl-pci" ;; esac
    case "$_dpy_help" in
        *"$GL_BACKEND"*) ;;
        *) _missing="${_missing:+$_missing or }$GL_BACKEND" ;;
    esac
    if [ -n "$_missing" ]; then
        echo "WARNING: $QEMU_BIN has no $_missing, so DISPLAY_MODE=$DISPLAY_MODE" >&2
        echo "         cannot render on the host GPU. Falling back to" >&2
        echo "         DISPLAY_MODE=$NOGL_MODE, which rasterizes every frame on the" >&2
        echo "         *guest* CPU -- correct, and slower by orders of magnitude." >&2
        echo "         Build QEMU with virglrenderer and OpenGL to get GL here; see" >&2
        echo "         docs/BUILDING.md. Pass --no-gl to silence this." >&2
        DISPLAY_MODE="$NOGL_MODE"
        _resolve_display_mode
    fi
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
