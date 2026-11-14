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

stub() {    # src dst
    sed -e 's|renice "$amount" -p "$id".*|echo RENICE|' \
        -e 's|kill "$id" >/dev/null 2>&1.*|echo KILL|' \
        -e 's|docker update --cpu-shares 256 "$cid".*|echo THROTTLE|' \
        -e 's|docker kill "$cid".*|echo DKILL|' \
        -e 's|^command -v docker.*||' \
        -e 's|^for c in \$(docker ps -q 2>/dev/null); do|for c in fake; do|' \
        -e "s|p=\$(docker inspect -f '{{.State.Pid}}' \"\$c\" 2>/dev/null)|p=999999|" \
        "$1" > "$2"
    chmod +x "$2"
    if grep -qE '^[^#]*(renice "|kill "\$id"|docker (update|kill|ps|inspect))' "$2"; then
        echo "FAIL stub left a real action in $2"; exit 1
    fi
}

stub "$repo/scripts/respond-processes.sh" "$work/proc.sh"
stub "$repo/examples/docker/respond-containers.sh" "$work/docker.sh"

for s in proc docker; do
    post=RENICE; del=KILL
    [[ $s == docker ]] && { post=THROTTLE; del=DKILL; }
    r="$work/$s.sh"
    check "$s no mode post"       ""      "$("$r" post 999999 amount=4)"
    check "$s no mode delete"     ""      "$("$r" delete 999999)"
    check "$s dryrun post"        ""      "$("$r" post 999999 amount=4 mode=dryrun)"
    check "$s dryrun delete"      ""      "$("$r" delete 999999 mode=dryrun)"
    check "$s post-only post"     "$post" "$("$r" post 999999 amount=4 mode=post-only)"
    check "$s post-only delete"   ""      "$("$r" delete 999999 mode=post-only)"
    check "$s delete post"        "$post" "$("$r" post 999999 amount=4 mode=delete)"
    check "$s delete delete"      "$del"  "$("$r" delete 999999 mode=delete)"
    check "$s unknown mode"       ""      "$("$r" delete 999999 mode=kill-all)"
    check "$s last mode wins"     ""      "$("$r" delete 999999 mode=delete mode=dryrun)"
    check "$s label no smuggle"   ""      "$("$r" delete 999999 label=mode=delete)"
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

rm -f "$work/proc.sh" "$work/docker.sh"
rmdir "$work"
exit $fail
