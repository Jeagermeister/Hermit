# TODO — the publication docket

The punch list between this tree and a public GitHub repository. **Release engineering
only.** Product sequencing lives in [ROADMAP.md](./ROADMAP.md), settled decisions in
[DECISIONS.md](./DECISIONS.md), and the supervisor-side levers the benchmark series priced
are docketed in ROADMAP.md's phase list — hermit-bench's `TODO.md § 2 · Supervisor` points
there, so nothing below duplicates it.

This file should be deletable once the repository is public.

Audited 2026-08-29, re-checked 2026-09-01 and cleared 2026-09-04 on the dev laptop. Two
things were checked and came back clean, so they are recorded rather than docketed: **no AI
attribution anywhere in the commits** (no `Co-Authored-By`, no generator watermark, in any
message or body), and a `LICENSE` at the root.

---

## Before the first public push

- [x] ~~**Scrub the operator's home directory from four bench result files.**~~ — **done
  2026-09-04.** Fourteen occurrences across
  `bench/fsops/results/fsops-20260813T12{2555,2605,3010,3040}Z.{json,jsonl}`, each replaced
  with `~`, which is the convention hermit-bench records under. The scrub changed no
  measurement: the scored fields of every record — model, task, repeat, passed, valid,
  checks_passed, checks_total — were fingerprinted before and after and the digests match
  (the same proof the SWEEP4 scrub carried). Zero literal occurrences remain in tracked
  files.

  **Then the premise turned out to be wrong, and the fix went further.** The repository was
  already public: a Gitea push-mirror to GitHub, synced on every commit, had carried the
  four files into public history. So on 2026-09-04 the history itself was rewritten on
  Gitea (`git filter-repo --replace-text`) and force-synced to the mirror: the home path
  became `~` in every commit, and a second form the docket had never listed — the username
  as an `ls -l` owner column captured inside a check's `detail`, in two older result files
  — became `user user`. Verified before the push: 186 commits intact, every branch's file
  list identical, the scored-field fingerprints of all six files unchanged, commit
  messages untouched. The mirror was set private for the duration; making it public again
  is a separate, deliberate step.

  **Not finished, found 2026-09-06.** Two corrections to the paragraph above. One commit
  message *was* rewritten — filter-repo rewrites abbreviated hashes it finds in messages, and
  one message cites a commit by its short id. And the 186 figure does not reconcile with what
  is measurable now: 168 pre-rewrite commits are reachable from the backup tags, and the
  remote holds those 168 plus 13 made since. More seriously, **the scrub did not reach the
  GitHub mirror.** A force-push unreferences objects, it does not delete them, and GitHub
  still serves the pre-rewrite commits by id — a blob still containing the username was
  fetched from `raw.githubusercontent.com` on 2026-09-06 at a force-pushed id. Docketed as
  [DOCKET.md](./DOCKET.md) 1.14; the fix is a GitHub Support request to garbage-collect
  unreachable objects. The old→new mapping is [docs/91-commit-map.md](./docs/91-commit-map.md). Every other clone (Kitchen, Framework) diverges the way
  the August attribution scrub made them diverge, and takes the same tag-then-reset.

- [x] ~~**Fix two Tailscale URLs that would go public.**~~ — **done 2026-09-04.** The README's
  evidence list now points at [hermit-bench](https://github.com/Jeagermeister/hermit-bench)
  and names the private tournament repository without linking it; the logo build script's
  comment names `aiscrub` without a URL. No Tailscale hostname remains in tracked files.

- [x] ~~**Rewrite the README lede.**~~ — **done 2026-09-04.** It opens with the thesis — *never
  trust a completion claim; check the tree* — then what Hermit is in one paragraph, why it
  exists in one, and the E1 headline at its real width (74% → 94%, seven paired tasks,
  p = 0.125, reported as underpowered). The backronym survives as a footnote to the lede,
  and the document table is where it was.

- [x] ~~**Flip the cross-references, both repositories in the same session.**~~ — **done
  2026-09-04, differently.** hermit-bench's three sentences no longer say the supervisor is
  private; they say it lives in its own repository and nothing about its visibility, so
  nothing becomes wrong the moment this repository is pushed. What remains for push day is
  additive: put the GitHub link into those three places once it exists.

## Recorded, not docketed

**Binary assets carry no readable provenance metadata.** All 21 files under `assets/`
were scanned 2026-08-29: none carried metadata of any kind, and none carried AI provenance
markers. Re-scan before the push if any asset is added, regenerated, or re-exported in the
meantime.

Be exact about what that proves. It proves the files carry no *readable* provenance
metadata — C2PA, XMP, EXIF, IPTC, generator exhaust in PNG text chunks. It says nothing
about pixel-level watermarking, which survives re-encoding and has no public detector to
check against.

**Housekeeping done alongside, 2026-09-04.** The never-run Phase 0 diagnostic harness under
`bench/` was removed (ROADMAP.md Phase 0 records where the real one lives); three stashes and
the `fix/compaction-guard` branch, all superseded by what merged, were dropped; and the
stale `origin/fix/note-injection` was deleted on Gitea.

## Not blocking publication

The Kitchen model tier and the E2 collection are results work, not release work. Both are
docketed where they belong — hermit-bench's `TODO.md` and this repository's ROADMAP.md —
and neither needs to land before the repositories go public.
