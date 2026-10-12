#!/usr/bin/env python3
"""End-to-end tests for the `tasks` CLI (tasks.py), run in an isolated
XDG_DATA_HOME so the real store is never touched.

Run:  python3 tests/test_cli.py
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from datetime import date, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLI = ROOT / "tasks.py"


class CliTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.env = {**os.environ, "XDG_DATA_HOME": self._tmp.name}

    def tearDown(self):
        self._tmp.cleanup()

    def data_file(self) -> Path:
        return Path(self._tmp.name) / "tasks" / "tasks.json"

    def run_cli(self, *args, check=True):
        p = subprocess.run([sys.executable, str(CLI), *args],
                           capture_output=True, text=True, env=self.env)
        if check and p.returncode != 0:
            self.fail(f"tasks {' '.join(args)} failed ({p.returncode}): {p.stderr}")
        return p

    def cli(self, *args):
        # NOTE: deliberately not named run() — that would shadow
        # unittest.TestCase.run and stop the test runner from working.
        return self.run_cli(*args).stdout

    def store(self):
        return json.loads(self.data_file().read_text(encoding="utf-8"))

    def write_store(self, obj):
        self.data_file().parent.mkdir(parents=True, exist_ok=True)
        self.data_file().write_text(json.dumps(obj), encoding="utf-8")


class TestVersion(CliTestCase):
    def test_version(self):
        self.assertEqual(self.cli("version").strip(), "1.1.0")


class TestAdd(CliTestCase):
    def test_add_prints_and_persists(self):
        self.assertEqual(self.cli("add", "write the tests").strip(),
                         "added 1: write the tests")
        data = self.store()
        self.assertEqual(data["next_id"], 2)
        t = data["tasks"][0]
        self.assertEqual(t["title"], "write the tests")
        self.assertEqual(t["status"], "todo")
        self.assertFalse(t["done"])
        self.assertEqual(t["priority"], "med")
        self.assertIsNone(t["due"])
        self.assertEqual(t["created"], date.today().isoformat())

    def test_add_with_priority_and_due(self):
        self.cli("add", "ship it", "-p", "high", "--due", "2026-05-01")
        t = self.store()["tasks"][0]
        self.assertEqual(t["priority"], "high")
        self.assertEqual(t["due"], "2026-05-01")

    def test_add_due_tomorrow(self):
        self.cli("add", "x", "--due", "tomorrow")
        self.assertEqual(self.store()["tasks"][0]["due"],
                         (date.today() + timedelta(days=1)).isoformat())

    def test_add_due_off_clears(self):
        self.cli("add", "x", "--due", "off")
        self.assertIsNone(self.store()["tasks"][0]["due"])

    def test_add_invalid_due_fails(self):
        p = self.run_cli("add", "x", "--due", "nope", check=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("invalid due date", p.stderr)
        self.assertFalse(self.data_file().exists())

    def test_ids_increment(self):
        self.cli("add", "one")
        self.cli("add", "two")
        self.assertEqual([t["id"] for t in self.store()["tasks"]], [1, 2])


class TestList(CliTestCase):
    def test_list_empty(self):
        self.assertIn("no tasks", self.cli("list"))

    def test_list_shows_open_by_default(self):
        self.cli("add", "open task")
        self.cli("add", "done task")
        self.cli("done", "2")
        out = self.cli("list")
        self.assertIn("open task", out)
        self.assertNotIn("done task", out)

    def test_list_all_includes_done(self):
        self.cli("add", "a")
        self.cli("add", "b")
        self.cli("done", "2")
        out = self.cli("list", "--all")
        self.assertIn("a", out)
        self.assertIn("b", out)

    def test_list_done_only(self):
        self.cli("add", "a")
        self.cli("add", "b")
        self.cli("done", "1")
        out = self.cli("list", "--done")
        self.assertIn("a", out)
        self.assertNotIn("b", out)


class TestToggle(CliTestCase):
    def test_done_and_open(self):
        self.cli("add", "x")
        self.assertEqual(self.cli("done", "1").strip(), "done 1: x")
        t = self.store()["tasks"][0]
        self.assertTrue(t["done"])
        self.assertEqual(t["status"], "done")
        self.assertEqual(self.cli("open", "1").strip(), "reopened 1: x")
        t = self.store()["tasks"][0]
        self.assertFalse(t["done"])
        self.assertEqual(t["status"], "todo")

    def test_missing_id_errors(self):
        p = self.run_cli("done", "99", check=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("99", p.stderr)


class TestRemoveClear(CliTestCase):
    def test_rm(self):
        self.cli("add", "x")
        self.assertEqual(self.cli("rm", "1").strip(), "removed 1: x")
        self.assertEqual(self.store()["tasks"], [])

    def test_rm_missing_errors(self):
        p = self.run_cli("rm", "99", check=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("99", p.stderr)

    def test_clear_done(self):
        self.cli("add", "a")
        self.cli("add", "b")
        self.cli("done", "2")
        self.assertEqual(self.cli("clear-done").strip(),
                         "cleared 1 completed task(s)")
        self.assertEqual([t["id"] for t in self.store()["tasks"]], [1])

    def test_clear_when_none(self):
        self.assertEqual(self.cli("clear-done").strip(), "nothing to clear")


class TestErrors(CliTestCase):
    def test_unknown_command_exits_2(self):
        p = self.run_cli("frobnicate", check=False)
        self.assertEqual(p.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
