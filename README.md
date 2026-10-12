# Tasks

A minimal **3-column kanban** task manager for [Omarchy](https://omarchy.org) —
text and colour only (no icon clutter), smooth drag-and-drop, built for people
who live in the keyboard. The GUI is a small native **C + GTK4** binary, so it
starts instantly and stays light; a zero-dependency **CLI/TUI** shares the same
JSON store.

```
┌─ To Do ────────┐  ┌─ In Progress ──┐  ┌─ Done ─────────┐
│ ○ ship v1      │  │ ● review PR    │  │ ✓ write notes  │
│ ● buy milk     │  │                │  │                │
```

- **Columns**: To Do · In Progress · Done — each stretches with the window
- **Drag any task** between columns, or reorder within one
- **Drag to Done = complete**; drag back out to reopen
- **Click empty column space** to drop an add box right there
- Priority shown by **colour** — the title is tinted (high red · normal gray ·
  low green) with a small dot you can click to cycle
- **Follows your Omarchy theme**: reads the active
  `~/.local/state/omarchy/current/theme/colors.toml` at runtime and reloads
  **live** when you switch themes (falls back to a built-in deep-blue palette)
- **Undo** any Delete / Clear Done with `Ctrl+Z` or the header button
- **Due-date notifications** for overdue and due-today tasks (once a day)
- Keyboard-driven: `Ctrl+N` add, `Ctrl+F` search, `Ctrl+Shift+D` clear done, `Ctrl+Z` undo
- **One JSON file** — no database, no daemon, no server

## Requirements

- An Omarchy / Arch system with **GTK4** and **jansson**
- To build: `gcc` + `pkgconf` (`sudo pacman -S --needed gtk4 jansson`)

## Install

### Option A — package (recommended)

```bash
git clone https://github.com/dexter-morgan-oss/omarchy-tasks
cd omarchy-tasks
makepkg -si
tasks-setup-user          # wire up the launcher + Super+Shift+T
```

Once published to the AUR it is one command:

```bash
omarchy pkg aur add tasks-gui      # or: yay -S tasks-gui
```

### Option B — per-user (no root)

```bash
git clone https://github.com/dexter-morgan-oss/omarchy-tasks
cd omarchy-tasks
./install.sh
```

`install.sh` compiles the GUI, installs everything to `~/.local/bin`, installs
the app icon, and then runs `tasks-setup-user` for you. Make sure
`~/.local/bin` is on your `PATH`.

### What `tasks-setup-user` wires up

| Thing | Where |
|-------|-------|
| Launcher entry | `~/.local/share/applications/org.omarchy.tasks.desktop` |
| Omarchy menu row | `~/.config/omarchy/extensions/omarchy-menu.jsonc` (created if missing) |
| Keybinding | `SUPER + SHIFT + T` appended to `~/.config/hypr/bindings.lua` |

It is **idempotent** — re-run it any time to repair the integration. It backs up
`bindings.lua` before editing and validates the change with `hyprctl reload` +
`hyprctl configerrors`. Then launch with **Super + Shift + T** (pressing it again
focuses the existing window), or type `tasks` in the launcher.

## Use

| Action | How |
|--------|-----|
| Add | **click empty column space**, or `Ctrl+N` |
| Complete | **drag into Done** (drag back out to reopen) |
| Move / reorder | **drag** between or within columns |
| Priority | shown by **title colour**; click the **dot** to cycle high → normal → low |
| Rename | **double-click** a row, or right-click → Rename |
| Due date | right-click → Due date (`today` / `tomorrow` / `YYYY-MM-DD` / `off`) |
| Delete | right-click → Delete, or `Delete` / `Ctrl+D` |
| Undo | `Ctrl+Z`, or the **Undo** button in the header |
| Search | `Ctrl+F` |
| Clear Done | `Ctrl+Shift+D`, or right-click the Done header |

The add box understands `!high`, `.low`, and a trailing date:

```
!write release notes tomorrow    # ! = high priority, date at the end
.stock the pantry                # . = low priority
fix flaky test 2026-11-01        # plain + explicit date
```

Prefer the terminal? The CLI and GUI share one store:

```bash
tasks add "buy milk"              tasks add "ship v1" -p high --due tomorrow
tasks list [--all]                tasks done 2     # -> Done column
tasks open 2                      tasks rm 3
tasks clear-done
```

Running `tasks` with no arguments opens a vim-style TUI. See
[GUIDE.md](GUIDE.md) for the full walkthrough.

## Configuration

The GUI reads an optional ini file at
`~/.config/omarchy-tasks/config.ini` (created on demand — nothing is written
unless you make the file yourself):

```ini
[general]
follow_theme = true    # repaint using the active Omarchy theme (default true)
notify_due   = true    # desktop notification for due/overdue tasks (default true)
```

Both keys default to `true`, so the app follows your theme and reminds you about
due tasks out of the box. The `[general]` header is optional.

**Theming.** When `follow_theme` is on, the app parses the same
`colors.toml` Omarchy writes for your current theme (background, foreground,
accent, muted, red/green/yellow, …) and builds its stylesheet from it, keeping
the window translucent with GTK CSS only — no Hyprland config is touched. A
`GFileMonitor` watches the file, so switching themes with
`omarchy theme set …` recolours the window live. If the file is missing or
unreadable the built-in deep-blue palette is used.

**Due notifications.** At startup, and then every 15 minutes, the app sends a
single summary notification when any not-done task is due today or overdue —
at most once per calendar day. Set `notify_due = false` to disable it.

## Storage

Everything lives in one file: `~/.local/share/tasks/tasks.json`
(`$XDG_DATA_HOME/tasks/tasks.json`). Each task has a `status`
(`todo`/`progress`/`done`); the legacy `done` boolean stays in sync so the CLI
keeps working. Back it up by copying that one file.

## Uninstall

```bash
./uninstall.sh            # binaries, launcher entry, icon
./uninstall.sh --purge    # also remove the Super+Shift+T binding
./uninstall.sh --data     # also delete the task store
```

Or, if installed via `makepkg`: `sudo pacman -Rns tasks-gui`.

## Development

Build the GUI:

```bash
gcc -O2 -Wall -Wextra $(pkg-config --cflags gtk4 jansson) tasks-gui.c \
    -o tasks-gui $(pkg-config --libs gtk4 jansson) -lm
```

### Tests

Everything is covered by a single runner (stdlib Python 3 only — no pytest
needed). The GUI tests need a session with `WAYLAND_DISPLAY` or `DISPLAY`;
they self-skip when run headless.

```bash
bash tests/run_all.sh
```

| Suite | What it covers |
| --- | --- |
| `tests/test_taskstore.py` | storage layer: load/save round-trip, atomic writes, normalisation, `parse_due`, ids, sorting |
| `tests/test_cli.py` | `tasks` end-to-end: add / priority / due / list / done / open / rm / clear-done, exit codes |
| `tests/test_interop.py` | the CLI ↔ GUI `tasks.json` contract (field types, statuses, legacy `done`-only stores, due normalisation) |
| `tests/gui_selftest.sh` | builds `tasks-gui` with `-Wall -Wextra -Werror`, then runs its embedded selftests |

The GUI has two built-in self-test modes (**133 checks**; handlers are driven
directly so they run in a plain GTK loop). Besides the drag/keyboard/rename/
context-menu/JSON-contract paths, they cover the config parser, the Omarchy
`colors.toml` parser and generated CSS (including that a parsed palette is
installed into the live stylesheet), the Undo stack for Delete / Clear Done,
and the due-date notification selection, summary body and once-a-day gate:

```bash
# behavioural: drives the real handlers against a throwaway empty store
TASKS_SELFTEST=1 XDG_DATA_HOME=$(mktemp -d) ./tasks-gui

# load: asserts the GUI reads a CLI/foreign tasks.json correctly
TASKS_SELFTEST_LOAD=1 XDG_DATA_HOME=<dir-with-tasks.json> ./tasks-gui
```

Each prints `PASS`/`FAIL` lines and a summary, and exits non-zero on failure.

CI (`.github/workflows/ci.yml`) builds the GUI with `-Werror` and runs the
whole suite under Xvfb on every push and pull request.

## License

[MIT](LICENSE) © 2026 Ragul.
