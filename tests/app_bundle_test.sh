#!/bin/bash
#
# The built psDoom.app carries Summoner: Contents/Resources/summoner/summon,
# its core and its built-in plugins (and not its tests), and that bundled copy
# runs (read-only `describe` and `get` against the built-in `ps` plugin, with
# a scratch root and a scratch HOME; nothing is acted on).
#
# Usage: app_bundle_test.sh <path to psDoom.app> <work dir>
set -euo pipefail

app=${1:?usage: app_bundle_test.sh APP WORK}
work=${2:?usage: app_bundle_test.sh APP WORK}
case "$work" in
    /*/app-bundle-test) ;;
    *) echo "refusing work dir $work"; exit 2 ;;
esac
res="$app/Contents/Resources/summoner"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

check "summon is bundled and executable" '[[ -x "$res/summon" ]]'
check "core is bundled"                  '[[ -f "$res/summoner.py" ]]'
check "ps plugin is bundled"             '[[ -f "$res/plugins/processes.py" ]]'
check "csv plugin is bundled"            '[[ -f "$res/plugins/csv_rows.py" ]]'
check "tests are not bundled"            '[[ ! -e "$res/tests" ]]'
check "engine sits in MacOS"             '[[ -x "$app/Contents/MacOS/crispy-doom" ]]'

# The app icon: Info.plist names it and the named .icns is in Resources.
icon=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIconFile' "$app/Contents/Info.plist" 2>/dev/null || true)
icns="$app/Contents/Resources/$icon"
check "Info.plist sets CFBundleIconFile"   '[[ "$icon" == psDoom.icns ]]'
check "the named .icns is in Resources"    '[[ -n "$icon" && -s "$icns" && "$(head -c 4 "$icns")" == icns ]]'
check "Crispy Doom's GPL text is bundled"  '[[ -s "$app/Contents/Resources/Crispy-Doom-COPYING.md" ]]'

py=""
for c in /opt/homebrew/bin/python3 /usr/local/bin/python3 \
         /Library/Developer/CommandLineTools/usr/bin/python3 \
         /Applications/Xcode.app/Contents/Developer/usr/bin/python3; do
    if [[ -x "$c" ]]; then py=$c; break; fi
done
if [[ -z "$py" ]]; then
    echo "skip run checks: no python3 on this host (psDoom would show no process monsters)"
else
    rm -rf "$work"
    mkdir -p "$work/home"
    # -I -B as psDoom runs it, so nothing is written into the app.
    out=$(env -u SUMMONER_ROOT HOME="$work/home" \
          "$py" -I -B "$res/summon" --root "$work/root" describe)
    check "bundled summon describes the ps plugin" '[[ "$out" == *relevance* ]]'
    env -u SUMMONER_ROOT HOME="$work/home" "$py" -I -B "$res/summon" --root "$work/root" refresh
    out=$(env -u SUMMONER_ROOT HOME="$work/home" \
          "$py" -I -B "$res/summon" --root "$work/root" get)
    check "bundled summon lists processes" '[[ "$out" == *"# dim id=relevance"* && "$out" == *"id="* ]]'
    check "nothing written to the scratch home" '[[ ! -e "$work/home/.config" ]]'
    check "no __pycache__ in the app" '[[ ! -e "$res/__pycache__" && ! -e "$res/plugins/__pycache__" ]]'
    rm -rf "$work"
fi
exit $fail
