# Blocking Sentry Telemetry
By defailt Engine OS ships with crash reporting and anonymous analytics enabled by default on every version of Engine OS tested so far. To avoid flooding InMusic with bad data we block analytics.

## What to block
The Sentry ingest host events get sent to: `o230257.ingest.sentry.io`

Point the hostname at localhost so the connection fails to send, Engine already handles telemetry failures, so this doesn't affect anything else:

```sh
mount -o remount,rw /
echo "127.0.0.1 o230257.ingest.sentry.io" >> /etc/hosts
```

Whether this persists across reboots depends on the device's filesystem layout:

- **armv7/RK3288** — `/etc` is overlaid onto the `emmc.img` overlay partition (see [BUILDING.md](../BUILDING.md)'s eMMC overlay section); the edit lands in that overlay's writable layer and persists as long as the same `emmc.img` is reused.
- **arm64/RANE SYSTEM ONE** — no overlay at all; `/etc` lives directly on the base rootfs image, so the edit persists in `rootfs_out.img` itself once written (see [BUILDING.md](../BUILDING.md)'s arm64 section).