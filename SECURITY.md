# Security

## Reporting a vulnerability

Please report security issues **privately**, not as a public issue:

- Open a draft advisory:
  <https://github.com/dexter-morgan-oss/omarchy-tasks/security/advisories/new>
- Or, if you prefer, email the maintainer at the address in `git log`.

You'll get an acknowledgement as soon as possible. Please include steps to
reproduce and the version (`tasks version`).

## Supported versions

Only the latest release is supported. If you're on an older version, please
upgrade first.

## Scope and design

Tasks is deliberately small and local:

- **No network.** The GUI and CLI never open a network connection. Your data
  never leaves the machine.
- **One local file.** Everything lives in
  `$XDG_DATA_HOME/tasks/tasks.json`. No database, no daemon, no server.
- **No spawning.** Task text is only ever rendered as text; it is never passed
  to a shell.

The main things worth knowing:

- The store is plain JSON owned by your user. Protect it with normal file
  permissions (it lives under your home directory).
- The per-user installer writes launcher, menu and keybinding entries under
  `~/.config` and `~/.local`; it never needs root.
- If you install the system package, `tasks-setup-user` is what wires up the
  desktop for your user.
