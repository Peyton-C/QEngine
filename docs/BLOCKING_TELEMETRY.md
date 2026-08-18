# Blocking Telemetry
By defailt Engine OS ships with crash reporting and anonymous analytics enabled by default on every version of Engine OS tested so far. To avoid flooding InMusic with bad data we block analytics.

## What to block
[rootfs_steps/block_telemetry.sh](../scripts/build_scripts/rootfs_steps/block_telemetry.sh) holds the list, and is the one place to add to — one hostname per line, and both rootfs builders pick it up on the next build. It is deliberately the list rather than this document, so that what gets blocked cannot drift from what is documented as blocked. Read it for the current set; as of writing it covers Sentry crash reporting, two PostHog product-analytics endpoints, an InMusic analytics host, and Google Analytics.

Because these become `/etc/hosts` entries they can only be hostnames — `/etc/hosts` has no way to express a wildcard, a port or a URL path, so blocking everything under a domain means listing each subdomain Engine actually resolves. A hostname that Engine reaches by IP address rather than by name cannot be blocked this way at all.

Point each hostname at localhost so the connection fails to send. Engine already handles telemetry failures, so this doesn't affect anything else:

```sh
mount -o remount,rw /
for host in o230257.ingest.sentry.io analytics.inmusicbrands.com; do
    echo "127.0.0.1 $host" >> /etc/hosts
done
```

The rootfs builders do this for you at build time — `block_telemetry` in the shared [rootfs_steps/](../scripts/build_scripts/rootfs_steps/) appends every host in that list to the image's `/etc/hosts`, skipping any already present, so it is safe to re-run over an image that has been built before. The commands above are for a running guest, or a device you are not rebuilding.

Whether this persists across reboots depends on the device's filesystem layout:

- **armv7/RK3288** — `/etc` is overlaid onto the `emmc.img` overlay partition (see [BUILDING.md](BUILDING.md)'s eMMC overlay section); the edit lands in that overlay's writable layer and persists as long as the same `emmc.img` is reused.
- **arm64/RANE SYSTEM ONE** — no overlay at all; `/etc` lives directly on the base rootfs image, so the edit persists in `rootfs_out.img` itself once written (see [BUILDING.md](BUILDING.md)'s arm64 section).