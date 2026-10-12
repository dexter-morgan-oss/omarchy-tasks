#!/usr/bin/env bash
# uninstall.sh — remove the per-user install created by install.sh.
#
#   ./uninstall.sh            remove binaries, launcher entry and icon
#   ./uninstall.sh --purge    also remove the Tasks keybinding from bindings.lua
#   ./uninstall.sh --data     also delete the task store (~/.local/share/tasks)
set -euo pipefail

BIN="${HOME}/.local/bin"
PURGE=0
DATA=0
for arg in "$@"; do
  case $arg in
    --purge) PURGE=1 ;;
    --data)  DATA=1 ;;
    *) echo "usage: uninstall.sh [--purge] [--data]" >&2; exit 2 ;;
  esac
done

rm -f "$BIN/tasks-gui" "$BIN/tasks" "$BIN/taskstore.py" "$BIN/tasks-setup-user"
rm -f "$HOME/.local/share/applications/org.omarchy.tasks.desktop"
rm -f "$HOME/.local/share/icons/hicolor/scalable/apps/org.omarchy.tasks.svg"
echo "Removed binaries, launcher entry and icon."

if (( PURGE )); then
  BIND="$HOME/.config/hypr/bindings.lua"
  if [[ -f $BIND ]] && grep -q 'org.omarchy.tasks' "$BIND"; then
    cp "$BIND" "$BIND.bak.$(date +%s)"
    grep -v \
      -e '-- Tasks: minimal GUI task manager' \
      -e 'omarchy-launch-or-focus org.omarchy.tasks' \
      "$BIND" > "$BIND.tmp"
    mv "$BIND.tmp" "$BIND"
    command -v hyprctl >/dev/null 2>&1 && [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]] \
      && hyprctl reload >/dev/null 2>&1 || true
    echo "Removed the Tasks keybinding (backup left next to bindings.lua)."
  fi
fi

if (( DATA )); then
  rm -rf "$HOME/.local/share/tasks"
  echo "Deleted the task store."
fi

echo "Done."
