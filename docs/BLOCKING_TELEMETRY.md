# Telemetry
QEngine blocks configured inMusic crash-reporting and analytics hosts in every rootfs it builds. The application can still create local crash reports; blocking prevents hostname-based connections to the listed services.

## Hosts
[block_telemetry.sh](../scripts/build_scripts/rootfs_steps/block_telemetry.sh) owns the hostname list. Add new endpoints there, one hostname per line, with a comment describing their purpose. Keep the list out of other scripts and documentation.

The shared `block_telemetry` step writes each host as `127.0.0.1 HOST` in the rootfs's `/etc/hosts`. It skips entries already present, so builders can apply it again without duplicating those lines.

Hosts entries cannot express wildcards, URL paths or ports. List each resolved subdomain separately. This mechanism does not block connections made directly to an external IP address.

## Existing guests
Stop the instance and rebuild with `new_instance.sh --force` to apply an updated list to its rootfs. Writable `/etc` overlays can hide the rebuilt file, so update the running guest as well when it retains an older hosts file.

Transfer the repository's `scripts/build_scripts/rootfs_steps/block_telemetry.sh` to `/data/block_telemetry.sh` in the guest, using [Remote access](REMOTE_ACCESS.md). As root, source the helper and apply the same list:

```sh
# If / is read-only, remount it before updating /etc/hosts.
mount -o remount,rw /
. /data/block_telemetry.sh
block_telemetry /
```

The helper is sourced, not run as a standalone script. It reports each host as blocked or already blocked. Remove the transferred helper when finished; future list changes still belong in the repository's shared file.

## Persistence
| Layout | Where a live edit persists |
| --- | --- |
| RK3288, with `/etc` overlaid | Writable layer on `emmc.img`; survives rootfs rebuilds while that disk is kept |
| RK3588, without that overlay | Instance `rootfs.img`; replaced on a rootfs rebuild |

`--force` keeps writable disks. Rebuilding therefore updates the base rootfs but does not reset an existing overlay. See [Storage](engine/STORAGE.md) and [Instances](../scripts/build_scripts/INSTANCES.md) for disk handling.
