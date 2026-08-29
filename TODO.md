# TODO — the publication docket

The punch list between this tree and a public GitHub repository. **Release engineering
only.** Product sequencing lives in [ROADMAP.md](./ROADMAP.md), settled decisions in
[DECISIONS.md](./DECISIONS.md), and the supervisor-side levers the benchmark series priced
are docketed in ROADMAP.md's phase list — hermit-bench's `TODO.md § 2 · Supervisor` points
there, so nothing below duplicates it.

This file should be deletable once the repository is public.

Audited 2026-08-29 on the dev laptop. Two things were checked and came back clean, so they
are recorded rather than docketed: **no AI attribution anywhere in 127 commits** (no
`Co-Authored-By`, no generator watermark, in any message or body), and a `LICENSE` at the
root.

---

## Before the first public push

- [ ] **Commit the MCP documentation sync.** Eight files have been modified since
  `mcp.cpp` shipped 2026-08-28 and never committed: `DECISIONS.md`, `README.md`,
  `ROADMAP.md`, and `docs/{01-what-hermit-is,13-cli-reference,30-benchmarks,90-glossary,README}.md`
  — 41 insertions, 24 deletions. The change is coherent and finished: it moves the
  frontend row from *"CLI today, MCP-over-stdio next"* to present tense, annotates
  ROUTING.md §12 step 6 done, and unblocks E2 in every place that called it blocked.
  It needs a read-through and a commit, not more work.

- [ ] **Scrub `~` from four bench result files.** Fourteen occurrences
  across `bench/fsops/results/fsops-20260813T12{2555,2605,3010,3040}Z.{json,jsonl}`.
  hermit-bench records home directories as `~` and has zero literal occurrences; these
  files predate that convention. Publishing them as-is puts the operator's username in
  permanent history for no benefit.

  **The scrub must not change a measurement.** Fingerprint the scored fields — model,
  task, repeat, passed, valid, checks_passed, checks_total — before and after, and
  require the hashes to match. That is the same proof the SWEEP4 scrub carried.

- [ ] **Fix two Tailscale URLs that would go public.** [README.md:154](./README.md) links
  `local-agent-benchmarks` at `gitea-ec2.tail328f9a.ts.net` — a dead link outside the
  tailnet, and the name is stale besides; the repository is `hermit-bench`.
  [assets/logo/build.sh:36](./assets/logo/build.sh) has a comment pointing at the private
  `aiscrub` repository. Not a security hole, just unnecessary exposure. Repoint the README
  at the GitHub URL once it exists.

- [ ] **Rewrite the README lede.** It currently opens with a backronym and a 6-row table
  of documents. A reader arriving from a GitHub profile gets 90 seconds and learns the
  project has documents. Lead with what Hermit is and the thesis the benchmark repository
  already states better than this one does — *never trust a completion claim; check the
  tree* — then the finding that earns attention.

- [ ] **Flip the cross-references, both repositories in the same session.** hermit-bench
  asserts the supervisor is private in three places: `README.md:9`, `README.md:18`, and
  `TODO.md:90`. Each becomes wrong the moment this repository is pushed — keep the two
  repos' descriptions of each other in sync.

## Recorded, not docketed

**Binary assets carry no readable provenance metadata.** All 21 files under `assets/`
were scanned 2026-08-29: none carried metadata of any kind, and none carried AI provenance
markers. Re-scan before the push if any asset is added, regenerated, or re-exported in the
meantime.

Be exact about what that proves. It proves the files carry no *readable* provenance
metadata — C2PA, XMP, EXIF, IPTC, generator exhaust in PNG text chunks. It says nothing
about pixel-level watermarking, which survives re-encoding and has no public detector to
check against.

## Not blocking publication

The Kitchen model tier and the E2 collection are results work, not release work. Both are
docketed where they belong — hermit-bench's `TODO.md` and this repository's ROADMAP.md —
and neither needs to land before the repositories go public.
