# 14. Expectations

A free-text instruction carries no post-condition, so you state one alongside it. Expectations
are what turn `hermit agent` from a reporter into a judge — and they are what the retry
machinery (R7) has to work with, so writing them well is most of using Hermit well.

## The grammar

Six kinds, stated as repeatable `--expect` flags or an `expectations` array in the config
file. The authoritative grammar lives on
[`app/expect.h`](../src/hermit/app/expect.h); this is what each kind means:

| spec | met when, afterwards |
|---|---|
| `exists:PATH` | the path is present |
| `dir:PATH` | present *and* a directory |
| `absent:PATH` | not present |
| `preserved:FROM=TO` | `TO` now holds the bytes `FROM` held at baseline |
| `identical:A=B` | `A` and `B` hold the same bytes now |
| `satisfies:PATH=TEXT` | a model judges whether `PATH`'s content satisfies `TEXT` |

Two idioms worth knowing:

- **`preserved:p=p`** is the "do not touch p" constraint — state it for anything the task
  should read but never modify.
- **`preserved:old=new`** is a verified move or copy: the destination must hold the source's
  baseline bytes. `absent:old` beside it makes it a move.

`satisfies:` splits at the *first* `=`; its right half is your words, carried verbatim to the
judge and quoted back in any unmet finding. The other two-sided kinds need exactly one `=`
and refuse a second rather than guessing; a path that itself contains `=` is written in the
config file's object form (`{"kind": "preserved", "path": ..., "other": ...}`).

## The rules paths live under

An expectation names a path exactly as the tree spells it from the root: **no leading `/`, no
`..`**, and a symlink means the link itself, never what it points at. Anything else is refused
before the model is called — a mis-spelled path is not a harmless typo here, it becomes a
permanent `unmet` that the retry loop would hand back as a concrete failure to go and fix. A
set no tree could satisfy (`exists:x` beside `absent:x`) is refused for the same reason. The
reasoning behind the spelling rules — symlinks and `..` make the resolver and the tree walker
disagree about which file a key names — is recorded in [ROUTING.md](../ROUTING.md) §12
step 4b.

## How the verdict is decided

**Structural kinds are decided from snapshots, every turn.** The judge is a pure function of
the baseline tree and the current tree; it never reads the model's reply, opens a file
mid-decision, or asks the model anything. Three outcomes per expectation:

- **met** — decided from the tree.
- **unmet** — decided from the tree, and it feeds the retry: the finding's sentence is the
  "one concrete remaining failure" the next attempt is handed.
- **undecidable** — one side could not be read. Reported to you, **never** sent to the model
  and never retried: a fresh session cannot act on "something could not be read". A tree that
  could not be walked at all leaves *every* expectation undecidable rather than reporting a
  confident verdict about a tree nobody saw.

**`satisfies:` is decided by a model, once per attempt, only after every structural
expectation is met** — fix the structure, then fix the meaning, and no judge tokens are spent
on an attempt that already failed structurally. The judge runs in a fresh session and is
handed ground truth only: the file's bytes as they stand, your criterion, and the tree's
relative path listing. It never sees the transcript — the same discipline as everything else
in the verification path ([D15](../DECISIONS.md)).

Every decided `satisfies:` line carries the label **"(the model's judgment, not a
measurement)"**, because a hash comparison and a model's opinion must never read alike. The
default judge is the working model; `--judge-model` brings another, and the measured guidance
on choosing one is in [chapter 12](./12-choosing-a-model.md) — in short, general models
measured zero false unmets in 120 runs where a code-specialised judge confabulated 14/40.

## How the retry uses it

An unmet verdict re-invokes the model: up to `--attempts` total (default 3), each a **fresh
session** — the measured finding is that these models do not reliably self-correct in place —
handed the original task plus the one concrete remaining failure, composed fresh each time so
the framing cannot nest by the third try.

Two policies protect the meaning of what you stated ([ROUTING.md](../ROUTING.md) §12
step 4c):

- **One baseline per job.** Every attempt is judged against the tree the *first* attempt
  started from. `preserved:` reads its source bytes from the baseline, so re-baselining
  between attempts would let a wrong first attempt silently change what your expectation
  means.
- **Infrastructure is never retried.** Transport failures, a refused session, an unreadable
  tree, misconfiguration — these stop the job. Retry exists for model inconsistency, nothing
  else. A run cut off by a bound *with* an unmet finding is retried: the judge's evidence is
  no weaker for the model having been stopped mid-stride.

Exit `3` means something you stated is measurably undone after all attempts.

## Structure is not meaning — the standing limit

`report.md` containing the literal text `grep -oP '(?<=^).*' notes.txt` satisfies
`exists:report.md` perfectly. That run actually happened — observed live at the terminal, and
kept on the record as an unrecorded observation rather than dressed as a measurement
([D13](../DECISIONS.md)) — and it is why `satisfies:` exists:

```bash
hermit agent --root ~/scratch --model qwen3.5:9b \
  --expect exists:report.md \
  --expect 'satisfies:report.md=one line of prose stating the total number of widget units' \
  'Summarise notes.txt into report.md as a one-line total of widget units.'
```

Handed exactly that planted failure, the 9B judge returned *"The file report.md contains
shell commands such as grep and bc instead of a single line of prose stating the total number
of widget units"* — which is a better retry prompt than any template. But a judgment is an
opinion with a label on it, not a hash; where the judge is blind, silent failure returns.
The benchmark series measured that law from both sides
([chapter 30](./30-benchmarks.md)), and calibration matters in the other direction too: an
expectation stricter than the task's real bar — a byte-identity check on a file the task
legitimately rewrites — stays permanently unmet and sends every retry chasing something no
model can satisfy. Assert the task's bar, not a stronger one.
