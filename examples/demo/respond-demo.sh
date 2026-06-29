#!/bin/sh
#
# psDoom demo responder -- no real side effects.
#
# Records what psDoom asked for so you can see the response contract in action
# without anything being signalled. Invoked as:
#   respond-demo.sh <verb> <id> [amount=<n>] [label=<text>]
#
# Watch it with:  tail -f "${TMPDIR:-/tmp}/psdoom-demo-actions.log"

log="${TMPDIR:-/tmp}/psdoom-demo-actions.log"
printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*" >> "$log"
exit 0
