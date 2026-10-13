# Contributing

Thanks for helping improve Tasks! Bug reports, ideas and pull requests are all
welcome. This is a small, deliberately simple app — the goal is to keep it
fast, native and uncluttered.

## Ways to help

- **Report a bug** — use the
  [bug report form](https://github.com/dexter-morgan-oss/omarchy-tasks/issues/new/choose).
  Real-world reports (a theme that breaks, a drag that misbehaves, an install
  that fails) are the most valuable thing you can send.
- **Request a feature** — open a feature request, or start a
  [Discussion](https://github.com/dexter-morgan-oss/omarchy-tasks/discussions)
  if you're unsure.
- **Send a pull request** — small, focused changes are easiest to review.
- **Say hello on Discord** — the [Omarchy Discord](https://discord.gg/tXFUdasqhY)
  is a great place to share what you built or find a bug.

## Build and test

```bash
sudo pacman -S --needed gtk4 jansson gcc pkgconf python
make            # build the GUI
make test       # run the whole test suite
```

`make test` runs the storage, CLI, CLI/GUI-interop and GUI suites. The GUI suite
drives the real handlers through 156 embedded checks and skips cleanly when
there's no display.

Before opening a PR, please make sure `make` is warning-free and `make test`
passes.

## Layout

| Path | What it is |
|------|------------|
| `tasks-gui.c` | the whole GTK4 GUI (C, no build system beyond a one-line `gcc`) |
| `tasks.py` | the CLI and the curses TUI |
| `taskstore.py` | shared JSON store: load, save, ids, dates |
| `tasks-setup-user` | per-user desktop integration |
| `install.sh` / `uninstall.sh` | per-user install helpers |
| `PKGBUILD` / `.SRCINFO` | the Arch package |
| `tests/` | the test suite |

The GUI and CLI both read and write `$XDG_DATA_HOME/tasks/tasks.json`, so any
change to the on-disk format must keep both sides compatible and keep the tests
green.

## Good to know

- **Keep it light.** No new runtime dependencies without a good reason; the CLI
  is intentionally stdlib-only.
- **`-Werror` is on** in CI, so warnings fail the build.
- **Match the surrounding style** — the codebase is small and consistent; try
  to follow what's already there.

## Releasing (maintainers)

1. Bump `VERSION` in **`tasks-gui.c`** and **`tasks.py`**.
2. Update `CHANGELOG.md` (move *Unreleased* into a dated version).
3. Commit, then tag and push: `git tag -a vX.Y.Z -m "Tasks vX.Y.Z" && git push origin main vX.Y.Z`.
4. Bump `pkgver` in **`PKGBUILD`**, point `source` at the new tag, then
   `makepkg --printsrcinfo > .SRCINFO`.
5. Get the tag tarball checksum:
   `curl -L https://github.com/dexter-morgan-oss/omarchy-tasks/archive/vX.Y.Z.tar.gz | sha256sum`
   and paste it into `sha256sums`.
6. `makepkg -f` once to confirm it builds, commit the `PKGBUILD`/`.SRCINFO`
   bump, and push.
7. Create the GitHub release and (once published) update the AUR package.

> Note: GitHub tag archives embed the commit id in the tarball header, so **any
> rewrite of a tagged commit changes the checksum** and step 5 must be redone.
