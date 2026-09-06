# 91. The commit map from the 2026-09-04 history rewrite

On **2026-09-04** this repository's history was rewritten to remove two strings that had been
committed by accident: an absolute home-directory path, replaced with `~`, and an owner column
captured from `ls -l` output inside two older result files, replaced with `user user`. The
rewrite was a `git filter-repo --replace-text` pass over every branch.

Almost nothing else changed. No file was added, removed or renamed anywhere, and no byte of any
file differs except through those two substitutions. One commit message changed: filter-repo
rewrites abbreviated hashes it finds in messages, and one message cites a commit by its short
id. But a rewrite renumbers every commit whose tree it touches, so **159 of the 168 commit ids
that existed before that date are gone**. The nine survivors are the earliest commits, which
predate the result files and so had nothing to substitute; they are in the table below with the
same id in both columns.

The ids the measurements pin the supervisor at are among the 159. This file maps them, and
every other pre-rewrite id, to the commit you can fetch today. It exists because the
measurements are meant to be reproducible by someone who is not us, and a pinned commit that
resolves to nothing is not a pin.

## The ids the measurements cite

These are the five pre-rewrite ids recorded in **hermit-bench**. A reader holding the id on the
left wants the commit on the right.

| recorded id | fetchable id | where it is recorded | the commit |
|---|---|---|---|
| `a5722dc` | `68cfc63` | E1's provenance table; one `harness_commit` in `fsops/results/`; three `hermit_commit` in `delta/results/` | Merge PR #24 — re-invoke the model with one concrete remaining failure |
| `6937c1d` | `a897336` | hermit-bench's README, as the commit this repository was extracted at | Run E1, the reliability experiment, and fix what the measurement found |
| `070da1e` | `043ba6f` | E3, E4 and E5's provenance tables; six `hermit_commit` in `delta/results/` | Merge PR #26 — guardrails: undo + retention (D14), and the semantic judge (D15) |
| `6a5ecd4` | `3cace10` | one `harness_commit` in `fsops/results/` | Turn on LTO as an opt-in `HERMIT_LTO` build option |
| `7cce819` | `b79d48b` | two `harness_commit` in `fsops/results/` | Move work trees out of the repo |

This repository's own `bench/fsops/results/` carries the same three `harness_commit` ids, and
`bench/delta/E1-RESULTS.md` carries `a5722dc`.

The ids recorded *inside* the committed result files are left as collected — they are what the
harness observed at collection time, and editing a result to match a later history would be
falsifying the record. Resolve them through this table instead. hermit-bench applies the same
rule to its own earlier rewrite, in that repository's README under *Provenance*.

## How the map was checked

The map was not inferred from commit messages alone. Every pre-rewrite commit was paired with
its rewritten counterpart by author timestamp and subject — 168 pairs, one to one, with no
timestamp-and-subject key shared by two commits in either history — and then each pair was
verified against the trees themselves:

- **19,634 blobs** compared, across all 168 pairs.
- **File lists identical** on every pair: no path added, removed or renamed anywhere.
- **1,120 blobs differ.** Every one of them is byte-for-byte the pre-rewrite blob with the two
  substitutions applied — checked by performing the substitution and comparing the resulting
  object hash, not by reading the diff.
- **0 unexplained differences.**

Comparing trees cannot see a changed commit message, so messages were compared separately:
**exactly one differs**, for the reason given above.

The map is complete for everything anyone can fetch. The 181 commits reachable from this
repository's branches on the remote are 168 with a pre-rewrite id, all in the table below, plus
13 made after the rewrite. (`TODO.md` records "186 commits intact" from a count taken at
rewrite time. That figure does not reconcile with anything measurable now, and is not the
basis of this map.)

## Where the old objects live, and where they still do

Two places, and the second one matters.

The pre-rewrite tips of 34 branches were kept as `backup/pre-rewrite-20260904/<branch>` tags on
the operator's laptop clone when the rewrite was force-pushed. Those tags were never pushed and
will not be; pushing them would republish the strings the rewrite removed. All 168 pre-rewrite
commits are reachable from those 34 tips, so the local coverage is complete even though the
remote carried 51 branches at the time.

**The pre-rewrite objects also still exist on GitHub, and are still publicly served.** This
repository is mirrored there, and a force-push does not delete what it unreferences — it only
makes it unreachable from any branch. Checked on 2026-09-06: the pre-rewrite commits are
returned by the API by full id, and a pre-rewrite blob still containing the username was
fetched over `raw.githubusercontent.com` at a force-pushed commit id. So the 2026-09-04 rewrite
achieved its goal on Gitea and on every fresh clone, but **not** on the mirror, where the old
objects remain reachable to anyone who knows an id. Removing them there needs GitHub to
garbage-collect unreachable objects, which is a support request, not something a push can do.
Tracked in [DOCKET.md](../DOCKET.md) 1.14.

This is why the disaster-recovery argument for committing the map — "if the laptop is lost, the
old ids become unrecoverable" — should be read as weaker than it looks: today the objects are
recoverable from the mirror by anyone holding an id. It is also why the left-hand column of the
table below is not inert. Publishing it is a deliberate choice, made with the sentence above in
view.

## The full map

Author-date order, oldest first — which is nearly but not exactly topological, so three rows
list a parent below its child. Left column is the pre-rewrite id, right column the current one.

| pre-rewrite | current | subject |
|---|---|---|
| `cc62c3f51ad314cafb9084772dba89d338c00ecd` | `cc62c3f51ad314cafb9084772dba89d338c00ecd` | Initial commit: parity tracking against upstream Hermes Agent |
| `e8c669d5430af41c70e3f7df898d4aaccb04173e` | `e8c669d5430af41c70e3f7df898d4aaccb04173e` | Add MIT license |
| `c46633583fa0009ad4a98ecd43486b63a833c624` | `c46633583fa0009ad4a98ecd43486b63a833c624` | Rescope: local-first Ollama supervisor, not a Hermes port |
| `17df7196a3f35610341d0f699fe5ac85b315e9a7` | `17df7196a3f35610341d0f699fe5ac85b315e9a7` | Add bench/: the Phase 0 Hermes-vs-OpenCode diagnostic harness |
| `8b2db8810519a7ef7cf652733091c483579bccc7` | `8b2db8810519a7ef7cf652733091c483579bccc7` | Point at local-agent-benchmarks as the canonical evidence location |
| `cf36802e29ee456db4fe50d2c34998ade81d8883` | `cf36802e29ee456db4fe50d2c34998ade81d8883` | Follow the local-agent-benchmarks directory rename |
| `4dbdf2141c30359d53ba78c5ea54605eced0e01e` | `4dbdf2141c30359d53ba78c5ea54605eced0e01e` | Note the Phase 0 deadline and what 72GB changes |
| `b0a263b197b80860b450c0dff711c7154499eb2e` | `b0a263b197b80860b450c0dff711c7154499eb2e` | Add opt-in determinism and GPU recording to the bench harness |
| `bd2ff18557dbc6f3b2cf22e26e8905dc02f1bd8b` | `bd2ff18557dbc6f3b2cf22e26e8905dc02f1bd8b` | Add fsops: filesystem-primitive benchmark for small local models |
| `34271e0dcdbd4f857a7498b7dd37edd264f4c281` | `c97ca814a56b5c8a5049de668216fa75d13e0a3b` | fsops sweep 2: three non-thinking models, and two corrections |
| `7cce8190cd8942cb7b5d5b50c37806efdf234dde` | `b79d48ba2c5b044485ca7689cf17aeacda8270dd` | Move work trees out of the repo; agents were writing correct output into it |
| `8b1bdfbd051d76e1f2bec7578493c4a989329fbf` | `1313d586a42fbf6151942ee4303844ace12ca8bf` | Confirm the escape mechanism: file tools, not the shell |
| `46774ab80344f52782dad458ff6d013f64e3b7e4` | `ede54cb31ec9437a322a2aaff77ccc5227acfaa7` | Add REQUIREMENTS.md: nine requirements, each traced to an observed failure |
| `524a832dc0c744108b07eab4a0cf63785d89e9a0` | `c947cdbef44a6b2241703b55df2c1a4bc12cee88` | Drop the SIGIL naming note - that is a separate project |
| `f7526d10490952c9da66959fd1fe084694b0d353` | `26f2b505215145d30483ab267fb028404e4ae4ee` | Narrow scope to module granularity; add SCOPE.md |
| `f7402f84efdfa3025d854224a2aeddacf4f23444` | `db83b1b4ed9bd6b03e8812607e58ea07efe75983` | Add build system and the R1 sandbox |
| `6250490ccad640cfc52de33f61568fdd68138581` | `a70339657cd6cc3d3cfe43baf7407fa9e308aaa1` | Settle the five pre-code decisions; document building |
| `2db88ca769f459a360a3636b8d3bff70f6f9e818` | `217dde4a2ca9d463690a87a717312c8e4a02f3f2` | Record the Phase 0 outcome, and the blocker on the matched suite |
| `5a6ceafd23766e6fb495bd292aecc1fb35440a87` | `ed50100a5f7ccccf491acac003ab87366336bc0c` | Correct R5: the line-number prefixes are Hermes' own rendering |
| `212cb768f6a8a40a7d573813be08ef9e49a369c1` | `5d7b7b86d18b914cd4acc5ac541dd719de0b0c79` | Finish the R5 retraction, and correct figures across the docs |
| `cd9e37130863e08c919cb849ee7e580ac275ddc4` | `db6e26b564e27a329dd4347fc788d8564399f147` | Settle the frontend and backend policy as D7 |
| `3f9c42c274b7d66a9e85167439f1cc5f9e124ad1` | `18627991e704ef72d112e1a7f181e65fe6141a4e` | Close a fail-open in path resolution: EACCES is not "absent" |
| `ac90ed5f125491c9e667e96aace18f3f229fe9a1` | `7ad3c7da8064232af8e663433f8217c61effec9c` | Add the Ollama client and the R9 preflight; correct R9's evidence twice |
| `a43d07e5610e9810d2b99dd247a00795bdb9ff82` | `bafa093399b1ed1d1c146d932f92d137172395b0` | Correct two false claims in the sandbox's own comments |
| `92e5dfd1ba56cf81cb2d8d3717a7e0765bb18864` | `66b9c36d5db9512bf9a7f78f9f9db2ae9dfb417d` | Add the config layer and the CLI it feeds; apply R1's rule to configuration |
| `f1b8ab6aa278abcb3778add198b8b18bfdd67e34` | `0ae0f002285dd6dc13ba25a457db10e806a50c76` | Add the session layer: decide what history to drop, because the server will not say |
| `a873834f8b27104ef19a1edc4539454d73a810d4` | `606c1ca8358fa8ff5349b5c272655766a0271d33` | Add D9: two local backends, decided in principle and deliberately deferred |
| `25e88c5a1b861e92a0b524f4422ce18f41d2bd5e` | `617a40ddf094a62b13fd5aa85c826d1b4df9afe4` | Merge pull request 'Add D9: two local backends, decided in principle and deliberately deferred' (#1) from decisions/d9-two-local-backends into main |
| `8bc42a1d027bf13c8c9464266967349ef393cccb` | `52ed541e9d289d05f7e740f8dabcf6c8e9e34638` | track: add agent/deadline.py to the ledger, record the bounded-execution question |
| `bd76ee62c02027c186e7e6c6759dc9d3f5655e3f` | `aaa091b85bbc597b9ca04b9cc6fa677f45522d78` | Merge pull request 'track: add agent/deadline.py to the ledger, record the bounded-execution question' (#2) from track/upstream-deadline-layer into main |
| `4ad644bdc0e650e933cf6b3beb45ccb30cf700ae` | `1c549b70df19edb2caa7a7ec7ff5710b3535365b` | docs: settle the tool surface, the tier split, and who may call what |
| `f7e83c37286285b802b96350183a1d713bf83d7f` | `951e748b51d4eec54913115e3e3d2c563c5fb399` | Merge pull request 'Settle the tool surface: tiers, the eleven tools, and who may call what' (#3) from docs/routing-tool-surface into main |
| `de5186564d792b7d23e948f92d34761a38902e98` | `f7492ad8541eb548990c052a4b664983c81e7c0e` | docs: record kernel confinement as D10, and what it does not close |
| `04f0aa78e88d8666476ada8b78ef0ff4dd29ac96` | `b35c45719bd73d3c6a703f8f046431be0df72c46` | docs: make D10 implementable from the repo alone |
| `336b24392ad219e89076eea7d962ecefc61ca1f2` | `a96202605d2007222477ef026fff794d5d68e7a8` | docs: bring ROADMAP's checklists in line with D10 |
| `73b3063c08a722dde99f4d0f58062fd6cb72c63c` | `e7d27b9fc8bfaf79306ed8f378077d6483c947a7` | Merge pull request 'Record kernel confinement as D10, and what it does not close' (#4) from docs/kernel-confinement into main |
| `076ffcb3d4783b07920716a1aff8513bd2248332` | `839f90548a328862fcbba9633d556af902fd46e7` | docs: probe the substrate (D11), scope Windows separately, name Kiro |
| `db582b57f761e8c5a10f3c8b59098ea67d505a32` | `a8a08769aba016512d445e460248c130a9107f03` | Merge pull request 'docs: probe the substrate (D11), scope Windows separately, name Kiro' (#5) from docs/substrate-probe-and-platform-scope into main |
| `e5c30b8cd01a9e059a541f470c2587e6e89d97db` | `087a27f31224d9dadb1b425c3a2022ad8edc350a` | Add the two link edges the tool surface needs |
| `a04c33d5cd832c419901847e927899d8a4df4fdd` | `ad95410a243cdfd0ff2dc6d588ddd9a9b0e9f350` | Add tool.h: D4's virtual base and the JSON-free descriptors |
| `64531a844937eae51991f3d5a2b33aabb42af5bb` | `95323e9bcf5e2f9fd15e1210f0954cb40412b6fb` | Apply the five-lens review: bind ToolArgs to its spec, fix four false claims |
| `7457652983fa22fce0a57b7d13c26d27a1b7dfc0` | `c547824813e8553a759d0190edb2779390a366d9` | docs: settle the edit staleness tuple as session state, never an argument |
| `e9169b9172ad46a8b11897c22a47ed87e3903ed5` | `728763733eba98afde8984df5e56adca08dabe06` | docs: grep returns sibling fields, not a colon-joined line |
| `01292b81f1d29a5a06939624eeaa32ec4a2e6010` | `2e8c4edbb96ea80791a1071b378f60ee0f493110` | track: the ledger measures porting parity, so its empty status is NOT_PORTED |
| `c29c366b480e4836d3432bf23372116d23495f6b` | `be0e702b202eca8972f5c855326e53e955de1163` | docs: settle the open() question -- allow the semantics, funnel the spelling |
| `f900b110228ca34fb945c46c6bea46c34febc766` | `038bff6938ab4f8c3b1e336e951ff3e61567fa5d` | build: force httplib's zstd toggle off, and record the rule that keeps the list current |
| `e25227d4c4bb3f08f208ff0c06cb26a4b0d30758` | `855fdef085e0e3685b879bd85f9a0eedea5b821b` | Merge pull request 'Steps 1 and 2 of the tool surface: the link edges, and tool.h' (#6) from core/tool-interface into main |
| `1fe03bd530a70cb2d63112cc09c5b87ba60bda65` | `78a8355c8b492ecfbcb349cd9a04ede6317e584e` | Merge pull request 'Close out the five-lens review's open items' (#7) from docs/close-review-open-items into main |
| `f3ff467a6cc9eb3a52f1a172b4aaceb8b5397777` | `1a1ebd1e29b7fdf7971da2f3139381ca3c201830` | Add the open funnel, SHA-256, and the first three Tier 0 tools |
| `aae686aa3a07c001a668ecfe3dd3d5966d9414f3` | `c99b3a428de6f272ff014011bf4fdc4d0d6a9525` | Complete the observe surface: find, grep, and the read cap |
| `38b63e3709aa01f1c42786c5c6e2d60c7485422c` | `f1d74ee80027f23052e10b0d2bc412473a9ea8eb` | Merge pull request 'Step 3, first slice: the open funnel, SHA-256, and read / hash / list' (#8) from core/observe-tools into main |
| `e1efc44cabf279ffbb775fc04661a644f5c96d7d` | `9fb754bbf017e4683c8dbc6740cbf84846692913` | Complete the eight Tier 0 tools: write, edit, move, and the observed-state guard |
| `4da1d3cbc0494642af51472d6a0e1cfab20f93eb` | `37ed4c34514496c6a5aba5d684d157e31ed4e96f` | Apply the five-lens review: fix a use-after-free, harden the backup store, commit read observations atomically |
| `ff5d45c0e419793a0341a8088ba0e581b54b66a7` | `d368da42c89b347d82f4a80def6c08797cc7f05d` | Merge pull request 'Step 3 complete: write, edit, move, and the observed-state guard' (#9) from core/mutate-tools into main |
| `2a8fd40b9aab2188a82f5b7a984a964504eb318c` | `9241ddc791e15c44fd82878e78515c0f66e7550b` | docs: give the README its use-case stories, and true up its map |
| `71a4e1a1bf91426bc6ad51ee6508739f084bc25d` | `4cf1888ad55a727c6f33b42357920acd86ee98cb` | Merge pull request 'README: the use-case stories, and a trued-up map' (#10) from docs/readme-use-cases into main |
| `d314b2c74f7fa016cce94f78e79f75910a89ead7` | `a468eb1357fa5a728b68b2e52decf738e6fef2f6` | docs: design bench/delta -- the before/after measurement, argued before any run |
| `435320869fe867a9bc629ed455a24b044dd8eaa1` | `6f680d779b935b017280add37f4166d342a99eae` | Merge pull request 'bench/delta: the before/after measurement, designed before it runs' (#11) from docs/bench-delta-design into main |
| `0954f3505d3b9908527fa7b03c681a05c0fed9fb` | `e0381827c3df3ff2adc621fa5fb8fb88ae80128f` | docs: record the questions this design gets asked, with what each answer concedes |
| `53056070549ca74addf7051c7ff4918085622721` | `40a548c3d9ed9bdf3a890d92d25ade92c4d7e7e4` | bench/fsops: stop transcripts-off runs inheriting a stale transcript |
| `9bafaf6865f1d7b00f906f6087f762f2eb672b08` | `d8e0a4c323e37918b7e3543ddd0ef1e52825faed` | docs: design bench/distill -- what would have to be true before training a worker |
| `f1482a957d1fe81568ccaf8366977dda9cf5a64c` | `579bfbdfabeff9c0e208b0fe54dfd7c0e1507de5` | bench/distill: draw the gate order, so the one binding edge is visible |
| `0c7469d573f07dc8471f2f65552ded3aa53804de` | `614757bf785da5430207112ed54b6be9ff1a1f42` | docs: draw the flow -- request path, one mutating call, the supervisor turn |
| `5326260a4b268aa65553a644001eb7069083903f` | `8467b6fb9a331ba706f1eced9c5677f7233c7993` | FLOW: correct the mutating-call diagram -- move is not write |
| `1569898fed902cf7c1639125adb6f20b54215a73` | `8af98a7259b85cf89ce03e0be484a7cae87084a0` | bench/distill: true up the corpus cost table |
| `47e0e8de1555695c44ee00e822060a8c87f8fe45` | `d0cf32e8ed096eaa5cbfaab156af6a4deca01157` | Merge pull request 'FAQ: the questions this design gets asked, with what each answer concedes' (#12) from docs/evaluation-faq into main |
| `29d13deab13250897a5f6aa7d4ab8cc294f19612` | `8e6e943fa60eb6fdabc0c8f4794a4c388c75cca7` | Merge pull request 'bench/fsops: stop transcripts-off runs inheriting a stale transcript' (#13) from bench/fix-transcript-contamination into main |
| `cf065840000f599d3d2ccb2451c9909cd60767e8` | `d38fcadf4120614a638267137743a667b02c652c` | Merge pull request 'bench/distill: what would have to be true before training a worker model' (#14) from bench/distill-design into main |
| `afbdb92b75b27bf5868b02ddd07abdad76122ac1` | `708c4ea1a8df59ec92d71eb0359e6e60bc9d0cca` | Merge pull request 'FLOW.md: the architecture drawn — request path, mutating call, supervisor turn' (#15) from docs/flow-diagram into main |
| `1e72cfe7ae239e85a89cc1a5b38ee6f40a14dbd8` | `49913d4a72acd68d14504e9200775ef1a924866c` | ollama: put tool calls on the wire, and keep `format` off that path (D12) |
| `a96390bd8f7988cf5f5cf523e84f13a19115d79e` | `b6c71fd909e2bd634930bb6e7d00778e3174c9c5` | Phase 2 complete: the agent loop -- dispatch, bounded turns, grouped trim |
| `363e5496211911a942334c1036259937e0d4d7ae` | `f263f0d7bf53579cc33a8e49e9be5b70a0572ce3` | Record why llama3.x loses its tools mid-run, found by the loop's first weak-model run |
| `0ed8389e26d5093baf9e430c154a4422a431e577` | `31eee333c6cae02ee4e4eb5f4218787fcd4f1421` | Apply the review: a crash on binary files, two silent bounds, and tests that tested nothing |
| `9dae14d29cc2d442a29af7e8307ad409b56cb4b4` | `bf19470fa5cf5458fa3bc9bf7c35c10f3aa2abed` | Independent re-measurement: D12 confirmed, two of my claims corrected, one new hazard |
| `df01bc0f5e28efc3c307e7d5c1e9f1780a6b1bcb` | `92a6cf91159a4b51faf1aa6949c550b78fe53822` | Merge pull request 'Phase 2 complete: the agent loop, D12, and the review that found a crash in it' (#16) from core/agent-loop into main |
| `e8027da5f0b1071146e9cd8a79e626295039fed6` | `34a64f86e0ddcfd1210421663009a055a904652d` | Phase 3, first half: per-turn state verification that never reads the reply |
| `dee59953b074b0b688ed8d9392a7bcea1da89f7b` | `7b59cc050d222620681bf26d4c46a59d3132550d` | Review fixes: a fabricated verification, an R4 hole, an fd leak, and two claims that were not true |
| `546975e5f60c1cee54322d23c096d2194d445eda` | `f40999d1d51175788ca64a3b0d7ebc361368fb8c` | Merge pull request 'Phase 3, first half: per-turn state verification that never reads the reply' (#17) from supervisor/state-verification into main |
| `cd42309474deab5e6db8072abcbd058f0527e434` | `cf0720feaf5f8bc512d823454b2ba5c096d91156` | Phase 3, second half: a judge that decides structure and says so when it cannot |
| `cc9aa57ac3120ba47b1cb8bb88580b4e595b8f3c` | `ac0423d6475f3b7f44bef569deb595dbed1c0e48` | Record permission bits, so a chmod stops reading as noise |
| `a2db29a0aed6afedd1499cc2b2f514a6a0d8d0ca` | `19bda5ad9ee877f5add6a6417869bdd50897c5ce` | Merge pull request 'Phase 3, second half: a judge that decides structure and says so when it cannot' (#18) from supervisor/judgment-half into main |
| `47f7e005ded89854bdce2c188d4aa42d8074d6e0` | `4fed8c27bdd014bee8c4b0a6413d67817bed1a4a` | Merge pull request 'Record permission bits, so a chmod stops reading as noise' (#19) from verify/permission-bits into main |
| `b95a663a6a671c6db40d700cafa827a93819b38d` | `271d2b6a53d7cd9d29208784d97a872ebc500ca7` | Cut comment narrative density across the codebase, and fix what turned up along the way |
| `d1d8683357b6673851ef1fa1541958a1d52fa2f2` | `cd1a4415785ef60e25ecb013f08f5a27f80e9718` | Merge pull request 'Cut comment narrative density, and fix what turned up along the way' (#20) from cleanup/comment-density into main |
| `c03b30be017655fabd168c74118b7ab6756a74d7` | `8a88022d839a5a5481a1b855eab14019fb26c28e` | Rename the project: Hermes-Cpp becomes Hermit |
| `841687f388933e4d5f887c380fd168aa8c370be0` | `95819a09126f43f1ce135eb1347b7752fcae45f9` | Merge pull request 'Rename the project: Hermes-Cpp becomes Hermit' (#21) from rename/hermit into main |
| `2d71214179ca51a8f881c9fadb2f2d078deccf8f` | `30c43362ff2368f2521ed4b19be146e341be104f` | Spell out the Hermit acronym in the README |
| `3f4978a7e1f26820df1fd8d4061e5c8df98b6414` | `cbc19f8395191607fffa0cd21be5529dbc2b2680` | Merge pull request 'Spell out the Hermit acronym in the README' (#22) from docs/acronym-tagline into main |
| `f521369a994067f642fc638af78bd840393bf97f` | `8c8610c3919312cad290bfbbf9ccdfb7ad891dc1` | Wire the judge to a caller, and fail closed when the tree cannot be read |
| `3a76bbac2d17349ac5db4a9c05761b93cd2c480e` | `5e6e2b391bd7d7bdb35c6bf699735c44db3d30e5` | Merge pull request 'Wire the judge to a caller, and fail closed when the tree cannot be read' (#23) from supervisor/wire-the-judge into main |
| `b96e730d4962d93f91155dae1729bcced5447ef7` | `a598dd34853e58797da422cfeb5ee10b2269fed1` | Re-invoke the model with one concrete remaining failure |
| `a5722dce6d723c49e53290571022399710a7710c` | `68cfc639193d70cc312063e6ec7e222d68004bdd` | Merge pull request 'Re-invoke the model with one concrete remaining failure' (#24) from supervisor/reinvocation into main |
| `6937c1d022074fa4f09691ab8fda32c04efc568a` | `a89733611a784bf3d99e9d10197fa91fa701296b` | Run E1, the reliability experiment, and fix what the measurement found |
| `ea40fdd6f50ea41d0684c263c0df1936782c0242` | `df1b6fedbed26e90e4e435b7d74ecb2b720926ec` | Order result-file selection by the filename stamp, never by mtime |
| `757862240f173e7a3fce273229c6433436764234` | `25babdcbadfacfb2334fae2cb0dad23d0bdc0825` | Move the result artifacts to hermit-bench, keep the suites |
| `7c45a15b5dc2b5dcae269629d750cdd00f36e2de` | `d771fb61a644a4fac0458744042f1ee1957150de` | Merge pull request 'Run E1, the reliability experiment, and fix what the measurement found' (#25) from bench/e1-reliability into main |
| `9b604121dbe8c4ab2b5cc5020fabed2dbddb2586` | `b6f8edafd318715f2886ae7d713d4936c02f94d9` | Close R4's undo half (D14) and build the semantic judge (D15) |
| `cf92d195939d71671844d3597288a142b1328189` | `616d933bf77922edb4800e968193a408054d80f6` | Fix what the second review pass confirmed in the first pass's own fixes |
| `3b9bc985ccc606b3a5387165e68d2dd13efdd46c` | `8c1a1d878d552129eca847c43d711759e7cfc970` | Cut comment narrative density in D14/D15, and fix one factual slip |
| `070da1e4ccd8d71ebc0eb2c1d3e24bf8328cb365` | `043ba6fe9edbba92ef6cfbfffdf804920429606e` | Merge pull request 'Guardrails: undo + retention (D14), and the semantic judge (D15)' (#26) from guardrails-and-semantic-judge into main |
| `9d4f69fb0ca9b0bf8d37e101c2b432951e9e78c3` | `9dd3c9ac2f44538e2f542624f926f3047590716d` | Roadmap: docket the measured levers from the E3-E5 series |
| `152ced06ea94b07907b899b3544737f5c804f7c7` | `57630d3dda5c1fdfc6ab1be8b587d4016f99f181` | Merge pull request 'Roadmap: docket the measured levers from the E3-E5 series' (#27) from roadmap/measured-levers into main |
| `a8e4d7ff6c4addfaf055b33c49d5d911a67fd736` | `7af8b1492efbc439434197def9920a97fa05f177` | Performance: the CPU-floor findings from the 2026-08-19 review |
| `d3c90258095d016882c64a8b257ed83d0d016d06` | `91a96e3e2af57ad598abc7436308bd5318115000` | Tokens are the wall clock: slim tool rows, trim with hysteresis |
| `6fdd9dba017359a79be20b046d110f4cea8e2dea` | `5affc8cc74cc4a8322dca4c45bc2da4c4786c72c` | Merge pull request 'Performance: the clean package from the 2026-08-19 review' (#28) from perf/review-clean-package into main |
| `2fcaf787f3a7a814313a75b13d1dd25c65d9cc19` | `972cc1382ef5ee73d8e5f94380b3430e1df55811` | Build the D10 kernel confinement mechanism |
| `5c8bae70e6fc04ff6d226a6d3820dc3cea011b45` | `a3bd36452ec0a68440a2cc40edf071ef255f1edb` | Build the shell Tool: R8 bound, capture, and close D13's gate |
| `bb71c637dbe92904fdda86a4c4965a756f2c983c` | `7eb05ae5e3b295fbe19a6355f2d4190bdf0076ee` | Merge pull request 'Build the shell Tool: R8 bound, capture, and close D13's gate' (#29) from core/kernel-confinement into main |
| `38a7a9083c57eccc73c7a1f85e81b9d88552fceb` | `d57ff66e2086a712c6cf1e7e00fbe723adaeca7f` | Document the confined shell tool in README |
| `6bf982dad715b2f7f883b59eb1a131e97eb9cb75` | `90c501a76d58817d17627bb1dd8c53eaffe9eeaa` | Close D7's gate: openat(O_NOFOLLOW) component-walk, and publication |
| `4a005070ec6e7e82da2684f5a4752ecb1c6cdb45` | `08f9526c187220b383ac080b44631f535c202553` | Merge pull request 'Document the confined shell tool in README' (#30) from docs/readme-shell-usage into main |
| `693d65ba3c7a8d4f1e22e13ae90b13cb1287166c` | `9bbd140b92966e00d1f7a80f4a4d2ae9f8ce3c23` | Merge pull request 'Close D7's gate: openat(O_NOFOLLOW) component-walk, and publication' (#31) from security/d7-openat-walk into main |
| `c78e9f848dbbc696ad1e98512c6133e784165122` | `5b6ced695dd974c6b08d6719d3cf92e7b6cef999` | Close undo.cpp's D7 gap, and simplify the surrounding comments |
| `bb1f010498b4e9bd2a095f5f737743f3b9049e5b` | `b456f6b0c733b5bc6a3a4f47e03f96bca33c8b3c` | Merge pull request 'Close undo.cpp's D7 gap, and simplify the surrounding comments' (#32) from security/d7-gate-followup-and-humanize into main |
| `292e8104e50a68e3932b9ba74879804aa809b9f2` | `f70ba83c786504f3eee2f666f70a101f1cc0b9d7` | Add a performance/size review, gated behind the maintainer's call |
| `6a5ecd4c9528f5757332769391f3b92e754d9250` | `3cace1068dbdcdf9346517bcfb8322aa438d74a3` | Turn on LTO as an opt-in HERMIT_LTO build option |
| `a831df56862577da74cc12f73fef0531c1192e1a` | `3f2f25002096369002debf1a771039ad7a7d4ce1` | Correct the LTO block's comment and give it its own build directory |
| `747dfb7f7ae3c2f2a0ffec3950fac02320c7c1a3` | `ff8aa9cc15ac305ea90a5750a59ace20f8db9533` | Close ROUTING step 8: the fsops re-run, and what it corrected |
| `532f138d0935883870dca9e9bb6d84a49f78b76c` | `7ba8bd80897994c17e6414ecd41cd8e05671a46e` | Merge pull request 'Close ROUTING step 8: the fsops re-run, plus two LTO review follow-ups' (#33) from bench/fsops-sweep3-and-lto-followups into main |
| `bb92265cc767a7c78ba2e1ca8e9f32a67e0b537b` | `40f25b00f7e547f37598fcdc787482d18d87b49e` | Add the logo and its derived asset set |
| `9c416e72b161996f0992a1f1bed8dd09b85095ce` | `06c00f4c98a0290e472537e61b4b5d593077cbfc` | Keep the generated-image sources out of history |
| `ade1e80d7a604b522b793038ee8290d1eb214a13` | `f4870b50af8aa1a2f8c4244e9024e3311b270dcb` | Move the logo to vector and fix the four known defects |
| `2b6fa123d7e833d65e797b934929a92918fbb54c` | `e450962222035856197241b8cefcf24b7e28397a` | Merge pull request 'Add the logo and its derived asset set' (#34) from brand/logo-and-assets into main |
| `501bd0c59af9c1f0377711f24faca084569cbf7e` | `7c0157e8b151887737db71881d30d39c513c61b0` | Add a user-facing book under docs/, alongside the design documents |
| `861de7a375b7e72e9f44cdd2eae5ebe65a534368` | `f0d919d5632e79014f8590b590ed99062f5c9026` | Merge pull request 'Add a user-facing book under docs/' (#35) from docs/user-book into main |
| `316928dc488d81381c594745c722970653164510` | `84e0df2b6675e920ccffc1a5ac972d0277a2f663` | Build mcp.cpp: the MCP-over-stdio frontend, closing ROUTING.md step 6 |
| `f544b77aaefa65e9e45f6639870ef0c05b366db5` | `f7ed6b73553ed63df87e4c5a90df72cd0726bb42` | Merge pull request 'Build mcp.cpp: the MCP-over-stdio frontend, closing ROUTING.md step 6' (#36) from core/mcp-frontend into main |
| `dbfd7253d294e91b59607c39d4a770de28033e2c` | `aee1820af6ffd16d898e4eb0a394e8f82c142020` | Add a publication docket for going public on GitHub |
| `4351de1de45a0f61c48b5dbaeeecc2c3da555ebc` | `d2d6936ee52d46279a17efc294e595943afd4234` | Merge pull request 'Add a publication docket for going public on GitHub' (#37) from docs/publication-todo into main |
| `0658e0ebc5aa48faaec7d86131d847794dcb2e67` | `1d40759476d49e0d288bb4f7ab094cdf7123cd8e` | Sync docs to mcp.cpp having shipped 2026-08-28 |
| `76fc0c000284654d38833049496159fcad35bac5` | `f66b11905e66f9dce43f78d57c4b6a02c8159ea4` | Merge pull request 'Sync docs to mcp.cpp having shipped 2026-08-28' (#38) from docs/mcp-sync-commit into main |
| `506d0a3cfd1a95c112bd787956a77f1f17ab12cc` | `399b4928d8986dcc71d5fb7647abf2c8588f1bd2` | Open the compaction question, and rule out summarizing to answer it |
| `5d95a512938f340e7f8607273c03591f0b79e5e0` | `006c7e19d479eaaca7066d177e9f05f9817d89c5` | Merge pull request 'Open the compaction question, and rule out summarizing to answer it' (#39) from docs/compaction-open-question into main |
| `848434a937ea87dfad1e470410ebcf59dc11d8f3` | `37a08b81bbb1e5a942a91a987909f244752498ca` | Rebuild the context window from the tree instead of trimming it |
| `e7ce558788d7d2ed64e9badb057b8121215fafa6` | `54923011dd0ad91c901ad3a577cd05bb10755d26` | Merge pull request 'Rebuild the context window from the tree instead of trimming it' (#40) from core/compaction into main |
| `b1c78019d4d4e6379f43251340681d981a2d740a` | `15c15d5bd55d03fcb60c3b9010d782bfa13f2c7a` | Make the call trace agree with the footer about refusals |
| `65047788d90b82f762662cf1b710027006c25524` | `51f072a5c1563678cbc25765004673d7063acb97` | Build the read record, measure it, and leave it off |
| `931af886783c94d6cd5814066351723ca242a3d3` | `e7239f3772ce81f84ef777494ed7320791c747b7` | Stop a filename forging lines in the note and the R6 trace |
| `7c923ad508a48d23fad20debd1de6288ffd0e982` | `be6e491a91924ac7a2071b499368d593198a1193` | Make a rebuild clear the trim's target, not merely shrink the prompt |
| `1362d4f303c7927c6903415eff5d9143318f7801` | `d674d4b419c7fc7ee21c794b267006431c7d9da4` | Report the calls the truncation path answers |
| `d390e5beb1be51d7c300dd461fd136c126af33b8` | `45844b082e6d56041b66ad5f30dc73fb18d1033a` | Say what the record actually holds |
| `f0d46ca031a26a3cba691c2e5a7a38e66e421af7` | `dda2d385d1983aa22f34ce7b0597412e3c615479` | Tighten the troubleshooting wording |
| `a57154f4ec47fd1bbe7fd93c3d49a0ab04d9ae20` | `b18b7663ec7bf074cb828e3d0533ec2ef1ddace7` | Tighten D17 |
| `0848e593faf878f89a76466c16c3052b84cf6fe7` | `bdce2e6504851c06b1e6f2f494211b42ca59b07d` | Merge pull request 'Stop a filename forging lines in the note and the R6 trace' (#43) from fix/note-injection into main |
| `2f6e46e94f1a33f8f602e8b0b5ff46f8c0665b30` | `8cc3339170fbd186665355aad87955295c98948b` | Scrub the record's paths too, and name its tests for what they hold |
| `1d34be6641e83caa9f1b06c7ef810137f6fdafb4` | `95512032e944abb084220bc7ca1fa5dfd8003595` | Merge pull request 'Land the compaction guard fix, which #44 did not reach main with' (#45) from fix/compaction-guard-rebased into main |
| `8ee1444ac5e63f964c8dae8fbd0229c85db572e2` | `301a706a33f1bbe52b10f10a01ca0836758506c4` | Record what the guard change did to the read record |
| `e1542cee30b0ef90866a34931fd91c2d5748bfca` | `ba4aae745b4113851c027476906b5f53993f8210` | Merge pull request 'Build the read record, measure it, and leave it off' (#42) from core/read-record into main |
| `e82d9f056e54480308afc2a5147339215603e8c5` | `ec0a02e53a78870938cb133ba39b81cc218f3345` | Merge pull request 'Make the call trace agree with the footer about refusals' (#41) from fix/callevent-refused into main |
| `3ca0abcc0574f2ef42b7b7749285008e286d6e9c` | `e164d4f42f9ddae04efc3c15ef73ea414e6f7027` | Walk find's directory tree with an explicit stack, not recursion |
| `a4315a09168430673d50ef387398b84fce543ddb` | `b446ac34e9cb4e68d893988e031011021331e1bf` | Drop setuid/setgid/sticky on edit's replace path, matching write's |
| `35b6e54e2217c431b2a4f1a0a9a4e219b95668dd` | `6ed4c17fd85e73c2c38b64f3430ac5562213044c` | Merge pull request 'Fix a stack-overflow crash in find, and a permission-bit mismatch in edit' (#46) from fix/find-stack-overflow into main |
| `90dc79a448bc8eaa74cb7b8732c70bec6cffbd03` | `c68dd86cc8ef63aab13345bf10a502a8a96134f6` | Stop calling --read-record's cost a frequency, when it can be a threshold |
| `204b83a632f9b8a9df421015a174480431cecde5` | `e58a19dc18612f13368c53a285559688744c962d` | Add D17 to the glossary's decision table |
| `06dba553243cd8124efb27d712932e30e1dbc601` | `30f9a47b31cce6f8a2c558e69915e9b71976b5dc` | Catch FLOW.md up: the judge/retry ring and MCP both shipped |
| `f24eb69b36a159fe26dad11f535d64e87e6346bb` | `c96fb8f3005b85b7d81ab13ccb93b7651f77cebe` | Stop claiming context management needs nothing beyond a budget check |
| `c2037f08a2d4fba3e9bc74c56b495055dba07806` | `48d681799de8de7e2ea645ac165fa5bfd0acce43` | Fix ROADMAP's cross-reference to a "still open" question D17 settled |
| `cd54321250624b56e816904220595b19c1ca2ede` | `178c7789987ec8744067c36c5e5f84d31757a071` | List the four supervisor modules missing from README's layout table |
| `ba1bc3d3e56e7c07a95baac5828ffd70616218d1` | `11bff56af060ab65bc1945007ef462f380121fc7` | Update the FAQ's stale test count |
| `93051112fbce59eb9386ba138831a02f679adaa1` | `c404f6df07e07517b256fb0d47d7f44db68d2efe` | Clear a resolved TODO item, and recount the AI-attribution audit |
| `0f6fa7791cec403b9151b472d5c8bbd41df9b99f` | `a641c650c181aeda0f10d0b28ec964ed742a091b` | Bring the book's "last brought current" banner forward to today |
| `5f42a0a143d3e83b9295ddd2f526e83098023241` | `30f210cab4a5a348dc10e8eeb42d9636afb31004` | Merge pull request 'Catch the docs up: five weeks of feature work had left nine files stale' (#47) from docs/catch-up-sweep into main |
| `adf87e4da110a615094b71db0299eb896210cf0c` | `a72cea4d7a077ce681cdefb7e4f2b194992d3292` | Admit Ollama Cloud under D18, gated behind --allow-cloud |
| `ddb65d92fea37942040ba2bd9211898f03ee0994` | `1c00e42d45ec44cae5ea32cf42146d6f8320e4a7` | Merge pull request 'Admit Ollama Cloud under D18, gated behind --allow-cloud' (#48) from decisions/d18-ollama-cloud into main |
| `f23568faa0bd770485af7b49d569b13dc2f0a4e7` | `e8d0bcf2d81fe2b132f22fe19205d422a42435b9` | Hand the shell test fixture the launch-time fd baseline, so its tests run under ctest |
| `5a9422979dd4a87a7f52db50230c5f8097a99115` | `cb14ee4ddc2d716c19299a66f2b98b9adfdfb933` | Clear the publication docket: scrub, dead links, README lede, cross-references |
| `73dd7ead38bbef4dc2088dbbe117e092e5eb7457` | `2fe8948a475f553900f5df62ea7c41145e6f5034` | Admit `delete` under D19: opt-in, gated on observation, backed up before the name goes |
| `1c19cf3beb5ec900059cbc02de33fadde92711d3` | `aa6686a146a776e7aba98af284b099c81a250bfb` | Say what delete does through a link, mark it in `hermit config`, and tighten its refusals |

Commits made after 2026-09-04 have no pre-rewrite id and are not listed.
