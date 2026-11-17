#!/bin/bash
# sweep.sh [check.sh options] HOST ... : run check.sh on every instance of each
# host at once, one host's report after another.
#
# Options before the first host go to check.sh: -g, -s, -b, -p. Hosts run in
# parallel and instances on a host in turn. Exits 1 if any host's check did.
set -eu
source "$(dirname "$0")/../common.sh"
flags=()
while [ $# -gt 0 ]; do
    case "$1" in
        -p) flags+=("$1" "$2"); shift 2 ;;
        -*) flags+=("$1"); shift ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || { sed -n '2,6p' "$0" >&2; exit 2; }
# Once, here, so the hosts do not each start the same Docker build.
"$TEST_SCRIPTS/fake/build.sh"
mkdir -p "$OUT/fake"
pids=()
status=0
for host in "$@"; do
    "$TEST_SCRIPTS/fake/check.sh" -H "$host" ${flags[@]+"${flags[@]}"} \
        > "$OUT/fake/sweep-${host#*@}.txt" 2>&1 &
    pids+=($!)
    # With -s each host adds its instances' libraries to one shim build, so
    # the hosts take turns.
    case " ${flags[*]-} " in *" -s "*) wait $! || status=1 ;; esac
done
# One at a time by pid: macOS ships bash 3.2, which has no wait -n.
for pid in "${pids[@]}"; do
    wait "$pid" 2>/dev/null || status=1
done
for host in "$@"; do
    echo "=== $host"
    grep -v '^--- ' "$OUT/fake/sweep-${host#*@}.txt" || true
done
exit $status
