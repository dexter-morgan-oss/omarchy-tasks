#!/usr/bin/env bash
# install.sh — build Tasks and install it for the current user.
#
# Installs the GUI + CLI/TUI to ~/.local/bin, the app icon to the user icon
# theme, then wires up the launcher entry, Omarchy menu row and the
# SUPER+SHIFT+T keybinding (via tasks-setup-user).
#
# For a system-wide install instead, use the PKGBUILD:
#   makepkg -si
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${HOME}/.local/bin"
ICON_DIR="${HOME}/.local/share/icons/hicolor/scalable/apps"

if ! command -v gcc >/dev/null 2>&1; then
  echo "error: gcc is required to build the GUI." >&2
  exit 1
fi

# GTK4 + jansson are required for the GUI; error out early with a hint.
if ! pkg-config --exists gtk4 jansson; then
  echo "error: gtk4 and jansson development packages are required:" >&2
  echo "  sudo pacman -S --needed gtk4 jansson" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Building tasks-gui…"
gcc -O2 -march=native -Wall $(pkg-config --cflags gtk4 jansson) \
    "$DIR/tasks-gui.c" -o "$TMP/tasks-gui" $(pkg-config --libs gtk4 jansson) -lm

install -Dm755 "$TMP/tasks-gui"        "$BIN/tasks-gui"
install -Dm755 "$DIR/tasks.py"         "$BIN/tasks"
install -Dm755 "$DIR/taskstore.py"     "$BIN/taskstore.py"
install -Dm755 "$DIR/tasks-setup-user"  "$BIN/tasks-setup-user"
install -Dm644 "$DIR/tasks.svg"        "$ICON_DIR/org.omarchy.tasks.svg"

echo "Installed:"
echo "  $BIN/tasks-gui   (compiled GTK4 GUI)"
echo "  $BIN/tasks       (CLI/TUI: tasks add/list/done/rm)"
echo "  $BIN/tasks-setup-user (re-run to repair desktop integration)"
echo

"$BIN/tasks-setup-user" "$BIN/tasks-gui"

if ! command -v tasks >/dev/null 2>&1; then
  echo
  echo "Note: add ~/.local/bin to your PATH if 'tasks' is not found."
fi
