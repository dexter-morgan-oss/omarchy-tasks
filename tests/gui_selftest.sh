#!/usr/bin/env bash
# Build tasks-gui with warnings-as-errors and run its embedded selftests.
#
# The GUI needs a display; if none is available the script prints SKIP and
# exits 0 so it can live in a headless CI pipeline alongside the Python tests.
#
# Usage: tests/gui_selftest.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SRC="tasks-gui.c"
BIN="$(mktemp /tmp/opencode/tasks-gui-test.XXXXXX)" || exit 1
TMP="$(mktemp -d /tmp/opencode/tasks-gui-tests.XXXXXX)" || exit 1
trap 'rm -f "$BIN"; rm -rf "$TMP"' EXIT

pass=0
fail=0
note() { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail + 1)); }

note "build (-Wall -Wextra -Werror)"
if gcc -O2 -Wall -Wextra -Werror $(pkg-config --cflags gtk4 jansson) "$SRC" \
        -o "$BIN" $(pkg-config --libs gtk4 jansson) -lm 2>"$TMP/build.log"; then
    ok "compiled clean"
else
    bad "compile failed"
    cat "$TMP/build.log"
    exit 1
fi

# ---- --version ------------------------------------------------------------
note "--version"
gui_ver="$("$BIN" --version 2>/dev/null | awk '{print $2}')"
cli_ver="$(python3 tasks.py version 2>/dev/null)"
if [ -n "$gui_ver" ] && [ "$gui_ver" = "$cli_ver" ]; then
    ok "tasks-gui and tasks agree on version ($gui_ver)"
else
    bad "version mismatch: gui='$gui_ver' cli='$cli_ver'"
fi

if [ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    note "display"
    printf '  SKIP GUI selftests (no WAYLAND_DISPLAY or DISPLAY)\n'
    printf '\n%s passed, %s failed\n' "$pass" "$fail"
    [ "$fail" -eq 0 ]
    exit
fi

run_gui() { # <data-dir> <env assignments...>
    local data="$1"; shift
    mkdir -p "$data/tasks"
    env XDG_DATA_HOME="$data" "$@" "$BIN" 2>&1
}

# ---- behavioural selftest -------------------------------------------------
note "behavioural selftest (empty store)"
out="$(run_gui "$TMP/empty" TASKS_SELFTEST=1)"
rc=$?
if printf '%s' "$out" | grep -q "selftest OK (0 failures)" && [ "$rc" -eq 0 ]; then
    ok "selftest OK (0 failures)"
else
    bad "selftest failed (rc=$rc)"
    printf '%s\n' "$out" | grep -E '^FAIL|FAILURES' || printf '%s\n' "$out"
fi
nchecks="$(printf '%s' "$out" | grep -c '^PASS ')"
printf '  (%s checks passed)\n' "$nchecks"

# ---- GUI writes a store the CLI can read ----------------------------------
note "GUI store is CLI-readable"
if XDG_DATA_HOME="$TMP/empty" python3 tasks.py list --all >/dev/null 2>"$TMP/cli.log"; then
    ok "tasks list reads the GUI-written store"
else
    bad "tasks list rejected the GUI-written store"
    cat "$TMP/cli.log"
fi

# ---- load selftest (reads a CLI/foreign store) ----------------------------
note "load selftest (reads fixture store)"
mkdir -p "$TMP/load/tasks"
cat > "$TMP/load/tasks/tasks.json" <<'JSON'
{
  "next_id": 5,
  "tasks": [
    {"id": 1, "title": "cligh", "done": false, "status": "todo", "priority": "high", "due": "2026-01-01", "created": "2026-01-01"},
    {"id": 2, "title": "prog", "done": false, "status": "progress", "priority": "low", "due": null, "created": "2026-01-02"},
    {"id": 3, "title": "fin", "done": true, "status": "done", "priority": "med", "due": null, "created": "2026-01-03"},
    {"id": 4, "title": "legacy", "done": true, "created": "2026-01-04"}
  ]
}
JSON
out="$(run_gui "$TMP/load" TASKS_SELFTEST_LOAD=1)"
rc=$?
if printf '%s' "$out" | grep -q "load selftest OK (0 failures)" && [ "$rc" -eq 0 ]; then
    ok "load selftest OK (0 failures)"
else
    bad "load selftest failed (rc=$rc)"
    printf '%s\n' "$out" | grep -E '^FAIL|FAILURES' || printf '%s\n' "$out"
fi

# ---- malformed store must not crash the GUI -------------------------------
note "malformed store is tolerated"
mkdir -p "$TMP/broken/tasks"
printf '{ this is not valid json' > "$TMP/broken/tasks/tasks.json"
out="$(run_gui "$TMP/broken" TASKS_SELFTEST=1)"
rc=$?
if [ "$rc" -eq 0 ] && printf '%s' "$out" | grep -q "selftest OK"; then
    ok "GUI survives a corrupt tasks.json"
else
    bad "GUI crashed or misbehaved on corrupt json (rc=$rc)"
    printf '%s\n' "$out" | tail -5
fi
if compgen -G "$TMP/broken/tasks/tasks.json.corrupt-*" >/dev/null; then
    ok "corrupt store was quarantined, not overwritten"
else
    bad "corrupt store was not backed up"
fi

printf '\n%s passed, %s failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
