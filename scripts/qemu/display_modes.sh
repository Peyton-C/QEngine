# Sourced by run_qemu.sh — not executed on its own.
#
# Turns DISPLAY_MODE into QEMU display arguments, and says whether the mode needs a
# GL-capable GPU. Adding a display backend means adding a case arm here rather than
# copying a launcher.
#
# The audio backend is picked from the host OS rather than being part of the mode.
# It had been part of the per-launcher copies this replaced: the plain VNC launcher
# asked for coreaudio and HVF while the virgl one asked for pipewire and KVM, so on
# Linux only one of the two could ever have worked. That was drift between copies, not
# a real difference between VNC modes.

DISPLAY_MODE="${DISPLAY_MODE:-sdl}"

# egl-headless renders on a specific DRM node. renderD128 is not always the card you
# want — on a machine with an integrated and a discrete GPU it is usually the
# integrated one — so it is overridable.
RENDERNODE="${RENDERNODE:-/dev/dri/renderD128}"

case "$DISPLAY_MODE" in
    sdl)     DISPLAY_ARGS="-display sdl,show-cursor=on";       NEEDS_GL=0 ;;
    sdl-gl)  DISPLAY_ARGS="-display sdl,gl=on,show-cursor=on"; NEEDS_GL=1 ;;
    cocoa)   DISPLAY_ARGS="-display cocoa,show-cursor=on";     NEEDS_GL=0 ;;
    vnc)     DISPLAY_ARGS="-vnc :${VNC_DISPLAY:-1}";           NEEDS_GL=0 ;;
    # Renders with GL off-screen and serves the result over VNC. Note that HMP
    # `screendump` reports "no surface" under egl-headless, so take screenshots with
    # a VNC client rather than the monitor.
    egl-vnc) DISPLAY_ARGS="-display egl-headless,rendernode=$RENDERNODE -vnc :${VNC_DISPLAY:-1}"
             NEEDS_GL=1 ;;
    none)    DISPLAY_ARGS="-display none";                     NEEDS_GL=0 ;;
    *)
        echo "ERROR: unknown DISPLAY_MODE '$DISPLAY_MODE'." >&2
        echo "       Known modes: sdl sdl-gl cocoa vnc egl-vnc none" >&2
        exit 1 ;;
esac

case "$(uname -s)" in
    Darwin) AUDIODEV_BACKEND="coreaudio"; AUDIODEV_ID="mac" ;;
    *)      AUDIODEV_BACKEND="pipewire";  AUDIODEV_ID="host" ;;
esac
