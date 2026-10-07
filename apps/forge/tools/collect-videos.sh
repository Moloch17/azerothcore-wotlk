#!/usr/bin/env bash
#
# Bring a run's evaluation videos (dungeon-curriculum I0) home from the cluster workers, over SSH: each worker's
# runs/<stage>/videos/ into this machine's run directory, under videos/from-<worker>/ (a worker names its evaluations
# after its own decisions, so they would collide with the host's and with each other's).
#
#   apps/forge/tools/collect-videos.sh [--dry-run] [--workers "user@host ..."] [--remote-dir DIR] [--runs-dir DIR]
# <stage>
#
#   --dry-run       print what would be run, connect to nothing
#   --workers       the machines to pull from (default: the cluster's workers -- eli is left out until the user says)
#   --remote-dir    the repository on each worker, relative to its home or absolute (default: azerothcore, the clone
#                   cluster-pull.sh makes)
#   --runs-dir      the run directories, relative to the repository (default: var/animus-forge/shared/runs)
#
# Key-based SSH only (BatchMode: never a password prompt); a worker that cannot be reached is reported and skipped.
# Only videos are copied (the APNGs, their sidecars and each evaluation's index), never checkpoints or logs.

set -euo pipefail
cd "$(dirname "$0")/../../.."

workers="spencer@192.168.0.66 thomas@192.168.0.67 sarah@192.168.0.68 moloch@192.168.0.117"
remote_dir="azerothcore"
runs_dir="var/animus-forge/shared/runs"
dry_run=0
stage=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) dry_run=1; shift ;;
        --workers) workers="$2"; shift 2 ;;
        --remote-dir) remote_dir="$2"; shift 2 ;;
        --runs-dir) runs_dir="$2"; shift 2 ;;
        -h|--help) sed -n '3,17p' "$0"; exit 0 ;;
        -*) echo "Unknown option $1 (--help lists them)" >&2; exit 2 ;;
        *) stage="$1"; shift ;;
    esac
done

if [[ ! "$stage" =~ ^[a-z0-9_]+$ ]]; then
    echo "Which stage? apps/forge/tools/collect-videos.sh [--dry-run] <stage>   (e.g. group2_corridor)" >&2
    exit 2
fi

ssh_options="-o BatchMode=yes -o ConnectTimeout=10"
failed=0
for worker in $workers; do
    name="${worker%@*}"
    source="$worker:$remote_dir/$runs_dir/$stage/videos/"
    target="$runs_dir/$stage/videos/from-$name/"
    command=(rsync -a --partial --include='*/' --include='*.png' --include='*.json' --include='*.html'
             --exclude='*' -e "ssh $ssh_options" "$source" "$target")
    if [[ $dry_run -eq 1 ]]; then
        echo "mkdir -p $(printf '%q' "$target")"
        printf '%q ' "${command[@]}"
        echo
        continue
    fi
    echo "Videos: $worker ..."
    mkdir -p "$target"
    if ! "${command[@]}"; then
        echo "  $worker: nothing copied (unreachable, no key, or no videos for $stage)" >&2
        failed=$((failed + 1))
    fi
done

if [[ $dry_run -eq 0 ]]; then
    echo "Done: $runs_dir/$stage/videos/from-<worker>/ ($failed of $(wc -w <<< "$workers") workers failed)."
fi
[[ $failed -eq 0 ]]
