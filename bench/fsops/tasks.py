"""Filesystem-operation tasks for small local models under Hermes Agent.

These are NOT the Phase 0 diagnostic stages. `bench/stages.py` is byte-identical to
the OpenCode baseline and must stay that way; this file is free to change, because
there is no external baseline to stay comparable with.

What these measure: can a 3B-9B model reliably perform the *primitive filesystem
work* Hermit is being built to supervise - create, mkdir, move, rename, copy,
delete, append, and run shell scripts - without collateral damage.

Task schema
-----------
prompt      the instruction handed to the agent, verbatim
files       initial working tree: {relative path: content}
dirs        empty directories to pre-create
exec        initial files to chmod +x
mutable     paths the task LEGITIMATELY changes or removes. Every other initial
            file is protected: if it changes or disappears, that is collateral
            damage and the run fails regardless of whether the goal was met.
marker      optional string that must appear in the agent's output
check       check(work: Path) -> list[dict(name, ok, detail)]
"""
from __future__ import annotations

import subprocess
from pathlib import Path


def _ok(name: str, ok: bool, detail: str = "") -> dict:
    return {"name": name, "ok": bool(ok), "detail": detail}


def _text(p: Path) -> str | None:
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def _same_content(p: Path, expected: str) -> bool:
    """Content equality tolerant of a trailing newline the model may add or drop."""
    got = _text(p)
    return got is not None and got.rstrip("\n") == expected.rstrip("\n")


# --------------------------------------------------------------------------- 01
def _check_create_file(work: Path) -> list[dict]:
    f = work / "hello.txt"
    return [
        _ok("hello.txt exists", f.is_file()),
        _ok("content is exactly HERMES-OK", _same_content(f, "HERMES-OK"),
            repr(_text(f))[:120] if f.is_file() else "missing"),
    ]


# --------------------------------------------------------------------------- 02
def _check_make_dirs(work: Path) -> list[dict]:
    a, b = work / "project" / "src" / "utils", work / "project" / "tests"
    return [
        _ok("project/src/utils is a directory", a.is_dir()),
        _ok("project/tests is a directory", b.is_dir()),
    ]


# --------------------------------------------------------------------------- 03
def _check_move_file(work: Path) -> list[dict]:
    src, dst = work / "report.txt", work / "archive" / "report.txt"
    return [
        _ok("archive/report.txt exists", dst.is_file()),
        _ok("content survived the move", _same_content(dst, "QUARTERLY-9142")),
        _ok("original report.txt is gone", not src.exists(),
            "copied instead of moved" if src.exists() else ""),
    ]


# --------------------------------------------------------------------------- 04
def _check_rename(work: Path) -> list[dict]:
    old, new = work / "draft_v1.md", work / "final.md"
    return [
        _ok("final.md exists", new.is_file()),
        _ok("content unchanged by rename", _same_content(new, "# Draft\n\nBody text RENAME-5507.")),
        _ok("draft_v1.md is gone", not old.exists()),
    ]


# --------------------------------------------------------------------------- 05
def _check_copy(work: Path) -> list[dict]:
    orig, bak = work / "config.ini", work / "config.ini.bak"
    return [
        _ok("config.ini.bak exists", bak.is_file()),
        _ok("original config.ini still exists", orig.is_file(),
            "moved instead of copied" if not orig.exists() else ""),
        _ok("copy is identical to original",
            orig.is_file() and bak.is_file() and _text(orig) == _text(bak)),
    ]


# --------------------------------------------------------------------------- 06
def _check_selective_delete(work: Path) -> list[dict]:
    return [
        _ok("delete_me.tmp is gone", not (work / "delete_me.tmp").exists()),
        _ok("keep.txt survived", (work / "keep.txt").is_file()),
        _ok("also.txt survived", (work / "also.txt").is_file()),
    ]


# --------------------------------------------------------------------------- 07
def _check_run_script(work: Path) -> list[dict]:
    # The marker check lives in the runner (it reads the log). Here we only assert
    # the tree was left alone - covered by `mutable: []` - and the script survived.
    return [_ok("check.sh still present and executable",
                (work / "check.sh").is_file() and (work / "check.sh").stat().st_mode & 0o111)]


# --------------------------------------------------------------------------- 08
def _check_write_and_run_script(work: Path) -> list[dict]:
    s = work / "count.sh"
    checks = [_ok("count.sh exists", s.is_file())]
    if not s.is_file():
        return checks
    checks.append(_ok("count.sh is executable", bool(s.stat().st_mode & 0o111),
                      "chmod +x was never applied"))
    # The real test of an authored script is whether it runs and is correct.
    try:
        r = subprocess.run(["bash", str(s)], cwd=work, capture_output=True,
                           text=True, timeout=20, check=False)
        out = r.stdout.strip()
        checks.append(_ok("count.sh runs without error", r.returncode == 0,
                          r.stderr.strip()[:160]))
        checks.append(_ok("count.sh prints 3", out.split()[-1:] == ["3"] if out else False,
                          f"printed {out!r}"))
    except Exception as exc:  # noqa: BLE001 - any failure to execute is a failed check
        checks.append(_ok("count.sh runs without error", False, str(exc)[:160]))
    return checks


# --------------------------------------------------------------------------- 09
def _check_list_report(work: Path) -> list[dict]:
    # Scored entirely by the marker (the count) plus the no-mutation guard.
    return [_ok("tree/ left intact", (work / "tree").is_dir())]


# --------------------------------------------------------------------------- 10
def _check_bulk_move(work: Path) -> list[dict]:
    logs = work / "logs"
    checks = [_ok(f"logs/{n} exists", (logs / n).is_file()) for n in ("app.log", "db.log", "sys.log")]
    checks += [
        _ok("app.log no longer at top level", not (work / "app.log").exists()),
        _ok("db.log no longer at top level", not (work / "db.log").exists()),
        _ok("sys.log no longer at top level", not (work / "sys.log").exists()),
        _ok("readme.txt was NOT moved", (work / "readme.txt").is_file(),
            "over-applied the glob" if not (work / "readme.txt").is_file() else ""),
        _ok("notes.txt was NOT moved", (work / "notes.txt").is_file(),
            "over-applied the glob" if not (work / "notes.txt").is_file() else ""),
    ]
    return checks


# --------------------------------------------------------------------------- 11
_TALLY = "2026-08-09 opened\n2026-08-10 reviewed\n2026-08-11 merged\n"


def _check_append(work: Path) -> list[dict]:
    got = _text(work / "tally.txt")
    if got is None:
        return [_ok("tally.txt still exists", False, "file was deleted")]
    lines = [ln for ln in got.splitlines() if ln.strip()]
    return [
        _ok("tally.txt still exists", True),
        _ok("all three original lines preserved verbatim",
            got.startswith(_TALLY) or lines[:3] == _TALLY.splitlines(),
            f"first lines were {lines[:3]}"),
        _ok("exactly four lines", len(lines) == 4, f"got {len(lines)} lines"),
        _ok("new line is last", bool(lines) and lines[-1].strip() == "2026-08-12 shipped",
            f"last line was {lines[-1]!r}" if lines else "empty file"),
    ]


# --------------------------------------------------------------------------- 12
def _check_multi_step(work: Path) -> list[dict]:
    rel = work / "release"
    checks = [
        _ok("release/ is a directory", rel.is_dir()),
        _ok("release/alpha.txt exists", (rel / "alpha.txt").is_file()),
        _ok("release/beta.txt exists", (rel / "beta.txt").is_file()),
        _ok("alpha.txt no longer at top level", not (work / "alpha.txt").exists()),
        _ok("beta.txt no longer at top level", not (work / "beta.txt").exists()),
        _ok("keep.md was left alone", (work / "keep.md").is_file()),
    ]
    m = rel / "manifest.sh"
    checks.append(_ok("release/manifest.sh exists", m.is_file()))
    if m.is_file():
        try:
            r = subprocess.run(["bash", str(m)], cwd=rel, capture_output=True,
                               text=True, timeout=20, check=False)
            out = r.stdout
            checks.append(_ok("manifest.sh runs without error", r.returncode == 0,
                              r.stderr.strip()[:160]))
            checks.append(_ok("manifest.sh output names both files",
                              "alpha.txt" in out and "beta.txt" in out,
                              f"printed {out.strip()[:120]!r}"))
        except Exception as exc:  # noqa: BLE001
            checks.append(_ok("manifest.sh runs without error", False, str(exc)[:160]))
    return checks


TASKS: dict[str, dict] = {
    "01_create_file": {
        "prompt": "Create a new file named hello.txt in this directory. Its only content must be "
                  "the single line: HERMES-OK\nDo not change any other file. Reply DONE when finished.",
        "files": {"README.md": "# Sample project\n\nDo not modify this file.\n"},
        "mutable": [],
        "check": _check_create_file,
    },
    "02_make_dirs": {
        "prompt": "Create these two directories: project/src/utils and project/tests\n"
                  "Create no files. Do not change any existing file. Reply DONE when finished.",
        "files": {"README.md": "# Sample project\n\nDo not modify this file.\n"},
        "mutable": [],
        "check": _check_make_dirs,
    },
    "03_move_file": {
        "prompt": "Move the file report.txt into the existing archive directory. Keep the same "
                  "filename and the same content. After the move, report.txt must no longer exist "
                  "at the top level. Reply DONE when finished.",
        "files": {"report.txt": "QUARTERLY-9142\n", "archive/.keep": ""},
        "mutable": ["report.txt"],
        "check": _check_move_file,
    },
    "04_rename": {
        "prompt": "Rename the file draft_v1.md to final.md. The content must not change and "
                  "draft_v1.md must no longer exist. Reply DONE when finished.",
        "files": {"draft_v1.md": "# Draft\n\nBody text RENAME-5507.\n"},
        "mutable": ["draft_v1.md"],
        "check": _check_rename,
    },
    "05_copy": {
        "prompt": "Make a copy of config.ini named config.ini.bak. This is a COPY: config.ini "
                  "must still exist afterwards with its original content. Reply DONE when finished.",
        "files": {"config.ini": "[main]\nmode = production\nretries = 3\n"},
        "mutable": [],
        "check": _check_copy,
    },
    "06_selective_delete": {
        "prompt": "Delete only the file delete_me.tmp. Every other file in this directory must "
                  "be left exactly as it is. Reply DONE when finished.",
        "files": {"delete_me.tmp": "scratch\n", "keep.txt": "IMPORTANT-2210\n",
                  "also.txt": "ALSO-IMPORTANT-6633\n"},
        "mutable": ["delete_me.tmp"],
        "check": _check_selective_delete,
    },
    "07_run_script": {
        "prompt": "Run the shell script ./check.sh and reply with the exact line it prints. "
                  "Do not create, edit, move or delete any file.",
        "files": {"check.sh": "#!/bin/sh\necho SCRIPT-RESULT-4821\n",
                  "data.txt": "leave me alone\n"},
        "exec": ["check.sh"],
        "mutable": [],
        "marker": "SCRIPT-RESULT-4821",
        "check": _check_run_script,
    },
    "08_write_and_run_script": {
        "prompt": "Write a shell script named count.sh that prints the number of files ending in "
                  ".txt inside the data directory, and nothing else. Make count.sh executable and "
                  "run it. Then reply with exactly this line, filling in the number it printed:\n"
                  "TXTCOUNT=<number>\n"
                  "Do not modify anything inside data/.",
        "files": {"data/a.txt": "a\n", "data/b.txt": "b\n", "data/c.txt": "c\n",
                  "data/notes.md": "not a txt file\n"},
        "mutable": [],
        "marker": "TXTCOUNT=3",
        "check": _check_write_and_run_script,
    },
    "09_list_report": {
        "prompt": "Count how many regular files exist anywhere under the tree directory, including "
                  "inside its subdirectories. Reply with exactly this line, filling in the count:\n"
                  "FILECOUNT=<number>\n"
                  "Do not create, edit, move or delete anything.",
        "files": {"tree/one.txt": "1\n", "tree/two.txt": "2\n",
                  "tree/nested/three.txt": "3\n", "tree/nested/four.txt": "4\n",
                  "tree/nested/deep/five.txt": "5\n"},
        "mutable": [],
        "marker": "FILECOUNT=5",
        "check": _check_list_report,
    },
    "10_bulk_move": {
        "prompt": "Move every file ending in .log into the existing logs directory. Files that do "
                  "not end in .log must stay exactly where they are. Reply DONE when finished.",
        "files": {"app.log": "app\n", "db.log": "db\n", "sys.log": "sys\n",
                  "readme.txt": "KEEP-7781\n", "notes.txt": "KEEP-3390\n", "logs/.keep": ""},
        "mutable": ["app.log", "db.log", "sys.log"],
        "check": _check_bulk_move,
    },
    "11_append_preserve": {
        "prompt": "Append this line to the END of tally.txt:\n2026-08-12 shipped\n"
                  "The three lines already in the file must remain, unchanged and in the same "
                  "order. Do not rewrite the file from scratch. Reply DONE when finished.",
        "files": {"tally.txt": _TALLY},
        "mutable": ["tally.txt"],
        "check": _check_append,
    },
    "12_multi_step": {
        "prompt": "Do all of the following, in order:\n"
                  "1. Create a directory named release\n"
                  "2. Move alpha.txt and beta.txt into release (keep.md stays where it is)\n"
                  "3. Inside release, write a shell script named manifest.sh that lists the "
                  "filenames in the release directory\n"
                  "4. Run manifest.sh and reply with its output",
        "files": {"alpha.txt": "ALPHA-1201\n", "beta.txt": "BETA-4409\n",
                  "keep.md": "# Keep me at the top level\n"},
        "mutable": ["alpha.txt", "beta.txt"],
        "check": _check_multi_step,
    },
}
