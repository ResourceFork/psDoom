#!/bin/bash
#
# The reference scripts under scripts/ and examples/ against the trust model
# (docs/script-backend.md): the responders honour `mode=`, and the poller
# never lists its own calling chain.
#
# Responders are run as stubbed COPIES under WORK: renice, kill and docker
# are replaced by `echo`, so nothing is ever signalled, reniced or killed.
#
# Usage: reference_scripts_test.sh <repo root> <work dir>
set -euo pipefail

repo=${1:?usage: reference_scripts_test.sh REPO WORK}
work=${2:?usage: reference_scripts_test.sh REPO WORK}
case "$work" in
    /*/reference-scripts-test) ;;
    *) echo "refusing work dir $work"; exit 2 ;;
esac
[[ -f "$repo/scripts/respond-processes.sh" ]] || { echo "bad repo"; exit 2; }
mkdir -p "$work"

fail=0
check() {   # name expected actual
    if [[ "$2" != "$3" ]]; then
        echo "FAIL $1: want [$2] got [$3]"
        fail=1
    else
        echo "ok   $1"
    fi
}

# Shell functions shadow the commands the responders call, injected right
# after the shebang, so a real renice, kill or docker can never run however
# a responder spells the call. A path-qualified call (/bin/kill) would bypass
# a function, so any is refused outright. The target pid is beyond any pid a
# system hands out (macOS stops at 99998, Linux pid_max at 4194304).
TARGET=2147483646
stub() {    # src dst
    if grep -qE '/(usr/)?(s?bin)/(kill|renice|docker)|command +(kill|renice)|builtin +kill' "$1"; then
        echo "FAIL $1 calls kill/renice/docker by path; cannot stub safely"; exit 1
    fi
    {
        head -n 1 "$1"
        # The responders discard their output, so the stubs record to a log.
        echo "renice() { echo RENICE >> '$work/stub.log'; }"
        echo "kill() { echo KILL >> '$work/stub.log'; }"
        echo "docker() { case \"\$1\" in ps) echo fake;; inspect) echo $TARGET;; update) echo THROTTLE >> '$work/stub.log';; kill) echo DKILL >> '$work/stub.log';; esac; }"
        tail -n +2 "$1"
    } > "$2"
    chmod +x "$2"
}

stub "$repo/scripts/respond-processes.sh" "$work/proc.sh"
stub "$repo/examples/docker/respond-containers.sh" "$work/docker.sh"

run() {    # responder args...: what the stubs recorded
    : > "$work/stub.log"
    "$@" >/dev/null 2>&1 || true
    tr -d '\n' < "$work/stub.log"
}

for s in proc docker; do
    post=RENICE; del=KILL
    [[ $s == docker ]] && { post=THROTTLE; del=DKILL; }
    r="$work/$s.sh"
    check "$s no mode post"       ""      "$(run "$r" post $TARGET amount=4)"
    check "$s no mode delete"     ""      "$(run "$r" delete $TARGET)"
    check "$s dryrun post"        ""      "$(run "$r" post $TARGET amount=4 mode=dryrun)"
    check "$s dryrun delete"      ""      "$(run "$r" delete $TARGET mode=dryrun)"
    check "$s post-only post"     "$post" "$(run "$r" post $TARGET amount=4 mode=post-only)"
    check "$s post-only delete"   ""      "$(run "$r" delete $TARGET mode=post-only)"
    check "$s delete post"        "$post" "$(run "$r" post $TARGET amount=4 mode=delete)"
    check "$s delete delete"      "$del"  "$(run "$r" delete $TARGET mode=delete)"
    check "$s unknown mode"       ""      "$(run "$r" delete $TARGET mode=kill-all)"
    check "$s last mode wins"     ""      "$(run "$r" delete $TARGET mode=delete mode=dryrun)"
    check "$s label no smuggle"   ""      "$(run "$r" delete $TARGET label=mode=delete)"
done

# The poller runs under this shell: neither this shell, the poller's own
# shell, nor anything above them may be listed.
chain=$(ps -axo pid=,ppid= | awk -v start="$$" '
    { parent[$1] = $2 }
    END { p = start; for (n = 0; p > 1 && n < 64; n++) { print p; p = parent[p] } }')
out=$("$repo/scripts/poll-processes.sh")
hits=0
for p in $chain; do
    if printf '%s\n' "$out" | grep -qE "^id=$p	"; then
        echo "FAIL poller listed ancestor pid $p"; hits=1
    fi
done
check "poller skips its calling chain" 0 "$hits"
check "poller lists something" 1 "$([[ -n "$out" ]] && echo 1 || echo 0)"
if printf '%s\n' "$out" | grep -qE 'label=(login|sudo|loginwindow|Dock|Finder)$'; then
    check "poller skips protected and setuid names" 0 1
else
    check "poller skips protected and setuid names" 0 0
fi

rm -f "$work/proc.sh" "$work/docker.sh" "$work/stub.log"
rmdir "$work"
exit $fail
