#!/usr/bin/env python3
"""Unit tests for taskstore.py — the storage layer shared by the CLI and GUI.

Run:  python3 tests/test_taskstore.py
"""

import json
import os
import sys
import tempfile
import unittest
from datetime import date, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import taskstore  # noqa: E402


class IsolatedDataHome(unittest.TestCase):
    """Point XDG_DATA_HOME at a throwaway dir so tests never touch real data."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._old = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = self._tmp.name

    def tearDown(self):
        if self._old is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._old
        self._tmp.cleanup()

    @property
    def data_file(self) -> Path:
        return Path(self._tmp.name) / "tasks" / "tasks.json"


class TestDataFile(IsolatedDataHome):
    def test_honours_xdg_data_home(self):
        self.assertEqual(taskstore.data_file(), self.data_file)

    def test_default_is_local_share(self):
        os.environ.pop("XDG_DATA_HOME", None)
        expected = Path.home() / ".local/share/tasks/tasks.json"
        self.assertEqual(taskstore.data_file(), expected)


class TestLoadSave(IsolatedDataHome):
    def test_load_missing_returns_empty_default(self):
        self.assertEqual(taskstore.load(), {"next_id": 1, "tasks": []})

    def test_load_malformed_json_returns_default(self):
        self.data_file.parent.mkdir(parents=True)
        self.data_file.write_text("{not json", encoding="utf-8")
        self.assertEqual(taskstore.load(), {"next_id": 1, "tasks": []})

    def test_malformed_json_is_quarantined_not_deleted(self):
        self.data_file.parent.mkdir(parents=True)
        self.data_file.write_text("{not json", encoding="utf-8")
        taskstore.load()
        backups = list(self.data_file.parent.glob("tasks.json.corrupt-*"))
        self.assertEqual(len(backups), 1, "corrupt store should be backed up")
        self.assertEqual(baks := backups[0].read_text(encoding="utf-8"),
                         "{not json")
        # the original is moved aside, so a later save starts from a clean file
        self.assertFalse(self.data_file.exists())

    def test_load_non_dict_payload_is_quarantined(self):
        self.data_file.parent.mkdir(parents=True)
        self.data_file.write_text("[1, 2, 3]", encoding="utf-8")
        self.assertEqual(taskstore.load(), {"next_id": 1, "tasks": []})
        self.assertEqual(len(list(self.data_file.parent.glob("*.corrupt-*"))), 1)

    def test_load_bumps_stale_next_id(self):
        self.data_file.parent.mkdir(parents=True)
        self.data_file.write_text(json.dumps({
            "next_id": 1,
            "tasks": [{"id": 1, "title": "a"}, {"id": 5, "title": "b"}],
        }), encoding="utf-8")
        data = taskstore.load()
        self.assertEqual(data["next_id"], 6)

    def test_save_bumps_stale_next_id(self):
        data = {"next_id": 2, "tasks": [{"id": 9, "title": "b"}]}
        taskstore.save(data)
        again = json.loads(self.data_file.read_text(encoding="utf-8"))
        self.assertEqual(again["next_id"], 10)

    def test_save_then_load_round_trip(self):
        data = {"next_id": 1, "tasks": []}
        taskstore.new_task(data, "write docs", priority="high", due="2026-05-01")
        taskstore.save(data)
        again = taskstore.load()
        self.assertEqual(again["next_id"], 2)
        self.assertEqual(len(again["tasks"]), 1)
        t = again["tasks"][0]
        self.assertEqual(t["title"], "write docs")
        self.assertEqual(t["priority"], "high")
        self.assertEqual(t["due"], "2026-05-01")
        self.assertFalse(t["done"])
        self.assertEqual(t["status"], "todo")

    def test_save_leaves_no_tmp_file(self):
        taskstore.save({"next_id": 1, "tasks": []})
        self.assertEqual(list(self.data_file.parent.glob("*.tmp")), [])

    def test_save_normalises_before_writing(self):
        data = {"next_id": 1, "tasks": [
            {"id": 1, "title": "x", "done": True, "priority": "med", "due": None},
        ]}
        taskstore.save(data)
        on_disk = json.loads(self.data_file.read_text(encoding="utf-8"))
        self.assertEqual(on_disk["tasks"][0]["status"], "done")

    def test_load_normalises_legacy_done_only_tasks(self):
        self.data_file.parent.mkdir(parents=True)
        self.data_file.write_text(json.dumps({
            "next_id": 3,
            "tasks": [
                {"id": 1, "title": "legacy open", "done": False},
                {"id": 2, "title": "legacy done", "done": True},
            ],
        }), encoding="utf-8")
        data = taskstore.load()
        self.assertEqual(data["tasks"][0]["status"], "todo")
        self.assertEqual(data["tasks"][1]["status"], "done")

    def test_save_creates_parent_dirs(self):
        self.assertFalse(self.data_file.parent.exists())
        taskstore.save({"next_id": 1, "tasks": []})
        self.assertTrue(self.data_file.exists())


class TestParseDue(unittest.TestCase):
    def test_cleared_forms(self):
        for v in ("", None, "  ", "off", "OFF", "none", "-"):
            self.assertIsNone(taskstore.parse_due(v), msg=repr(v))

    def test_today_and_tomorrow(self):
        today = date.today().isoformat()
        tomorrow = (date.today() + timedelta(days=1)).isoformat()
        self.assertEqual(taskstore.parse_due("today"), today)
        self.assertEqual(taskstore.parse_due("TODAY"), today)
        self.assertEqual(taskstore.parse_due("tomorrow"), tomorrow)
        self.assertEqual(taskstore.parse_due("  tomorrow "), tomorrow)

    def test_iso_and_whitespace(self):
        self.assertEqual(taskstore.parse_due("2026-12-31"), "2026-12-31")
        self.assertEqual(taskstore.parse_due(" 2026-01-05 "), "2026-01-05")

    def test_invalid_returns_false(self):
        for v in ("garbage", "2026-13-01", "2026-1-5", "01-02-2026", "2026/01/05"):
            self.assertIs(taskstore.parse_due(v), False, msg=repr(v))

    def test_leap_and_calendar_validation(self):
        self.assertEqual(taskstore.parse_due("2024-02-29"), "2024-02-29")
        self.assertIs(taskstore.parse_due("2026-02-29"), False)

    def test_normalises_iso_basic_form(self):
        # date.fromisoformat accepts the compact form; parse_due normalises it
        self.assertEqual(taskstore.parse_due("20260105"), "2026-01-05")


class TestNormalize(unittest.TestCase):
    def test_status_wins_over_done(self):
        t = {"status": "progress", "done": True}
        taskstore.normalize(t)
        self.assertEqual(t["status"], "progress")
        self.assertFalse(t["done"])

    def test_done_only_sets_status(self):
        t = {"done": True}
        taskstore.normalize(t)
        self.assertEqual(t["status"], "done")

    def test_unknown_status_falls_back(self):
        t = {"status": "banana", "done": True}
        taskstore.normalize(t)
        self.assertEqual(t["status"], "done")


class TestNewTask(unittest.TestCase):
    def test_fields_and_id_increment(self):
        data = {"next_id": 7, "tasks": []}
        t = taskstore.new_task(data, "hello", priority="low", due="2026-01-02",
                               status="progress")
        self.assertEqual(t["id"], 7)
        self.assertEqual(data["next_id"], 8)
        self.assertEqual(t["title"], "hello")
        self.assertEqual(t["priority"], "low")
        self.assertEqual(t["due"], "2026-01-02")
        self.assertEqual(t["status"], "progress")
        self.assertFalse(t["done"])
        self.assertEqual(t["created"], date.today().isoformat())

    def test_done_status_sets_done_flag(self):
        data = {"next_id": 1, "tasks": []}
        self.assertTrue(taskstore.new_task(data, "x", status="done")["done"])

    def test_bogus_status_becomes_todo(self):
        data = {"next_id": 1, "tasks": []}
        self.assertEqual(taskstore.new_task(data, "x", status="banana")["status"],
                         "todo")


class TestFindAndSort(unittest.TestCase):
    def test_find_task(self):
        data = {"next_id": 3, "tasks": [{"id": 1, "title": "a"},
                                        {"id": 2, "title": "b"}]}
        self.assertEqual(taskstore.find_task(data, 2)["title"], "b")
        self.assertIsNone(taskstore.find_task(data, 99))

    def test_sort_orders_undone_first_by_due_prio_id(self):
        data = {"next_id": 6, "tasks": [
            {"id": 1, "title": "done", "done": True, "status": "done",
             "priority": "high", "due": "2020-01-01"},
            {"id": 2, "title": "no due", "done": False, "status": "todo",
             "priority": "high", "due": None},
            {"id": 3, "title": "later low", "done": False, "status": "todo",
             "priority": "low", "due": "2026-01-01"},
            {"id": 4, "title": "later high", "done": False, "status": "todo",
             "priority": "high", "due": "2026-01-01"},
            {"id": 5, "title": "earliest", "done": False, "status": "todo",
             "priority": "med", "due": "2025-12-31"},
        ]}
        order = [t["id"] for t in taskstore.sort_tasks(data["tasks"])]
        self.assertEqual(order, [5, 4, 3, 2, 1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
