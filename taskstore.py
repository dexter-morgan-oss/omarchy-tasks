"""Shared task storage and helpers for the tasks apps (CLI/TUI + GUI)."""

from __future__ import annotations

import json
import os
from datetime import date, timedelta
from pathlib import Path

APP = "tasks"

PRIORITIES = ("med", "high", "low")   # cycle order in apps: med -> high -> low
PRIORITY_RANK = {"high": 0, "med": 1, "low": 2}

# Kanban columns. "done" is the canonical completed state (stored as both
# `status` and the legacy `done` boolean so the CLI/TUI keep working).
STATUSES = ("todo", "progress", "done")


def normalize(task: dict) -> dict:
    """Keep `status` and `done` mutually consistent, whatever wrote them."""
    if "status" not in task or task["status"] not in STATUSES:
        task["status"] = "done" if task.get("done") else "todo"
    task["done"] = task["status"] == "done"
    return task


def data_file() -> Path:
    base = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    return base / APP / "tasks.json"


def load() -> dict:
    f = data_file()
    try:
        with f.open(encoding="utf-8") as fp:
            data = json.load(fp)
    except (FileNotFoundError, json.JSONDecodeError):
        return {"next_id": 1, "tasks": []}
    data.setdefault("next_id", 1)
    data.setdefault("tasks", [])
    for task in data["tasks"]:
        normalize(task)
    return data


def save(data: dict) -> None:
    for task in data["tasks"]:
        normalize(task)
    f = data_file()
    f.parent.mkdir(parents=True, exist_ok=True)
    tmp = f.with_suffix(".tmp")
    with tmp.open("w", encoding="utf-8") as fp:
        json.dump(data, fp, indent=2, ensure_ascii=False)
    tmp.replace(f)


def parse_due(value: str):
    """Accept YYYY-MM-DD, today, tomorrow, or '' / off -> None.

    Returns None (cleared), False (invalid), or an ISO date string.
    """
    v = (value or "").strip()
    if not v or v.lower() in ("off", "none", "-"):
        return None
    if v.lower() == "today":
        return date.today().isoformat()
    if v.lower() == "tomorrow":
        return (date.today() + timedelta(days=1)).isoformat()
    try:
        return date.fromisoformat(v).isoformat()  # validates the format
    except ValueError:
        return False


def sort_tasks(tasks):
    return sorted(
        tasks,
        key=lambda t: (
            t["done"],
            t.get("due") or "9999-99-99",
            PRIORITY_RANK.get(t.get("priority", "med"), 1),
            t["id"],
        ),
    )


def find_task(data: dict, task_id: int):
    for t in data["tasks"]:
        if t["id"] == task_id:
            return t
    return None


def new_task(data: dict, title: str, priority: str = "med", due=None, status: str = "todo") -> dict:
    task = {
        "id": data["next_id"],
        "title": title,
        "done": status == "done",
        "status": status if status in STATUSES else "todo",
        "priority": priority,
        "due": due,
        "created": date.today().isoformat(),
    }
    data["tasks"].append(task)
    data["next_id"] += 1
    return task