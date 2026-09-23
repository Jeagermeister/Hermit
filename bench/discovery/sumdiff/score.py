#!/usr/bin/env python3
"""Official scorer for the sum-difference domain (DOCKET 1.15).

    score.py CANDIDATE.py OUT_DIR [--repeat N] [--hermit PATH]

Writes OUT_DIR/eval/score.json. The split between trusted and untrusted is the design:

- The candidate is untrusted code. It runs through an existing confined path -- `hermit mcp`
  with shell enabled, which refuses to start unless D10's probe reports Enforced -- in a fresh
  root holding nothing but a copy of the candidate, one fresh root per repeat.
- The objective is computed here, in the unconfined parent, from the captured stdout, and
  score.json is written under OUT_DIR, which the confined child was never granted. A score file
  written from inside the candidate's own writable root could be rewritten by anything the
  candidate left running; this one cannot.

Repeats exist to measure the candidate's determinism, not to average: the objective is exact,
so two runs either print the same set or they do not, and the record says which.
"""

import argparse
import hashlib
import json
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
import objective  # noqa: E402

SHELL_TIMEOUT_S = 60
SCORER_VERSION = 1


def sha256_file(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def git_head() -> str:
    r = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else "unknown"


def run_confined_once(hermit: Path, candidate: Path) -> dict:
    """One execution of the candidate under `hermit mcp`'s shell tool, in a fresh root."""
    with tempfile.TemporaryDirectory(prefix="sumdiff-score-") as tmp:
        root = Path(tmp) / "root"
        root.mkdir()
        shutil.copyfile(candidate, root / "solve.py")
        cfg = Path(tmp) / "hermit.json"
        cfg.write_text(json.dumps({"shell": {"enabled": True, "timeout_s": SHELL_TIMEOUT_S}}))
        requests = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                        "clientInfo": {"name": "sumdiff-score", "version": str(SCORER_VERSION)}}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
             "params": {"name": "shell", "arguments": {"command": "python3 solve.py"}}},
        ]
        t0 = time.monotonic()
        proc = subprocess.run(
            [str(hermit), "mcp", "--root", str(root), "--config", str(cfg)],
            input="".join(json.dumps(r) + "\n" for r in requests),
            capture_output=True, text=True, timeout=SHELL_TIMEOUT_S + 60,
        )
        elapsed = time.monotonic() - t0
        replies = {}
        for line in proc.stdout.splitlines():
            if line.strip():
                msg = json.loads(line)
                if "id" in msg:
                    replies[msg["id"]] = msg
        call = replies.get(2)
        if proc.returncode != 0 or call is None or "result" not in call:
            # The server itself refused -- most importantly, an unenforced confinement probe.
            return {"ran": False, "why": "hermit mcp did not serve the call",
                    "server_exit": proc.returncode, "server_stderr": proc.stderr[-4000:],
                    "elapsed_s": elapsed}
        result = call["result"]
        text = result["content"][0]["text"]
        if result.get("isError"):
            # Timeouts (R8) land here as a ToolError, never as a row.
            return {"ran": False, "why": text, "elapsed_s": elapsed}
        row = json.loads(text)[0]
        return {"ran": True, "exit_code": row["exit_code"], "stdout": row["stdout"],
                "stdout_truncated": row["stdout_truncated"], "stderr": row["stderr"],
                "elapsed_s": elapsed}


def score_run(run: dict) -> dict:
    if not run["ran"]:
        return {"valid": False, "reason": run["why"], "score": 0.0}
    if run["exit_code"] != 0:
        return {"valid": False, "reason": f"solve.py exited {run['exit_code']}", "score": 0.0}
    if run["stdout_truncated"]:
        return {"valid": False, "reason": "stdout exceeded the capture cap", "score": 0.0}
    return objective.evaluate(run["stdout"])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate", type=Path)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--hermit", type=Path, default=REPO / "build" / "hermit")
    args = ap.parse_args()

    candidate = args.candidate.resolve()
    runs = []
    for i in range(args.repeat):
        raw = run_confined_once(args.hermit.resolve(), candidate)
        ev = score_run(raw)
        runs.append({
            "repeat": i,
            "elapsed_s": round(raw["elapsed_s"], 3),
            "stdout_sha256": sha256_bytes(raw.get("stdout", "").encode()) if raw["ran"] else None,
            "stderr_tail": (raw.get("stderr") or raw.get("server_stderr") or "")[-2000:],
            **ev,
        })

    first = runs[0]
    shas = {r["stdout_sha256"] for r in runs}
    record = {
        "scorer": "bench/discovery/sumdiff/score.py",
        "scorer_version": SCORER_VERSION,
        "scorer_sha256": {"score.py": sha256_file(HERE / "score.py"),
                          "objective.py": sha256_file(HERE / "objective.py")},
        "objective": "Gamma(A) = log(|A+A|/|A|) / log(|A-A|/|A|), higher is better",
        "caps": {"max_elements": objective.MAX_ELEMENTS, "max_span": objective.MAX_SPAN,
                 "wall_clock_s": SHELL_TIMEOUT_S},
        "confinement": "hermit mcp shell tool (D10 Landlock; server refuses to start unless "
                       "its probe reports Enforced); objective computed outside the root",
        "hermit_commit": git_head(),
        "hermit_sha256": sha256_file(args.hermit.resolve()),
        "host": platform.node(),
        "scored_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "candidate_sha256": sha256_file(candidate),
        "score": first["score"],
        "valid": first["valid"],
        "reason": first["reason"],
        # None, not False, when some repeat printed nothing: unmeasured is not nondeterministic.
        "deterministic_across_repeats": None if None in shas else len(shas) == 1,
        "runs": runs,
    }
    out = args.out_dir / "eval"
    out.mkdir(parents=True, exist_ok=True)
    (out / "score.json").write_text(json.dumps(record, indent=2) + "\n")
    shutil.copyfile(candidate, out / "candidate.py")
    print(json.dumps({"score": record["score"], "valid": record["valid"], "reason": record["reason"],
                      "deterministic": record["deterministic_across_repeats"],
                      "sizes": {k: first.get(k) for k in ("n", "sumset_size", "diffset_size")}}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
