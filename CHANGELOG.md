# Changelog

All notable changes to Tasks are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Screenshots in the README (`docs/`).
- A `Makefile` (`make`, `make test`, `make install`, `make clean`).
- This changelog, plus GitHub issue/PR templates, `CONTRIBUTING.md` and
  `SECURITY.md`.

### Changed

- Install section simplified: one primary path, alternatives collapsed.
- Copyright and maintainer name updated to **P Ragul**.

## [1.2.0] - 2026-10-13

### Changed

- Replaced the static shortcuts strip with a **rotating, crossfading hint
  ticker** — one tip at a time, like a game loading screen — so the board stays
  uncluttered.
- Rewrote the README around a clearer install and feature summary.

## [1.1.0] - 2026-10-13

### Added

- **Omarchy theme following** — reads the active theme's `colors.toml` and
  recolours live when you switch, with a built-in palette as fallback.
- **Undo** (`Ctrl+Z`) for Delete and Clear Done.
- **Due-date notifications** — once a day for overdue and due-today tasks.
- **Configuration** in `~/.config/omarchy-tasks/config.ini`: `font`,
  `font_size`, `follow_theme`, `notify_due`.
- A full **test suite** covering storage, the CLI, CLI/GUI interop, and the GUI
  itself (embedded checks driving its real handlers).

### Changed

- Completion is **drag-only** — no key moves a task between columns.
- Removed the Undo button; `Ctrl+Z` is the single undo entry point.
- Unified font handling; every size now derives from `font_size`.

### Fixed

- Invisible UI when the theme's foreground colour is light (e.g. Vantablack).
- Segfault when setting a due date from the card's context menu.
- The due-date popover could freeze input.
- Delete now works on the clicked card, including cards in the Done column.
- Storage hardened: atomic writes (temp file, `fsync`, `rename`), a corrupt
  store is quarantined to `tasks.json.corrupt-<stamp>` instead of overwritten,
  and task ids are re-derived from the true maximum.

## [1.0.0] - 2026-10-12

### Added

- Initial release: a three-column kanban board (**To Do · In Progress · Done**)
  as a native **GTK4** app in C, plus a zero-dependency **CLI/TUI** sharing the
  same JSON store.
- Drag to move and complete, click-empty-space or `Ctrl+N` to add, priority
  shown by title colour and a clickable dot, and due dates via right-click.
- Priority and date markers in the add box (`!` high, `.` low, trailing date).
- Per-user desktop integration: launcher entry, Omarchy menu row and the
  `Super + Shift + T` keybinding.
- An Arch `PKGBUILD`.

[Unreleased]: https://github.com/dexter-morgan-oss/omarchy-tasks/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/dexter-morgan-oss/omarchy-tasks/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/dexter-morgan-oss/omarchy-tasks/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/dexter-morgan-oss/omarchy-tasks/releases/tag/v1.0.0
