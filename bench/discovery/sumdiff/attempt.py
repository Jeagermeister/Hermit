#!/usr/bin/env python3
"""One discovery attempt on the sum-difference domain, recorded.

    attempt.py --model TAG --record-dir DIR [--work-dir DIR] [--max-turns N] [--budget S]

Seeds a fresh sandbox root with seed/solve.py and a copy of objective.py as check.py, runs
`hermit agent --shell` against it with a fixed instruction, then scores the resulting solve.py
with score.py. The root lives under --work-dir (scratch); what is kept under --record-dir is
the record: run.json (hardware, model tag and digest, versions, flags), agent.log, a hash
manifest of the final root, and eval/score.json with the scored candidate beside it.

The model's own check.py is a convenience for the model. The official score never reads it:
score.py re-runs only solve.py, in a fresh root, and computes the objective itself.
"""

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]

INSTRUCTION = (
    "solve.py prints a finite set A of integers, separated by spaces. Its score is "
    "Gamma(A) = log(|A+A| / |A|) / log(|A-A| / |A|), where A+A is the set of all pairwise sums "
    "and A-A the set of all pairwise differences. Higher is better; the current solve.py prints "
    "an interval, which scores exactly 1.0. Improve solve.py so the set it prints scores as high "
    "as you can. Run `python3 check.py` to see the current score of solve.py. Rules: edit only "
    "solve.py; Python standard library only; it must finish within 60 seconds; at most 20000 "
    "distinct integers; max(A) - min(A) at most 1048576; print only the integers."
)


def sha256_file(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def cmd(*args) -> str:
    r = subprocess.run(list(args), capture_output=True, text=True)
    return (r.stdout or r.stderr).strip()


def model_digest(tag: str) -> str:
    with urllib.request.urlopen("http://localhost:11434/api/tags", timeout=10) as resp:
        for m in json.load(resp)["models"]:
            if m["name"] == tag or m["model"] == tag:
                return m["digest"]
    return "not found in /api/tags"


def manifest(root: Path) -> list:
    return [{"path": str(p.relative_to(root)), "bytes": p.stat().st_size, "sha256": sha256_file(p)}
            for p in sorted(root.rglob("*")) if p.is_file()]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--record-dir", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path, required=True)
    ap.add_argument("--max-turns", type=int, default=24)
    ap.add_argument("--budget", type=int, default=1500)
    ap.add_argument("--hermit", type=Path, default=REPO / "build" / "hermit")
    args = ap.parse_args()

    hermit = args.hermit.resolve()
    rec = args.record_dir.resolve()
    work = args.work_dir.resolve()
    if rec.exists() or work.exists():
        sys.exit(f"refusing to reuse {rec} or {work}: every attempt starts fresh")
    root = work / "root"
    root.mkdir(parents=True)
    rec.mkdir(parents=True)
    shutil.copyfile(HERE / "seed" / "solve.py", root / "solve.py")
    shutil.copyfile(HERE / "objective.py", root / "check.py")

    argv = [str(hermit), "agent", "--root", str(root), "--model", args.model,
            "--backups", str(work / "backups"), "--shell", "--shell-timeout", "90",
            "--max-turns", str(args.max_turns), "--budget", str(args.budget),
            "--attempts", "1", "--expect", "exists:solve.py", INSTRUCTION]

    run = {
        "domain": "sumdiff",
        "docket": "1.15",
        "model_tag": args.model,
        "model_digest": model_digest(args.model),
        "ollama_version": cmd("ollama", "--version"),
        "hermit_commit": cmd("git", "-C", str(REPO), "rev-parse", "HEAD"),
        "hermit_sha256": sha256_file(hermit),
        "host": platform.node(),
        "kernel": platform.release(),
        "cpu": cmd("sh", "-c", "lscpu | sed -n 's/^Model name: *//p'"),
        "gpu": cmd("nvidia-smi", "--query-gpu=name,memory.total,driver_version",
                   "--format=csv,noheader"),
        "python": platform.python_version(),
        "seed_sha256": sha256_file(root / "solve.py"),
        "instruction": INSTRUCTION,
        "argv": argv,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    t0 = time.monotonic()
    with open(rec / "agent.log", "w") as log:
        proc = subprocess.run(argv, stdout=log, stderr=subprocess.STDOUT)
    run["agent_exit"] = proc.returncode
    run["agent_wall_s"] = round(time.monotonic() - t0, 1)
    run["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    run["final_root"] = manifest(root)
    run["solve_py_changed"] = sha256_file(root / "solve.py") != run["seed_sha256"] \
        if (root / "solve.py").exists() else None
    (rec / "run.json").write_text(json.dumps(run, indent=2) + "\n")

    if (root / "solve.py").exists():
        subprocess.run([sys.executable, str(HERE / "score.py"), str(root / "solve.py"), str(rec),
                        "--hermit", str(hermit)], check=True)
    else:
        print("solve.py is gone; nothing to score")
    return 0


if __name__ == "__main__":
    sys.exit(main())
