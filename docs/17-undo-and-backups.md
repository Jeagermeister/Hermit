# 17. Undo and backups

Every mutation that would destroy bytes preserves them first (R4). This chapter is what that
buys you when something goes wrong, and — stated with equal care — what it does not cover.
The design record is [D14](../DECISIONS.md).

## The store

One generation per mutation that would destroy bytes — a `write` over an existing file, an
`edit`, a `delete` — landed
in a directory **outside the sandbox root**: by default `.hermit-backups-<root name>` beside
the root, or wherever `--backups` points (it must be outside the root). Outside for two
load-bearing reasons: the model must never be able to list, read, edit or move its own undo
data, and your tree's `find` and `list` results stay free of archive noise.

Two mutations deliberately take **no** generation, and that is correct rather than an
omission:

- **A create** destroys nothing — and it cannot silently replace, because creation publishes
  by link-no-replace, so a file that appeared between check and publish is preserved and the
  call refused.
- **A move** never destroys bytes (`RENAME_NOREPLACE` structurally cannot replace a
  destination, and the content is hash-proven at both ends); its undo is moving it back.

## Using it

```bash
hermit undo --root ~/notes            # list the generations; changes nothing
hermit undo --root ~/notes --restore 3
hermit undo --root ~/notes --last
hermit undo --root ~/notes --prune
```

**The bare invocation is the listing, always.** An operator reaching for undo is by
definition recovering from a mistake, and a recovery tool whose default mutates is how one
mistake becomes two. Both mutations are an explicit flag away.

**A restore is itself undoable.** Restoring generation N first preserves the target's
*current* bytes as a new generation, then publishes atomically — the same discipline as the
`write` tool, applied to the supervisor. Redo is not a feature; it is undo of the undo. The
restore path also re-resolves the recorded target through the sandbox on the way out, so a
hand-edited store row or a planted symlink cannot carry a restore outside the root.

**A deleted file comes back the same way.** `delete` ([D19](../DECISIONS.md)) preserves the
bytes before the name goes, so its generation sits in the listing like any other. Restoring it
recreates the file at its recorded path — and, since absence has no bytes, preserves nothing
first.

## Retention

Generations older than `--keep-hours` (default **72**) are pruned when an `agent` job starts
and on `undo --prune` — nothing else ever deletes from the store. The reasoning, verbatim
from the decision: supervised trees live under git, so the store covers the gap between a bad
mutation and the operator noticing. It is not the archive; git is.

Because pruning is destructive and runs automatically on an operator-supplied path, it is the
most guarded operation in the module: the store drops a `.hermit-store` marker at its root on
first use, and prune **refuses any directory that lacks it** — a `--backups` flag pointed at
the wrong place must never delete things that merely have numeric names. Generation numbering
never rewinds across pruning, so a new backup can never take a pruned one's identity in a
listing you half-remember.

## What undo cannot do

Stated here so it is read before it is needed:

- **It cannot remove a file the model created.** Creation preserves nothing — there were no
  pre-mutation bytes. If a run littered your tree with new files, the changeset names each
  one; removing them is yours to do (and `preserved:`/`absent:` expectations are how you make
  unwanted creations a *judged failure* next time).
- **It is per-file, not per-job.** "Put the whole tree back to before the run" is a sequence
  of restores you read off the listing, not one command.
- **It does not cover what `shell` did.** Only the structural mutating tools feed the store.
  A shell command that overwrites a file bypasses it — one more reason `shell` is opt-in, and
  one more thing the per-turn changeset (which does see shell's effects) is for. A shell `rm`
  bypasses it too, which is why `--delete` exists beside `--shell`: a removal path that is
  both gated and preserved ([D19](../DECISIONS.md)).
