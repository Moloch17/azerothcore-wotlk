#!/usr/bin/env bash
#
# Bring a run's evaluation videos (dungeon-curriculum I0) home from the cluster workers, over SSH: each worker's
# runs/<stage>/videos/ into this machine's run directory, under videos/from-<worker>/ (a worker names its evaluations
# after its own decisions, so they would collide with the host's and with each other's).
#
#   apps/forge/tools/collect-videos.sh [--dry-run | --check] [--workers "user@host ..."] [--remote-dir DIR]
#                                      [--runs-dir DIR] <stage>
#
#   --dry-run       print what would be run, connect to nothing
#   --check         connect read-only: list what each worker would send (find over SSH), write nothing here
#   --workers       the machines to pull from (default: the cluster's workers -- eli is left out until the user says)
#   --remote-dir    the repository on each worker, relative to its home or absolute (default: animus-forge, the
#                   workers' clone: /home/<user>/animus-forge)
#   --runs-dir      the run directories, relative to the repository (default: var/animus-forge/shared/runs)
#
# Key-based SSH only (BatchMode: never a password prompt); a worker that cannot be reached is reported and skipped.
# Only videos are copied (the APNGs, their sidecars and each evaluation's index), never checkpoints or logs.

set -euo pipefail
cd "$(dirname "$0")/../../.."

workers="spencer@192.168.0.66 thomas@192.168.0.67 sarah@192.168.0.68 moloch@192.168.0.117"
remote_dir="animus-forge"
runs_dir="var/animus-forge/shared/runs"
mode="copy"
stage=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) mode="print"; shift ;;
        --check) mode="check"; shift ;;
        --workers) workers="$2"; shift 2 ;;
        --remote-dir) remote_dir="$2"; shift 2 ;;
        --runs-dir) runs_dir="$2"; shift 2 ;;
        -h|--help) sed -n '3,18p' "$0"; exit 0 ;;
        -*) echo "Unknown option $1 (--help lists them)" >&2; exit 2 ;;
        *) stage="$1"; shift ;;
    esac
done

if [[ ! "$stage" =~ ^[a-z0-9_]+$ ]]; then
    echo "Which stage? apps/forge/tools/collect-videos.sh [--dry-run] <stage>   (e.g. group2_corridor)" >&2
    exit 2
fi

ssh_options=(-o BatchMode=yes -o ConnectTimeout=10)
failed=0
for worker in $workers; do
    name="${worker%@*}"
    remote="$remote_dir/$runs_dir/$stage/videos"
    target="$runs_dir/$stage/videos/from-$name/"
    # tar over the SSH connection, not rsync: a worker need not have rsync (moloch@.117 has none). Only the videos,
    # their sidecars and each evaluation's index pages.
    listing="cd $(printf '%q' "$remote") && find . -type f \\( -name '*.png' -o -name '*.json' -o -name '*.html' \\)"
    case "$mode" in
        print)
            echo "mkdir -p $(printf '%q' "$target")"
            echo "ssh ${ssh_options[*]} $worker \"$listing -print0 | tar -cf - --null -T -\"" \
                "| tar -xf - -C $(printf '%q' "$target")"
            ;;
        check)
            # Read-only: what each worker would send, nothing written here.
            echo "== $worker ($remote)"
            if ! ssh "${ssh_options[@]}" "$worker" "$listing -printf '%s %p\\n'" 2>&1 | sed 's/^/   /'; then
                failed=$((failed + 1))
            fi
            ;;
        copy)
            echo "Videos: $worker ..."
            mkdir -p "$target"
            if ! ssh "${ssh_options[@]}" "$worker" "$listing -print0 | tar -cf - --null -T -" | tar -xf - -C "$target"
            then
                echo "  $worker: nothing copied (unreachable, no key, or no videos for $stage)" >&2
                failed=$((failed + 1))
            fi
            ;;
    esac
done

if [[ "$mode" == "copy" ]]; then
    echo "Done: $runs_dir/$stage/videos/from-<worker>/ ($failed of $(wc -w <<< "$workers") workers failed)."
fi
[[ $failed -eq 0 ]]
