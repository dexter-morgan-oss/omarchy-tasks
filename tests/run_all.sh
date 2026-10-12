#!/usr/bin/env bash
# Run the whole test suite: storage unit tests, CLI end-to-end tests,
# CLI/GUI interop tests, and the GUI selftests.
#
# Usage: tests/run_all.sh
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

fail=0

run() {
    local name="$1"; shift
    printf '\n\033[1m#### %s\033[0m\n' "$name"
    if "$@"; then
        printf '\033[32m#### %s: OK\033[0m\n' "$name"
    else
        printf '\033[31m#### %s: FAILED\033[0m\n' "$name"
        fail=1
    fi
}

run "taskstore unit tests" python3 tests/test_taskstore.py
run "CLI end-to-end tests"  python3 tests/test_cli.py
run "CLI/GUI interop tests" python3 tests/test_interop.py
run "GUI selftests"         bash tests/gui_selftest.sh

printf '\n'
if [ "$fail" -eq 0 ]; then
    printf '\033[32mALL SUITES PASSED\033[0m\n'
else
    printf '\033[31mSOME SUITES FAILED\033[0m\n'
fi
exit "$fail"
