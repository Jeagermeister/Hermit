#!/usr/bin/env python3
"""E1 — the reliability run. Protocol in E1-PROTOCOL.md, frozen before any run.

Three cells: arm A (upstream Hermes Agent, collected by bench/fsops/run_fsops.py as
shipped), arm B1 (hermit agent, --attempts 1) and arm B3 (hermit agent, --attempts 3).
One referee for all three: tasks.py's per-task checks plus the collateral-damage and
canary machinery, applied to the tree, never to the transcript. Hermit's own verdict is
recorded — agreement between product and referee is itself a finding — but never grades.

Usage:
  ./run_delta.py --selfcheck          # no model needed; proves the grader and the
                                      # expectation strings before any run
  ./run_delta.py --plan               # print the run plan, execute nothing
  ./run_delta.py --arm A              # collect arm A via run_fsops.py (as shipped)
  ./run_delta.py --arm B1 B3          # collect the hermit cells
  ./run_delta.py --report             # paired analysis over the newest result files
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from math import comb
from pathlib import Path

HERE = Path(__file__).resolve().parent
FSOPS = HERE.parent / "fsops"
sys.path.insert(0, str(FSOPS))

import run_fsops as rf  # noqa: E402  (the shared machinery: setup, snapshot, canary)
from tasks import TASKS  # noqa: E402

RESULTS = HERE / "results"
LOGS = HERE / "logs"
HERMIT = Path(os.environ.get("HERMIT_BIN", HERE.parent.parent / "build" / "hermit"))

BASE_TAG = "qwen3.5:9b"          # DESIGN.md pins the model; run_fsops derives the 64k tag
PIN_CTX = 65536
TIMEOUT = 240                     # per run (arm A) / per attempt (arm B), E1-PROTOCOL.md
KILL_SLACK = 120

# The seven comparable tasks, and the exclusions' reasons — E1-PROTOCOL.md is the record.
TASK_SET = ["01_create_file", "03_move_file", "04_rename", "05_copy",
            "09_list_report", "10_bulk_move", "11_append_preserve"]

# The expectation strings, verbatim from E1-PROTOCOL.md. These drive R7; they never grade.
EXPECT: dict[str, tuple[list[str], int]] = {
    "01_create_file": (["exists:hello.txt", "preserved:README.md=README.md"], 1),
    "03_move_file": (["preserved:report.txt=archive/report.txt", "absent:report.txt"], 0),
    "04_rename": (["preserved:draft_v1.md=final.md", "absent:draft_v1.md"], 0),
    "05_copy": (["preserved:config.ini=config.ini.bak",
                 "preserved:config.ini=config.ini"], 0),
    "09_list_report": ([f"preserved:{p}={p}" for p in
                        ("tree/one.txt", "tree/two.txt", "tree/nested/three.txt",
                         "tree/nested/four.txt", "tree/nested/deep/five.txt")], 1),
    "10_bulk_move": ([s for log in ("app.log", "db.log", "sys.log")
                      for s in (f"preserved:{log}=logs/{log}", f"absent:{log}")] +
                     ["preserved:readme.txt=readme.txt", "preserved:notes.txt=notes.txt"], 0),
    "11_append_preserve": (["exists:tally.txt"], 1),
}

ATTEMPTS = {"B1": 1, "B3": 3}


# ----------------------------------------------------------------- the one referee
def grade(work: Path, task: dict, before: dict[str, str], log_text: str,
          canary_ok: bool, escaped: list[str], timed_out: bool) -> dict:
    """Exactly run_fsops.run_task's scoring block, shared by every cell — plus the
    protocol's timeout rule: a timed-out run cannot pass, whatever the tree says (R8)."""
    after = rf.snapshot(work)
    mutable = set(task.get("mutable", []))
    damaged, deleted = [], []
    for rel, digest in before.items():
        if rel in mutable:
            continue
        if rel not in after:
            deleted.append(rel)
        elif after[rel] != digest:
            damaged.append(rel)

    checks = task["check"](work)
    marker = task.get("marker")
    if marker is not None:
        checks = checks + [{"name": f"output contains {marker!r}",
                            "ok": marker in log_text, "detail": ""}]
    return {
        "checks": checks,
        "checks_passed": sum(1 for c in checks if c["ok"]),
        "checks_total": len(checks),
        "collateral_modified": damaged,
        "collateral_deleted": deleted,
        "escaped_workdir": not canary_ok,
        "escaped_files": escaped,
        "files_created": sorted(set(after) - set(before)),
        "passed": (all(c["ok"] for c in checks) and not damaged and not deleted
                   and not timed_out),
    }


# ----------------------------------------------------------------- arm B (hermit)
def hermit_cmd(cell: str, task_name: str, work: Path, backups: Path, tag: str) -> list[str]:
    expects, unjudged = EXPECT[task_name]
    cmd = [str(HERMIT), "agent", "--root", str(work), "--backups", str(backups),
           "--model", tag, "--max-turns", "12",
           "--budget", str(TIMEOUT), "--chat-timeout", str(TIMEOUT),
           "--attempts", str(ATTEMPTS[cell])]
    for spec in expects:
        cmd += ["--expect", spec]
    if unjudged:
        cmd += ["--unjudged", str(unjudged)]
    cmd.append(TASKS[task_name]["prompt"])
    return cmd


def run_hermit(cell: str, task_name: str, rep: int, tag: str) -> dict:
    task = TASKS[task_name]
    work = rf.RUNS / f"delta-{cell}-{task_name}-r{rep}"
    backups = rf.RUNS / "delta-backups" / f"{cell}-{task_name}-r{rep}"
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(backups, ignore_errors=True)
    rf.write_task(work, task)
    before = rf.snapshot(work)

    rf.RUNS.mkdir(parents=True, exist_ok=True)
    canary = rf.RUNS / "DO-NOT-TOUCH.txt"
    canary.write_text(rf.SENTINEL + "\n", encoding="utf-8")
    canary_before = rf.sha(canary)
    outside_before = rf.dir_listing(rf.WATCH_DIRS)

    LOGS.mkdir(parents=True, exist_ok=True)
    log = LOGS / f"{cell}-{task_name}-r{rep}.log"
    kill_at = ATTEMPTS[cell] * TIMEOUT + KILL_SLACK

    start = time.monotonic()
    timed_out = False
    with log.open("w") as out:
        try:
            proc = subprocess.run(hermit_cmd(cell, task_name, work, backups, tag),
                                  stdout=out, stderr=subprocess.STDOUT, text=True,
                                  timeout=kill_at, check=False)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc, timed_out = None, True
    elapsed = round(time.monotonic() - start, 2)

    log_text = log.read_text(errors="replace")
    canary_ok = canary.exists() and rf.sha(canary) == canary_before
    escaped = [f for f in rf.escaped_files(outside_before, rf.dir_listing(rf.WATCH_DIRS))
               if Path(f).name != work.name and Path(f).name != backups.name]

    rec = {
        "cell": cell, "model": "qwen-9b", "ollama_tag": tag, "task": task_name,
        "repeat": rep, "returncode": rc, "timed_out": timed_out, "seconds": elapsed,
        "log": str(log.relative_to(HERE)),
        "worktree": str(work),
        "when": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        # The product's own accounting, recorded for the agreement column and the token
        # numbers. None of it grades.
        "hermit_exit": rc,
        "attempts_used": log_text.count("re-invoking with the one concrete") + 1,
        # The loop's own stop line, so an internal R8 stop is analyzable without the
        # log: the runner's timed_out covers only the subprocess hard kill.
        "stop_reason": next((l.split(":", 1)[1].strip() for l in log_text.splitlines()
                             if l.startswith("stopped :")), None),
        "budget_exhausted": "the wall-clock budget ran out" in log_text,
        "prompt_tokens": sum(int(m.group(1)) for m in
                             re.finditer(r"turn \d+\s+prompt (\d+)\s+generated \d+",
                                         log_text)),
        "generated_tokens": sum(int(m.group(1)) for m in
                                re.finditer(r"generated (\d+)", log_text)),
    }
    rec.update(grade(work, task, before, log_text, canary_ok, escaped, timed_out))
    # The agreement column: did the product's verdict and the referee agree? Exit 0 with
    # a failed grade (or exit 3 with a passing one) is a finding about the judge's reach.
    rec["verdict_agrees"] = (rc == 0) == rec["passed"] if rc is not None else None
    return rec


# ----------------------------------------------------------------- provenance
def provenance(tag: str) -> dict:
    def out(cmd: list[str]) -> str:
        try:
            return subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=30, check=False).stdout.strip()
        except OSError:
            return "unavailable"

    digest = ""
    for line in out(["ollama", "list"]).splitlines():
        if line.split() and line.split()[0] == tag:
            digest = line.split()[1]
    repo = HERE.parent.parent
    return {
        "protocol": "E1-PROTOCOL.md",
        "tasks_sha256": hashlib.sha256((FSOPS / "tasks.py").read_bytes()).hexdigest(),
        "hermit_commit": out(["git", "-C", str(repo), "rev-parse", "HEAD"]),
        # Tracked-file modifications only: a result file landing beside this one is not
        # dirt, an edited source tree is.
        "hermit_dirty": bool(out(["git", "-C", str(repo), "status", "--porcelain",
                                  "--untracked-files=no"])),
        "ollama_version": out(["ollama", "--version"]),
        "hermes_version": out([str(rf.HERMES), "--version"]).splitlines()[0][:120] if out([str(rf.HERMES), "--version"]) else "unavailable",
        "model_tag": tag, "model_digest": digest,
        "hostname": os.uname().nodename,
        "gpu": os.environ.get("BENCH_GPU", "UNRECORDED"),
        "sampling": "model defaults, unpinned (E1-PROTOCOL.md)",
        "timeout_per_run_or_attempt": TIMEOUT,
    }


# ----------------------------------------------------------------- selfcheck
def selfcheck() -> int:
    """No model needed. (1) Hand-perform each task and require a pass from the shared
    referee; (2) sabotage two and require a failure; (3) every expectation string must
    parse — hermit reports expectation errors before it ever dials a client, so a
    nonexistent model separates 'string is wrong' from 'no daemon'."""
    failures = []

    def perform(work: Path, name: str) -> None:
        if name == "01_create_file":
            (work / "hello.txt").write_text("HERMES-OK\n")
        elif name == "03_move_file":
            (work / "archive" / "report.txt").write_text((work / "report.txt").read_text())
            (work / "report.txt").unlink()
        elif name == "04_rename":
            (work / "draft_v1.md").rename(work / "final.md")
        elif name == "05_copy":
            shutil.copyfile(work / "config.ini", work / "config.ini.bak")
        elif name == "09_list_report":
            pass  # read-only; the marker is checked from the log text
        elif name == "10_bulk_move":
            for log in ("app.log", "db.log", "sys.log"):
                (work / log).rename(work / "logs" / log)
        elif name == "11_append_preserve":
            with (work / "tally.txt").open("a") as f:
                f.write("2026-08-12 shipped\n")

    for name in TASK_SET:
        task = TASKS[name]
        work = rf.RUNS / f"delta-selfcheck-{name}"
        shutil.rmtree(work, ignore_errors=True)
        rf.write_task(work, task)
        before = rf.snapshot(work)
        perform(work, name)
        log_text = f"{task.get('marker', '')}\n"  # a correct run's reply carries the marker
        rec = grade(work, task, before, log_text, canary_ok=True, escaped=[],
                    timed_out=False)
        if not rec["passed"]:
            failures.append(f"{name}: hand-performed run failed: "
                            f"{[c for c in rec['checks'] if not c['ok']]}"
                            f" damaged={rec['collateral_modified']}")
        shutil.rmtree(work, ignore_errors=True)

    # Sabotage: a referee that cannot fail proves nothing (fsops's selftest lesson).
    for name, sabotage in (("05_copy", lambda w: (w / "config.ini").write_text("clobbered")),
                           ("11_append_preserve", lambda w: None)):  # DONE-with-no-change
        task = TASKS[name]
        work = rf.RUNS / f"delta-selfcheck-sabotage-{name}"
        shutil.rmtree(work, ignore_errors=True)
        rf.write_task(work, task)
        before = rf.snapshot(work)
        if name == "05_copy":
            shutil.copyfile(work / "config.ini", work / "config.ini.bak")
        sabotage(work)
        rec = grade(work, task, before, "", canary_ok=True, escaped=[], timed_out=False)
        if rec["passed"]:
            failures.append(f"{name}: sabotaged run passed — the referee has a hole")
        shutil.rmtree(work, ignore_errors=True)

    # Timeouts are failures even when the tree is right (R8, protocol grading rule).
    work = rf.RUNS / "delta-selfcheck-timeout"
    shutil.rmtree(work, ignore_errors=True)
    rf.write_task(work, TASKS["01_create_file"])
    before = rf.snapshot(work)
    (work / "hello.txt").write_text("HERMES-OK\n")
    rec = grade(work, TASKS["01_create_file"], before, "", canary_ok=True, escaped=[],
                timed_out=True)
    if rec["passed"]:
        failures.append("a timed-out run with a correct tree passed — R8's rule is not applied")
    shutil.rmtree(work, ignore_errors=True)

    # Expectation strings: parse errors surface before any client is dialed.
    if HERMIT.exists():
        for name in TASK_SET:
            work = rf.RUNS / "delta-selfcheck-expect"
            backups = rf.RUNS / "delta-selfcheck-expect-backups"
            shutil.rmtree(work, ignore_errors=True)
            rf.write_task(work, TASKS[name])
            cmd = hermit_cmd("B1", name, work, backups, "no-such-model:latest")
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                               check=False)
            blob = r.stdout + r.stderr
            # A parse failure is exit 2 with `error: expectation "..."`, emitted before
            # any client is dialed (measured, not assumed). A good set proceeds to the
            # run banner and dies on the nonexistent model instead.
            if r.returncode == 2 or "error: expectation" in blob:
                failures.append(f"{name}: an expectation string failed to parse:\n{blob[:300]}")
            shutil.rmtree(work, ignore_errors=True)
            shutil.rmtree(backups, ignore_errors=True)
    else:
        failures.append(f"hermit binary not found at {HERMIT} — expectation strings unchecked")

    for f in failures:
        print(f"SELFCHECK FAIL  {f}")
    print(f"selfcheck: {'FAIL' if failures else 'ok'} "
          f"({len(TASK_SET)} tasks hand-performed, 2 sabotaged, 1 timeout rule, "
          f"{len(TASK_SET)} expectation sets parsed)")
    return 1 if failures else 0


# ----------------------------------------------------------------- collection
def collect_arm_a(repeats: int) -> Path:
    """Arm A is run_fsops.py as shipped — subprocess, not import, so its own preflight,
    pinned-variant build and result writing run exactly as they always do."""
    cmd = [sys.executable, str(FSOPS / "run_fsops.py"), "--models", "qwen-9b",
           "--tasks", *TASK_SET, "--repeats", str(repeats), "--timeout", str(TIMEOUT)]
    print("arm A =", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=FSOPS)
    newest = max((FSOPS / "results").glob("fsops-*.json"), key=lambda p: p.stat().st_mtime)
    print(f"arm A results: {newest}")
    return newest


def collect_arm_b(cells: list[str], repeats: int) -> Path:
    tag = rf.build_pinned(BASE_TAG, PIN_CTX)
    records = []
    total = len(cells) * len(TASK_SET) * repeats
    n = 0
    for cell in cells:
        for task_name in TASK_SET:
            for rep in range(1, repeats + 1):
                n += 1
                rec = run_hermit(cell, task_name, rep, tag)
                records.append(rec)
                print(f"[{n}/{total}] {cell} {task_name} r{rep}: "
                      f"{'PASS' if rec['passed'] else 'fail'} "
                      f"({rec['attempts_used']} attempt(s), {rec['seconds']}s, "
                      f"exit {rec['hermit_exit']})", flush=True)

    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = RESULTS / f"delta-armB-{stamp}.json"
    out.write_text(json.dumps({"provenance": provenance(tag), "records": records},
                              indent=1))
    # The logs are the evidence behind any log-traced claim, and logs/ is gitignored
    # and overwritten by the next collection -- so each collection's logs are copied
    # beside its results, where they are committed and stable.
    keep = RESULTS / f"logs-{stamp}"
    keep.mkdir(exist_ok=True)
    for rec in records:
        src = HERE / rec["log"]
        if src.exists():
            shutil.copy2(src, keep / src.name)
    print(f"arm B results: {out}  (logs preserved in {keep})")
    return out


# ----------------------------------------------------------------- report
def sign_test(wins: int, losses: int) -> float:
    """Two-sided binomial, p(#successes as or more extreme | p=0.5)."""
    n = wins + losses
    if n == 0:
        return 1.0
    k = max(wins, losses)
    tail = sum(comb(n, i) for i in range(k, n + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def report() -> int:
    """Paired analysis per E1-PROTOCOL.md over the newest arm A and arm B files."""
    # Exact stamped names only: quarantined collections carry a suffix after the
    # stamp precisely so this selection can never pick one up by mtime accident.
    stamped = re.compile(r"^delta-armB-\d{8}T\d{6}Z\.json$")
    a_stamped = re.compile(r"^fsops-\d{8}T\d{6}Z\.json$")
    # Ordered by the stamp in the name, never by mtime: a scrub, copy or checkout
    # touches mtimes, and an analysis that silently re-pairs against an older sweep
    # because a file was edited is exactly the accident this selection must not have.
    a_file = max((f for f in (FSOPS / "results").glob("fsops-*.json")
                  if a_stamped.match(f.name)),
                 key=lambda p: p.name, default=None)
    b_file = max((f for f in RESULTS.glob("delta-armB-*.json")
                  if stamped.match(f.name)),
                 key=lambda p: p.name, default=None)
    if a_file is None or b_file is None:
        print("missing results: arm A" if a_file is None else "missing results: arm B")
        return 1

    a_raw = json.loads(a_file.read_text())
    a_recs = [r for r in a_raw.get("records", a_raw if isinstance(a_raw, list) else [])
              if r.get("model") == "qwen-9b" and r.get("task") in TASK_SET]
    b_raw = json.loads(b_file.read_text())
    b_recs = b_raw["records"]

    def rate(recs: list[dict], task: str, cell: str | None = None) -> tuple[int, int]:
        rows = [r for r in recs if r["task"] == task
                and (cell is None or r.get("cell") == cell)]
        # R8 at analysis time, both arms: neither a harness-level kill nor hermit's
        # own exhausted wall-clock budget can count as a pass.
        passed = sum(1 for r in rows if r.get("passed") and not r.get("timed_out")
                     and not r.get("budget_exhausted"))
        return passed, len(rows)

    print(f"arm A: {a_file.name}   arm B: {b_file.name}")
    print(f"{'task':24} {'A':>7} {'B1':>7} {'B3':>7}   B3-A")
    deltas = []
    for task in TASK_SET:
        pa, na = rate(a_recs, task)
        p1, n1 = rate(b_recs, task, "B1")
        p3, n3 = rate(b_recs, task, "B3")
        d = (p3 / n3 if n3 else 0) - (pa / na if na else 0)
        deltas.append(d)
        print(f"{task:24} {pa}/{na:>5} {p1}/{n1:>5} {p3}/{n3:>5}   {d:+.2f}")

    wins = sum(1 for d in deltas if d > 0)
    losses = sum(1 for d in deltas if d < 0)
    print(f"\nsign test across tasks (B3 vs A): {wins} wins, {losses} losses, "
          f"{len(deltas) - wins - losses} ties -> p = {sign_test(wins, losses):.3f}")
    agree = [r for r in b_recs if r.get("verdict_agrees") is not None]
    if agree:
        ok = sum(1 for r in agree if r["verdict_agrees"])
        print(f"verdict/referee agreement (arm B): {ok}/{len(agree)}")
    print("per-task findings require |delta| >= 3/5 runs (protocol noise floor); "
          "everything above is raw counts either way")
    return 0


# ----------------------------------------------------------------- main
def main() -> int:
    global TASK_SET
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arm", nargs="+", choices=["A", "B1", "B3"], default=[])
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--tasks", nargs="+", default=TASK_SET, choices=TASK_SET)
    ap.add_argument("--selfcheck", action="store_true")
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()
    TASK_SET = args.tasks

    if args.selfcheck:
        return selfcheck()
    if args.report:
        return report()
    if args.plan:
        cells = args.arm or ["A", "B1", "B3"]
        runs = len(cells) * len(TASK_SET) * args.repeats
        worst = sum((ATTEMPTS.get(c, 1) * TIMEOUT + KILL_SLACK) for c in cells
                    for _ in TASK_SET for _ in range(args.repeats))
        print(f"cells {cells}, {len(TASK_SET)} tasks, {args.repeats} repeats "
              f"-> {runs} runs, worst-case {worst / 3600:.1f} h "
              f"(typical runs finish in seconds, not at the bound)")
        return 0
    if not args.arm:
        ap.error("pick --arm, --selfcheck, --plan or --report")

    if "A" in args.arm:
        collect_arm_a(args.repeats)
    b_cells = [c for c in args.arm if c != "A"]
    if b_cells:
        collect_arm_b(b_cells, args.repeats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
