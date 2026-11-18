#!/usr/bin/env bash
# Viewer clicks reach the tool of the panel being worked in (welder_ui driven with scripted mouse
# input, see STRATA_TEST_VIEWERCLICK in src/ui/main.cpp). Each case: click a Transform's rotate
# field in the properties bin, type 15, then drag the Transform's centre handle in the viewer.
# The typed value must stick and the drag must move the Transform, whichever panels were opened
# after the Transform's and with a colour picker left armed on another node.
#
#   tools/viewer_click_test.sh [build dir]
#
# Needs a display or xvfb-run.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
UI="$BUILD/welder_ui"
[ -x "$UI" ] || { echo "no $UI (build first)"; exit 1; }

WORK="$(mktemp -d -t welder-vclick-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/home"
cat > "$WORK/comp.weld" <<'JSON'
{
  "strata": 1,
  "format": {"width": 1920, "height": 1080},
  "frame_range": [1, 1],
  "fps": 24,
  "nodes": [
    {"name": "BG", "type": "CheckerBoard", "params": {"size": 120}},
    {"name": "Transform1", "type": "Transform", "inputs": ["BG"], "params": {}},
    {"name": "Grade1", "type": "Grade", "inputs": ["Transform1"], "params": {}},
    {"name": "Crop1", "type": "Crop", "inputs": ["Grade1"], "params": {}}
  ]
}
JSON

run() {
  local cmd=("$UI" "$WORK/comp.weld" --view Crop1 --size 1600x1200 --screenshot "$WORK/shot.png" "$@")
  if [ -z "${DISPLAY:-}" ]; then cmd=(xvfb-run -a -s "-screen 0 1600x1300x24" "${cmd[@]}"); fi
  HOME="$WORK/home" STRATA_TEST_VIEWERCLICK=rotate timeout 180 "${cmd[@]}" 2>&1 | grep "^VIEWERCLICK" || true
}

fail=0
check() {  # <case> <output>
  local name=$1 out=$2
  local res
  res=$(printf '%s\n' "$out" | grep "^VIEWERCLICK Transform1 " || true)
  if [[ "$res" == *"rotate = 15"* && "$res" == *"translate = ["* && "$res" == *"picker: off"* ]]; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    printf '%s\n' "$out" | sed 's/^/     /'
    fail=1
  fi
}

# the Transform's panel opened first, a Crop's (with viewer handles of its own) last
check "older panel's handles, after a newer tool panel" "$(run --props Crop1,Grade1,Transform1)"
# a colour pick left on in the Grade
check "picker left on in another panel" "$(STRATA_TEST_VIEWERCLICK_PICK=Grade1.gain run --props Transform1,Grade1)"
# both at once
check "both" "$(STRATA_TEST_VIEWERCLICK_PICK=Grade1.gain run --props Crop1,Grade1,Transform1)"
exit $fail
