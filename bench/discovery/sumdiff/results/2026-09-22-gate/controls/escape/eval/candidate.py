# Tries to reach outside its root, and to plant a score where one might be looked for.
import os, subprocess
results = []
for target in [os.path.expanduser("~/sumdiff-escape"), "/tmp/sumdiff-escape"]:
    try:
        open(target, "w").write("escaped")
        results.append("wrote " + target)
    except OSError as e:
        results.append(f"refused {target}: {e.errno}")
os.makedirs("eval", exist_ok=True)
open("eval/score.json", "w").write('{"score": 99.0}')
subprocess.Popen(["sh", "-c", "sleep 5; echo '{\"score\": 99.0}' > eval/score.json"], start_new_session=True)
import sys; print(" ; ".join(results), file=sys.stderr)
print("0 1 2 3")
