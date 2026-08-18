# Sourced by the rootfs builders' privileged container — not executed on its own.
#
# Points every telemetry host Engine OS talks to at localhost, so the connection
# fails to connect rather than reaching InMusic. Engine already handles telemetry
# failures, so nothing else is affected. See docs/BLOCKING_TELEMETRY.md.
#
#   block_telemetry [rootfs mount point]    (default /mnt/rootfs)
#
# To block another host, add a line to the list at the bottom of this function —
# one hostname per line, with a comment saying what it is for. Blank lines and
# `#` comments are skipped, and trailing comments after a hostname are ignored,
# so entries can be annotated in place. Each is appended only if it is not
# already there, so re-running over a rootfs that has been through this before
# is a no-op rather than a pile of duplicates.
#
# Hostnames only. These become /etc/hosts entries, which cannot express
# wildcards, ports or paths — a blocked `*.example.com` needs each subdomain
# Engine actually resolves listed separately.
block_telemetry() {
    _rootfs="${1:-/mnt/rootfs}"
    _hosts="$_rootfs/etc/hosts"

    echo "--- blocking telemetry hosts (docs/BLOCKING_TELEMETRY.md) ---"
    # `read _host _rest` splits off the first word so a trailing comment lands in
    # _rest and is dropped; the case skips blank and fully-commented lines. Fed
    # by a heredoc rather than a pipeline so the loop is not a subshell.
    while read -r _host _rest; do
        case "$_host" in ''|'#'*) continue ;; esac

        _line="127.0.0.1 $_host"
        if grep -qxF "$_line" "$_hosts"; then
            echo "    already blocked: $_host"
        else
            echo "$_line" >> "$_hosts"
            echo "    blocked: $_host"
        fi
    done <<'TELEMETRY_HOSTS'
# List of telemetry hosts
o230257.ingest.sentry.io
analytics.inmusicbrands.com
posthog.data.aws.inmusic.dev
posthog.inmusic-cloud.prod.aws.inmusic.dev
ssl.google-analytics.com
TELEMETRY_HOSTS
}
