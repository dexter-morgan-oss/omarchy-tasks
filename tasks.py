#!/usr/bin/env python3
"""tasks — a minimal task manager for the terminal.

Zero dependencies (Python stdlib only). Terminal-first, so it fits
Omarchy's Hyprland/terminal workflow and matches whatever theme you run.

Storage: ~/.local/share/tasks/tasks.json   (respects $XDG_DATA_HOME)

Usage:
    tasks                 launch the interactive TUI (default)
    tasks add TITLE [-p high|med|low] [--due DATE]
    tasks list [--all] [--done]
    tasks done ID
    tasks open ID
    tasks rm ID
    tasks clear-done
    tasks version
"""

from __future__ import annotations

import argparse
import curses
import sys
from datetime import date
from pathlib import Path

# The packaged layout keeps taskstore.py in a libdir; make it importable there.
for _lib in (Path(__file__).resolve().parent, Path("/usr/lib/omarchy-tasks")):
    if (_lib / "taskstore.py").exists() and str(_lib) not in sys.path:
        sys.path.insert(0, str(_lib))

from taskstore import (
    PRIORITIES,
    find_task,
    load,
    new_task,
    parse_due,
    save,
    sort_tasks,
)

APP = "tasks"
VERSION = "1.2.0"

PRIORITY_MARK = {"high": "!", "med": " ", "low": "."}

HELP_TEXT = """\
  Key bindings
  -------------------------------------------
    j / down      move down        k / up      move up
    g / G         jump top / bottom
    a             add task         e           edit title
    x / Space     toggle done      d           delete task
    p             cycle priority   n           set / clear due date
    f             cycle filter (active / all / done)
    /             search           ESC         clear search
    c             clear completed  ?           this help
    q             quit
  -------------------------------------------
    due dates: YYYY-MM-DD, today, tomorrow, or off
"""


def die(msg: str) -> None:
    print(f"tasks: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- CLI

def cmd_add(args, data: dict) -> None:
    if args.due is not None:
        due = parse_due(args.due)
        if due is False:
            die(f"invalid due date: {args.due!r} (use YYYY-MM-DD, today, or tomorrow)")
    else:
        due = None
    t = new_task(data, args.title, args.priority, due)
    save(data)
    print(f"added {t['id']}: {t['title']}")


def print_tasks(tasks) -> None:
    for t in tasks:
        mark = "x" if t["done"] else " "
        prio = PRIORITY_MARK.get(t.get("priority", "med"), " ")
        if t.get("due") and not t["done"] and t["due"] < date.today().isoformat():
            due = f"OVERDUE {t['due']}"
        elif t.get("due"):
            due = t["due"]
        else:
            due = ""
        line = f"[{mark}] {prio} {t['id']:>3}  {t['title']}"
        if due:
            line = f"{line:<48} {due}".rstrip()
        print(line)


def cmd_list(args, data: dict) -> None:
    tasks = sort_tasks(data["tasks"])
    if args.done:
        tasks = [t for t in tasks if t["done"]]
    elif not args.all:
        tasks = [t for t in tasks if not t["done"]]
    if not tasks:
        print("no tasks" + ("" if args.all else " (see 'tasks list --all')"))
        return
    print_tasks(tasks)


def cmd_toggle_state(args, data: dict, done: bool) -> None:
    t = find_task(data, args.id)
    if t is None:
        die(f"no task with id {args.id}")
    t["done"] = done
    t["status"] = "done" if done else "todo"
    save(data)
    print(f"{'done' if done else 'reopened'} {t['id']}: {t['title']}")


def cmd_rm(args, data: dict) -> None:
    t = find_task(data, args.id)
    if t is None:
        die(f"no task with id {args.id}")
    data["tasks"].remove(t)
    save(data)
    print(f"removed {t['id']}: {t['title']}")


def cmd_clear(data: dict) -> None:
    done = [t for t in data["tasks"] if t["done"]]
    data["tasks"] = [t for t in data["tasks"] if not t["done"]]
    if done:
        save(data)
        print(f"cleared {len(done)} completed task(s)")
    else:
        print("nothing to clear")


# ---------------------------------------------------------------- TUI

class TUI:
    def __init__(self, stdscr, data: dict):
        self.stdscr = stdscr
        self.data = data
        self.filter = "active"      # active | all | done
        self.query = ""
        self.sel = 0
        self.help_open = False
        self.status = ""
        try:
            curses.curs_set(0)
        except curses.error:
            pass

    # -- helpers -----------------------------------------------------

    def visible(self) -> list:
        out = []
        q = self.query.lower()
        for t in sort_tasks(self.data["tasks"]):
            if self.filter == "active" and t["done"]:
                continue
            if self.filter == "done" and not t["done"]:
                continue
            if q and q not in t["title"].lower():
                continue
            out.append(t)
        return out

    def clamp_sel(self, n: int) -> None:
        self.sel = max(0, min(self.sel, max(n - 1, 0)))

    def input_line(self, prompt: str, initial: str = "") -> str | None:
        """Edit a line at the bottom of the screen.

        Enter -> the string (possibly empty); ESC -> None (cancel).
        """
        s = initial
        while True:
            h, w = self.stdscr.getmaxyx()
            self.stdscr.move(h - 1, 0)
            self.stdscr.clrtoeol()
            try:
                self.stdscr.addnstr(h - 1, 0, prompt + s, w - 1)
            except curses.error:
                pass
            self.stdscr.refresh()
            c = self.stdscr.getch()
            if c in (10, 13, curses.KEY_ENTER):
                return s
            if c == 27:  # ESC
                return None
            if c in (curses.KEY_BACKSPACE, 127, 8):
                s = s[:-1]
            elif 32 <= c <= 126:
                if len(s) < w - len(prompt) - 1:
                    s += chr(c)

    def confirm(self, msg: str) -> bool:
        """Single-key confirmation: y=yes, anything else=no."""
        self.status = msg + " (y/n)"
        self.render()
        while True:
            c = self.stdscr.getch()
            if c in (ord("y"), ord("Y")):
                return True
            if c in (ord("n"), ord("N"), 27, 10, 13):
                return False

    def format_task(self, t: dict, width: int) -> str:
        mark = "[x]" if t["done"] else "[ ]"
        prio = PRIORITY_MARK.get(t.get("priority", "med"), " ")
        meta = ""
        if t.get("due"):
            if not t["done"] and t["due"] < date.today().isoformat():
                meta = f"OVERDUE {t['due']}"
            else:
                meta = t["due"]
        line = f"{mark} {prio} {t['id']:>3}  {t['title']}"
        if meta and len(line) + len(meta) < width:
            line += " " * (width - len(line) - len(meta)) + meta
        return line[:width]

    # -- rendering ---------------------------------------------------

    def render(self) -> None:
        h, w = self.stdscr.getmaxyx()
        self.stdscr.erase()
        bottom = h - 1

        if self.help_open:
            for i, line in enumerate(HELP_TEXT.splitlines()):
                try:
                    self.stdscr.addnstr(min(h - 1, i + 1), 0, line, w - 1)
                except curses.error:
                    pass
            self.stdscr.addnstr(bottom, 0, " press ? or q to close ", w - 1, curses.A_REVERSE)
            self.stdscr.refresh()
            return

        # header
        head = f" {APP} — {self.filter}"
        if self.query:
            head += f"  /{self.query}"
        v = self.visible()
        n_open = sum(1 for t in v if not t["done"])
        tail = f" {n_open} open / {len(v)} shown "
        try:
            self.stdscr.addnstr(0, 0, head, w - 1, curses.A_BOLD)
            tail = f"{tail:>{len(tail)}}"
            self.stdscr.addnstr(0, w - len(tail), tail, w - 1, curses.A_DIM)
        except curses.error:
            pass

        # body
        for i, t in enumerate(v):
            if i >= bottom - 1:
                break
            attr = curses.A_NORMAL
            if t["done"]:
                attr = curses.A_DIM
            elif t.get("due") and t["due"] < date.today().isoformat():
                attr = curses.A_BOLD
            if i == self.sel:
                attr |= curses.A_REVERSE
            try:
                self.stdscr.addnstr(i + 1, 0, self.format_task(t, w), w - 1, attr)
            except curses.error:
                pass

        # status bar
        if self.status:
            left = self.status
            self.status = ""
        else:
            left = "a:add  x:done  e:edit  d:del  p:prio  n:due  f:filter  /:search  c:clear  ?:help  q:quit"
        try:
            self.stdscr.addnstr(bottom, 0, left, w - 1, curses.A_REVERSE)
        except curses.error:
            pass
        self.stdscr.refresh()

    # -- keys --------------------------------------------------------

    def handle_key(self, c: int) -> bool:
        """Return False to quit."""
        v = self.visible()
        n = len(v)

        if c == ord("q"):
            return False
        if c in (ord("j"), curses.KEY_DOWN):
            self.sel = min(self.sel + 1, max(n - 1, 0))
        elif c in (ord("k"), curses.KEY_UP):
            self.sel = max(self.sel - 1, 0)
        elif c in (curses.KEY_HOME, ord("g")):
            self.sel = 0
        elif c in (curses.KEY_END, ord("G")):
            self.sel = max(n - 1, 0)
        elif c == curses.KEY_PPAGE:
            self.sel = max(self.sel - (self.h - 3), 0)
        elif c == curses.KEY_NPAGE:
            self.sel = min(self.sel + (self.h - 3), max(n - 1, 0))
        elif c == ord("a"):
            title = self.input_line("title: ")
            if title is not None and title.strip():
                t = new_task(self.data, title.strip())
                self.sel = len(self.visible()) - 1
                self.status = f"added {t['id']}"
                save(self.data)
        elif c in (ord("x"), ord(" ")):
            if n:
                t = v[self.sel]
                t["done"] = not t["done"]
                t["status"] = "done" if t["done"] else "todo"
                self.status = f"{'done' if t['done'] else 'reopened'} {t['id']}"
                save(self.data)
        elif c == ord("e"):
            if n:
                t = v[self.sel]
                new = self.input_line("edit: ", initial=t["title"])
                if new is not None and new.strip():
                    t["title"] = new.strip()
                    save(self.data)
        elif c == ord("d"):
            if n:
                t = v[self.sel]
                self.data["tasks"].remove(t)
                self.clamp_sel(self.visible().__len__())
                save(self.data)
                self.status = f"removed {t['id']}"
        elif c == ord("p"):
            if n:
                t = v[self.sel]
                cur = t.get("priority", "med")
                t["priority"] = PRIORITIES[(PRIORITIES.index(cur) + 1) % len(PRIORITIES)]
                save(self.data)
        elif c == ord("n"):
            if n:
                t = v[self.sel]
                parsed = parse_due(self.input_line("due (YYYY-MM-DD / today / tomorrow / off): ",
                                                    initial=t.get("due") or ""))
                if parsed is False:
                    self.status = "invalid due date"
                else:
                    t["due"] = parsed
                    save(self.data)
        elif c == ord("f"):
            self.filter = {"active": "all", "all": "done", "done": "active"}[self.filter]
            self.sel = 0
        elif c == ord("/"):
            q = self.input_line("search: ", initial=self.query)
            if q is not None:
                self.query = q
            self.sel = 0
        elif c == 27:  # ESC clears search
            self.query = ""
            self.sel = 0
        elif c == ord("c"):
            done = [t for t in self.data["tasks"] if t["done"]]
            if done and self.confirm(f"clear {len(done)} completed task(s)?"):
                self.data["tasks"] = [t for t in self.data["tasks"] if not t["done"]]
                self.clamp_sel(self.visible().__len__())
                save(self.data)
                self.status = f"cleared {len(done)} task(s)"
            elif not done:
                self.status = "nothing to clear"
        elif c in (ord("?"),):
            self.help_open = True
        return True

    # -- main loop ---------------------------------------------------

    def run(self) -> None:
        while True:
            self.h, self.w = self.stdscr.getmaxyx()
            self.render()
            try:
                c = self.stdscr.getch()
            except KeyboardInterrupt:
                return
            if c == -1:
                continue
            if self.help_open:
                if c in (ord("q"), ord("?"), 27):
                    self.help_open = False
                continue
            if not self.handle_key(c):
                break
        save(self.data)


# ---------------------------------------------------------------- entry

def main(argv=None) -> None:
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(
        prog="tasks",
        description="minimal terminal task manager",
        epilog="run 'tasks' with no command to open the interactive TUI",
    )
    sub = parser.add_subparsers(dest="cmd")

    p = sub.add_parser("add", help="add a task")
    p.add_argument("title")
    p.add_argument("-p", "--priority", choices=PRIORITIES, default="med")
    p.add_argument("--due", help="YYYY-MM-DD, today, or tomorrow")

    p = sub.add_parser("list", help="list tasks (active by default)")
    p.add_argument("--all", action="store_true", help="show all tasks")
    p.add_argument("--done", action="store_true", help="show completed tasks")

    p = sub.add_parser("done", help="mark a task as done")
    p.add_argument("id", type=int)

    p = sub.add_parser("open", help="reopen a completed task")
    p.add_argument("id", type=int)

    p = sub.add_parser("rm", help="delete a task")
    p.add_argument("id", type=int)

    sub.add_parser("clear-done", help="remove all completed tasks")
    sub.add_parser("version", help="print version")

    args = parser.parse_args(argv)
    data = load()

    if args.cmd is None:
        try:
            curses.wrapper(TUI, data).run()
        except KeyboardInterrupt:
            pass
        return

    if args.cmd == "version":
        print(VERSION)
    elif args.cmd == "add":
        cmd_add(args, data)
    elif args.cmd == "list":
        cmd_list(args, data)
    elif args.cmd == "done":
        cmd_toggle_state(args, data, True)
    elif args.cmd == "open":
        cmd_toggle_state(args, data, False)
    elif args.cmd == "rm":
        cmd_rm(args, data)
    elif args.cmd == "clear-done":
        cmd_clear(data)
    else:
        parser.error(f"unknown command: {args.cmd}")


if __name__ == "__main__":
    main()