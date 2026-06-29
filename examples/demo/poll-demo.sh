#!/bin/sh
#
# psDoom demo poller -- synthetic entities, no real processes touched.
#
# A safe way to see the script backend (and the classifier / re-classification
# / fork-swarm features) without wiring psDoom to anything real. Loads change
# every poll so you can watch monsters re-grade, and one parent periodically
# spawns a burst of children to trip the fork-bomb swarm.
#
#   export PSDOOM_POLL_CMD="$PWD/examples/demo/poll-demo.sh"
#   export PSDOOM_RESPOND_CMD="$PWD/examples/demo/respond-demo.sh"

# Random int in [0, $1). srand() is time-seeded, so values vary between polls.
rnd() { awk 'BEGIN { srand(); print int(rand() * '"$1"') }'; }

# A steady cast spread across the size ladder; loads vary each poll so, with
# "Classify by = CPU", these visibly re-classify up and down.
printf 'id=101\tlabel=idle-svc\tweight=8000000\tload=%s\n'                "$(rnd 5)"
printf 'id=102\tlabel=web-worker\tweight=120000000\tload=%s\tparent=101\n' "$(rnd 220)"
printf 'id=103\tlabel=database\tweight=2000000000\tload=%s\n'              "$(rnd 400)"

# Fork-bomb demo: emit no children most of the time, then a burst of 8 for ~2s
# every ~10s. The 0 -> 8 jump in `database`'s child count trips the detector
# and spawns a Lost Soul swarm around it.
phase=$(( $(date +%s) % 10 ))
if [ "$phase" -lt 2 ]; then
    i=2000
    while [ "$i" -lt 2008 ]; do
        printf 'id=%d\tlabel=child\tweight=4000000\tload=10\tparent=103\n' "$i"
        i=$(( i + 1 ))
    done
fi

exit 0
