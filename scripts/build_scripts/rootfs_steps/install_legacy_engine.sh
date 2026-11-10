# Sourced by the armv7 builder's privileged container, never run on its own.
# shellcheck shell=bash
# Product-named executables and glibc 2.25 predate the modern shim and EGLFS stack.
# Re-running this step replaces only QEngine's files and preserves the stock unit.
install_legacy_engine() {
    local rootfs="${1:-/mnt/rootfs}" stage="${2:-/stage}" unit artifact rotation=90 extra_preload=""
    if [ -x "$rootfs/usr/JP07/JP07" ]; then unit=jp07; else unit=engine; fi
    # SC5000 1.4.1 and 1.5.2/1.6.0 already render in landscape on Qt 5.12.
    if [ -f "$rootfs/usr/qt/lib/libQt5Core.so.5.12" ]; then rotation=0; fi
    if [ -x "$rootfs/usr/Engine/JC11" ]; then
        # Prime 4 1.3.2/1.3.3 rotate the Qt 5.9 UI opposite to SC5000.
        # Its Qt 5.12 runtime (1.4.1) already renders in landscape.
        rotation=0
        if [ -f "$rootfs/usr/qt/lib/libQt5Core.so.5.9.2" ]; then rotation=270; fi
    fi
    mkdir -p "$rootfs/root" "$rootfs/etc/qengine" \
        "$rootfs/etc/systemd/system/multi-user.target.wants"
    for artifact in dtshim.so qmlrotate.so vnctouchbridge; do
        install -m 755 "$stage/legacy/$artifact" "$rootfs/root/$artifact"
    done
    if [ -f "$stage/legacy/alsashim.so" ]; then
        install -m 755 "$stage/legacy/alsashim.so" "$rootfs/root/alsashim.so"
        extra_preload=:/root/alsashim.so
    fi
    if [ -f "$rootfs/usr/qt/lib/libQt5Core.so.5.5.1" ]; then
        # Engine 1.0.x selects customcontext itself. Preserve the vendor plugin
        # outside Qt's search directory and serve that name with the raster one.
        local scenegraph="$rootfs/usr/qt/plugins/scenegraph"
        mkdir -p "$scenegraph/vendor"
        if [ ! -e "$scenegraph/vendor/libcustomcontext.so" ]; then
            mv "$scenegraph/libcustomcontext.so" "$scenegraph/vendor/"
        fi
        install -m 755 "$stage/legacy/libsoftwarecontext.so" "$scenegraph/libcustomcontext.so"
        install -m 755 "$stage/legacy/qt55vnc.so" "$rootfs/root/qt55vnc.so"
        touch "$rootfs/etc/qengine/qt55-offscreen"
        extra_preload=:/root/qt55vnc.so
    fi
    install -m 755 /vm/start_legacy_engine.sh "$rootfs/root/start_legacy_engine.sh"
    if [ -f "$rootfs/etc/systemd/system/$unit.service" ] &&
            [ ! -e "$rootfs/etc/systemd/system/$unit.service.stock" ]; then
        cp -a "$rootfs/etc/systemd/system/$unit.service" "$rootfs/etc/systemd/system/$unit.service.stock"
    fi
    cat > "$rootfs/etc/systemd/system/$unit.service" <<EOF
[Unit]
Description=QEngine legacy Engine
After=local-fs.target network.target dbus.service systemd-user-sessions.service

[Service]
Type=simple
Environment=LD_LIBRARY_PATH=/usr/qt/lib:/usr/Engine:/usr/Engine/ffmpeg:/usr/JP07:/usr/JP07/ffmpeg
Environment=LD_PRELOAD=/root/dtshim.so:/root/qmlrotate.so$extra_preload
Environment=QMLROTATE=$rotation
ExecStart=/root/start_legacy_engine.sh
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF
    if [ -f "$stage/legacy/alsashim.so" ]; then
        # SC5000 1.5.2/1.6.0 need a playback-only card: exposing loopback
        # capture reaches a null audio-buffer dereference after startup.
        # Plug conversion fixes rate negotiation; four channels cover both
        # stereo layers without advertising the plug plugin's 10000 channels.
        mkdir -p "$rootfs/etc/systemd/system/$unit.service.d"
        cat > "$rootfs/etc/systemd/system/$unit.service.d/legacy-alsa.conf" <<'EOF'
[Service]
Environment=ALSASHIM_AS=Loopback
Environment=ALSASHIM_MAX_CHANNELS=4
Environment=ALSASHIM_NO_CAPTURE=1
# Engine 1.5.2 corrupts its heap with the modern shim's enlarged buffers.
Environment=ALSASHIM_BUFFER_SCALE=1
EOF
    else
        rm -f "$rootfs/etc/systemd/system/$unit.service.d/legacy-alsa.conf"
    fi
    ln -sf "../$unit.service" "$rootfs/etc/systemd/system/multi-user.target.wants/$unit.service"
    # The launcher reads this marker to expose the app's VNC server, select a
    # non-GL console display, and give the RK3288 affinity setup four CPUs.
    printf '%s\n' "$unit" > "$rootfs/etc/qengine/legacy-engine"
    rm -f "$rootfs/etc/systemd/system/getty.target.wants/getty@tty1.service"
    ln -sf /dev/null "$rootfs/etc/systemd/system/getty@tty1.service"
    ln -sf /dev/null "$rootfs/etc/systemd/system/autovt@tty1.service"
}
