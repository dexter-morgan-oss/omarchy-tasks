# tasks — User Guide

A minimal 3-column kanban task manager for Omarchy. Text and colour only (no
icons), smooth drag-and-drop, and a CLI for adding things from the shell. The
window is a small native C + GTK4 program, so it opens and responds instantly.

The three columns are **To Do**, **In Progress**, and **Done**.

---

## 1. Opening the app

| Way | How |
|-----|-----|
| **Keyboard** | `Super + Shift + T` — opens Tasks (press again to focus it) |
| **Launcher** | Open the launcher, type `tasks`, `todo`, or `task manager` |
| **Launcher route** | `omarchy menu summon tasks` |
| **Terminal** | `tasks-gui` (or `tasks` for the CLI/TUI) |

---

## 2. Your first 60 seconds

1. Press `Super + Shift + T`.
2. **Click an empty spot** in the **To Do** column — a small `add a task…` box
   opens right there. Type `!email boss` and press **Enter** — the `!` makes it
   high priority.
3. Click another empty spot and add `wash the car`.
4. **Drag** `wash the car` into the **In Progress** column.
5. When it's finished, **drag** `wash the car` once more into **Done**.
6. Changed your mind? **Drag** it back out of **Done** to reopen it.

Everything saves the moment you change it.

---

## 3. The board

Each column:

- has a coloured dot + title + a **count badge** so you can see load at a glance
- scrolls on its own when it fills up
- lets you add a task **anywhere** in its empty space

Column colours are only accents: blue (To Do), amber (In Progress), green (Done).
A task's **priority** is shown by its title colour — red (high), gray (normal),
green (low) — plus a small dot on the right; click the dot to cycle.

The board uses Omarchy's monospace face (**JetBrainsMono Nerd Font**) on a deep
blue **translucent** palette — the window and column panels are semi-transparent
(alpha-composited over the desktop), and the chrome is deliberately empty — no
toolbar buttons. A **rotating shortcut ticker** along the bottom of the window
shows the keys below one at a time, crossfading every few seconds (and pausing
while the window is unfocused). Search is `Ctrl + F`; clearing the Done column is
`Ctrl + Shift + D` or a **right-click on the Done column header**.

### Adding

- **Click any empty area** inside a column → an inline `add a task…` box appears
  at that spot. Type and press **Enter**. Press **Esc** to cancel.
- `Ctrl + N` opens the same box at the top of **To Do** any time.

The add box understands a shortcut syntax:

| Input | Meaning |
|-------|---------|
| `!ship v1` | high priority |
| `.stock pantry` | low priority |
| `review PR tomorrow` | due tomorrow (also `today` or `YYYY-MM-DD`) |

### Moving things

- **Drag** a task onto another column to change its status.
- **Drag** a task into **Done** to complete it; drag it back out to reopen.
- **Drag** within a column to reorder (drop above/below a neighbour).

### Editing a task

- **Double-click** a row to rename it inline.
- **Right-click** a row for a small menu:
  - **Rename** — edit the title inline
  - **Due date** — type `today`, `tomorrow`, `YYYY-MM-DD`, or `off` to clear
  - **Delete** — remove the task
- Click the coloured **dot** on a row to cycle its priority.

Deleted something by mistake? Press `Ctrl + Z` to bring back the last
**Delete** or **Clear Completed**. (Completion itself is drag-only — the
keyboard does not move tasks between columns.)

### Keys

| Key | Action |
|-----|--------|
| `Ctrl + N` | open the add box at the top of To Do |
| `Ctrl + F` | search across all columns |
| `Ctrl + Shift + D` | clear completed (also: right-click the Done header) |
| `Ctrl + Z` | undo the last Delete / Clear Completed |
| `Delete`, `Backspace`, `Ctrl + D` | delete the selected task (any column) |
| `Esc` | cancel the add box / close search |
| `F11` | toggle fullscreen |

**Click a card to select it**, then press `Delete` to remove it. Clicking moves
focus to that column, so `Delete` always targets the card you clicked — even if
another column still showed a selection. (Deleting also works from the card's
right-click menu.)

Completion is **drag-only** — no key moves a task to **Done** (so adding a
task and pressing `Enter` twice can no longer send it to the Done column).

---

## 4. CLI

The GUI and CLI share the same store, so terminal and board stay in sync:

```bash
tasks add "buy milk"                            # goes to To Do
tasks add "ship v1" -p high --due tomorrow
tasks add "review PR" --due 2026-10-15
tasks list                                      # not-done tasks
tasks list --all                                # everything
tasks done 2                                    # -> moves to Done
tasks open 2                                    # -> back to To Do
tasks rm 3
tasks clear-done
```

A vim-style terminal TUI is still bundled: run `tasks` with no arguments
(`j`/`k` move, `a` add, `x` toggle, `?` help).

---

## 5. Notes

- Storage: `~/.local/share/tasks/tasks.json` (no database, no daemon).
- **Theme**: the window follows the active Omarchy theme (colours read from
  `~/.local/state/omarchy/current/theme/colors.toml`) and recolours live when
  you switch themes. Toggle with `follow_theme` in
  `~/.config/omarchy-tasks/config.ini`.
- **Font.** The board uses the Omarchy mono font (JetBrainsMono Nerd Font) at
  13px by default. Set `font` / `font_size` in the same config file to change
  the family or scale the whole type ramp — no rebuild needed.
- **Due reminders**: a desktop notification lists overdue / due-today tasks,
  once a day. Toggle with `notify_due` in the same config file.
- The GUI is single-instance: pressing `Super + Shift + T` while it is open
  focuses the existing window.
- App class: `org.omarchy.tasks` (used for keybind focus matching).
- Window is resizable — columns grow and shrink to fit.
