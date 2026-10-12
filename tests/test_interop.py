#!/usr/bin/env python3
"""Interoperability tests: the CLI (tasks.py/taskstore.py) and the GUI
(tasks-gui.c) share ~/.local/share/tasks/tasks.json. These tests pin the
contract both sides rely on:

  id       int
  title    str
  done     bool          (mirrors status == "done")
  status   "todo" | "progress" | "done"
  priority "high" | "med" | "low"
  due      null | "YYYY-MM-DD"
  created  str

Run:  python3 tests/test_interop.py
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "tasks.py"
sys.path.insert(0, str(ROOT))
import taskstore  # noqa: E402

GUI_FIELDS = ("id", "title", "done", "status", "priority", "due", "created")
STATUSES = ("todo", "progress", "done")
PRIORITIES = ("high", "med", "low")


def gui_contract_errors(task: dict) -> list:
    """Return a list of contract violations for one task dict."""
    errs = []
    if not isinstance(task.get("id"), int):
        errs.append("id not int")
    if not isinstance(task.get("title"), str):
        errs.append("title not str")
    if not isinstance(task.get("done"), bool):
        errs.append("done not bool")
    if task.get("status") not in STATUSES:
        errs.append(f"status={task.get('status')!r}")
    if task.get("priority") not in PRIORITIES:
        errs.append(f"priority={task.get('priority')!r}")
    due = task.get("due")
    if due is not None and not (isinstance(due, str) and len(due) == 10
                                and due[4] == "-" and due[7] == "-"):
        errs.append(f"due={due!r}")
    if not isinstance(task.get("created"), str):
        errs.append("created not str")
    return errs


class InteropCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._old_xdg = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = self._tmp.name
        self.env = {**os.environ, "XDG_DATA_HOME": self._tmp.name}

    def tearDown(self):
        if self._old_xdg is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._old_xdg
        self._tmp.cleanup()

    @property
    def store_path(self) -> Path:
        return Path(self._tmp.name) / "tasks" / "tasks.json"

    def cli(self, *args, check=True):
        p = subprocess.run([sys.executable, str(CLI), *args],
                           capture_output=True, text=True, env=self.env)
        if check and p.returncode != 0:
            self.fail(f"tasks {' '.join(args)} failed: {p.stderr}")
        return p.stdout

    def write_store(self, obj):
        self.store_path.parent.mkdir(parents=True, exist_ok=True)
        self.store_path.write_text(json.dumps(obj, indent=2), encoding="utf-8")

    def read_store(self) -> dict:
        return json.loads(self.store_path.read_text(encoding="utf-8"))


class TestCliWritesGuiContract(InteropCase):
    def test_cli_add_produces_every_gui_field(self):
        self.cli("add", "ship the thing", "-p", "high", "--due", "2026-05-01")
        data = self.read_store()
        self.assertIn("next_id", data)
        self.assertEqual(len(data["tasks"]), 1)
        t = data["tasks"][0]
        self.assertEqual(sorted(t.keys()), sorted(GUI_FIELDS))
        self.assertEqual(gui_contract_errors(t), [])

    def test_cli_default_add_is_contract_clean(self):
        self.cli("add", "plain")
        t = self.read_store()["tasks"][0]
        self.assertEqual(gui_contract_errors(t), [])
        self.assertIsNone(t["due"])
        self.assertEqual(t["priority"], "med")

    def test_cli_status_changes_stay_consistent(self):
        self.cli("add", "x")
        self.cli("done", "1")
        t = self.read_store()["tasks"][0]
        self.assertTrue(t["done"])
        self.assertEqual(t["status"], "done")
        self.cli("open", "1")
        t = self.read_store()["tasks"][0]
        self.assertFalse(t["done"])
        self.assertEqual(t["status"], "todo")


class TestCliReadsGuiStore(InteropCase):
    """A store the GUI wrote (status can be "progress"; done mirrors status)."""

    def _gui_store(self):
        return {
            "next_id": 4,
            "tasks": [
                {"id": 1, "title": "todo item", "done": False, "status": "todo",
                 "priority": "high", "due": "2026-01-01", "created": "2026-01-01"},
                {"id": 2, "title": "wip item", "done": False, "status": "progress",
                 "priority": "med", "due": None, "created": "2026-01-02"},
                {"id": 3, "title": "done item", "done": True, "status": "done",
                 "priority": "low", "due": None, "created": "2026-01-03"},
            ],
        }

    def test_list_shows_progress_task_as_open(self):
        self.write_store(self._gui_store())
        out = self.cli("list", "--all")
        self.assertIn("todo item", out)
        self.assertIn("wip item", out)      # progress != done
        self.assertIn("done item", out)
        # default list (open only) hides only the done one
        out = self.cli("list")
        self.assertIn("wip item", out)
        self.assertNotIn("done item", out)

    def test_done_and_rm_work_on_gui_store(self):
        self.write_store(self._gui_store())
        self.assertEqual(self.cli("done", "2").strip(), "done 2: wip item")
        t = self.read_store()["tasks"][1]
        self.assertEqual(t["status"], "done")
        self.assertTrue(t["done"])
        self.assertEqual(self.cli("rm", "1").strip(), "removed 1: todo item")
        ids = [t["id"] for t in self.read_store()["tasks"]]
        self.assertEqual(ids, [2, 3])

    def test_legacy_done_only_store_normalises(self):
        self.write_store({"next_id": 3, "tasks": [
            {"id": 1, "title": "legacy open", "done": False},
            {"id": 2, "title": "legacy done", "done": True},
        ]})
        out = self.cli("list", "--all")
        self.assertIn("[ ] " , out)
        self.assertIn("[x] ", out)
        # after any save the status field is materialised
        self.cli("done", "1")
        data = self.read_store()
        self.assertEqual(data["tasks"][0]["status"], "done")


class TestDueNormalisation(InteropCase):
    def test_cli_normalises_non_padded_due(self):
        # the CLI rejects non-ISO dates; the GUI normalises them to ISO so both
        # sides always write zero-padded "YYYY-MM-DD".
        p = subprocess.run([sys.executable, str(CLI), "add", "x",
                            "--due", "2026-1-5"],
                           capture_output=True, text=True, env=self.env)
        self.assertNotEqual(p.returncode, 0)

    def test_taskstore_round_trip_is_contract_clean(self):
        data = taskstore.load()
        taskstore.new_task(data, "a", priority="high", due="2026-05-01")
        taskstore.new_task(data, "b", priority="low", status="progress")
        taskstore.save(data)
        for t in self.read_store()["tasks"]:
            self.assertEqual(gui_contract_errors(t), [],
                             msg=f"contract violated: {t}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
