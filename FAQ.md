# Questions this design gets asked

Plain-speech answers to the questions an evaluator asks first, written down because they
have been argued once and should not be re-derived. **The binding reasoning lives in the
linked sections** — [ROUTING.md](./ROUTING.md), [SCOPE.md](./SCOPE.md),
[REQUIREMENTS.md](./REQUIREMENTS.md), [DECISIONS.md](./DECISIONS.md). If this file and a
linked document ever disagree, the linked document is right and this one is stale.

Each answer includes what it concedes. An argument that concedes nothing is not an
argument, it is a pitch.

---

## "Why not just let the model run shell or PowerShell commands?"

**We do keep shell**, and removing it was considered and rejected on measured grounds: tool
preference varies by model and is not under our control, and neither destructive incident in
the evidence base came through shell — both came through file tools, and so did every escape
into the repository root. The intuition that shell is the dangerous half is backwards, and
that is measured, not reasoned. Full argument, with the numbers:
[SCOPE.md](./SCOPE.md) § "Why terminal survives."

What shell alone cannot give you:

- **Containment by construction.** Every other tool takes a path type only the sandbox can
  build, so it physically cannot name a file outside the root. A shell command is an opaque
  string — there is no path argument to resolve, which is exactly why shell needs kernel
  confinement as a separate mechanism ([D10](./DECISIONS.md), [ROUTING.md](./ROUTING.md) §4).
- **Verification.** `cp a b` returns an exit code. `move` hashes the source, renames, hashes
  the destination, and compares (R3). Getting that from shell means writing a wrapper that
  hashes both ends — which is what the tool is.
- **Freedom from ambient state.** The original sin in our evidence: upstream's terminal tool
  followed the process working directory and ignored the sandbox flag, so exactly-correct
  files landed in wrong directories and got scored as model failure (R1).
- **Undecorated results.** Shell output is human-formatted text that must be parsed. A model
  once copied a harness's own `N|` line-number prefix into a file as content
  ([ROUTING.md](./ROUTING.md) §5).
- **Resistance to composition.** The sweeps show these models failing at multi-step
  composition rather than at individual operations. One `find | xargs sed -i` is fifteen
  operations with no verification between them and no undo.
- **Stability across machines.** "Have the model use CLI commands" requires knowing *which*
  shell, on which OS, with which path semantics — see [SCOPE.md](./SCOPE.md) § Platforms for
  how far Windows path handling diverges.

**Conceded:** shell is genuinely more flexible than any fixed tool surface, which is why it
is kept rather than removed — gated behind kernel confinement, and off the programmatic
surface by default ([ROUTING.md](./ROUTING.md) §8).

---

## "Why not write it in Python, like most MCP servers?"

**For most MCP servers, Python is the right answer** — mature SDK, faster to write, more
people can maintain it. Three things make this case different.

- **Deployment.** One static binary versus a Python environment on every developer machine:
  no virtualenv, no dependency resolution, no conflict with the system interpreter.
- **Startup compounds here specifically.** The architecture is *many bounded sessions*, not
  one long-running process, because that is what the tournaments concluded works. An
  interpreter pays one to three seconds per launch; a static binary pays about ten
  milliseconds. Multiply by every session.
- **The guarantee is structural rather than conventional.** The sandbox path type has a
  private constructor — only the resolver can create one — so a tool that tried to name a
  file outside the root *fails to compile*. The tier boundary works the same way: the core
  library does not link the HTTP or JSON libraries, so a filesystem tool that tried to reach
  a model would fail to link ([ROUTING.md](./ROUTING.md) §7). In a dynamic language that
  becomes a convention — "always resolve first" — and [D6](./DECISIONS.md)'s whole argument
  is that a check which must be *remembered* gets forgotten around tool #37.

**Not claimed: speed.** The process is blocked on the model and always will be
([README](./README.md), "Why native code, honestly").

**Conceded, and it is the stronger version of this question: why not Rust?** It would serve
the structural-guarantee argument as well or better, and add memory safety this language
does not. The compensating discipline here is explicit — every build runs under
AddressSanitizer and UndefinedBehaviorSanitizer, the suite is currently 771 tests at zero
warnings, and the adversarial review on 2026-08-16 caught a real use-after-free before it
shipped. That is a mitigation, not a refutation.

---

## "Why not just wait for models to get good enough that this is unnecessary?"

Two halves of the value, and they age differently.

**The supervision half does shrink** as models improve. If a model never claims completion
on an untouched tree, never writes the right file to the wrong directory, and never needs a
retry, the supervisor earns less.

**The containment and verification half does not.** They are safety properties, not
crutches — which is why the delegation case works with frontier cloud models too, and why
the economics experiment ([bench/delta](./bench/delta/DESIGN.md)) needs no local model at
all. No model of any size should spend judgment on bookkeeping, and no model of any size
should hold more filesystem authority than the task requires.

**Conceded:** if models become perfectly reliable at bookkeeping *and* you are willing to
grant them ambient authority over the machine, the case for this narrows to the audit trail.
That is a real condition, and it is written here rather than hidden.

---

## "Isn't this just another MCP server?"

The ecosystem default is a server that *adds* capability while inheriting the launching
user's authority. Kiro's own documentation states it plainly: stdio MCP servers "execute
arbitrary commands inside your environment with the same privileges and access as the agent
itself, including access to your source code, environment variables, secrets, and any
credentials available in the session."

This is the MCP server that **reduces** authority instead of inheriting it: containment by
construction, kernel confinement for the one tool that cannot be contained by type, and
verification on every mutation. The argument is stronger for a programmatic caller than for
a human one, because a human notices when a tool wanders and a model does not
([ROUTING.md](./ROUTING.md) §8).

**Conceded:** that pitch is only worth anything if the guarantees hold, which is why they are
tested rather than asserted, and why the measurement suite is designed to be able to
disprove the project's own thesis ([bench/delta/DESIGN.md](./bench/delta/DESIGN.md)).
