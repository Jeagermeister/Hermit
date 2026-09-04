# The Hermit book

A user's guide to Hermit: what it is, how to run it, which model to put behind it, and how to
read the numbers it stands on. Written at reading depth — every chapter explains its subject in
its own words, and links down to the design documents where the binding reasoning lives. If a
chapter here and a design document ever disagree, the design document is right and the chapter
is stale; say so in an issue rather than trusting either silently.

Status statements in these chapters are dated. The book was last brought current **2026-09-04**.

## Where to start

- **"I have a GPU and twenty minutes"** → [Building](./10-building.md), then
  [Quickstart](./11-quickstart.md).
- **"Which model should I run?"** → [Choosing a model](./12-choosing-a-model.md).
- **"What is this thing, and why would I trust it?"** → [What Hermit is](./01-what-hermit-is.md),
  then [The evidence](./02-the-evidence.md).
- **"I want to call it from my IDE"** → [MCP and Kiro](./20-mcp-and-kiro.md).
- **"Are the numbers real?"** → [The measurements](./30-benchmarks.md).

## Contents

### Part I — Understanding Hermit

| chapter | what it answers |
|---|---|
| [1. What Hermit is](./01-what-hermit-is.md) | A supervisor, not a chatbot — and what that means in practice |
| [2. The evidence](./02-the-evidence.md) | The recorded failures every guarantee traces to |
| [3. A tour of the architecture](./03-architecture.md) | Tiers, the ten tools, what "verified" means mechanically |

### Part II — Using Hermit

| chapter | what it answers |
|---|---|
| [10. Building](./10-building.md) | Compiler, CMake, tests, the sanitizer and LTO builds |
| [11. Quickstart](./11-quickstart.md) | From a fresh build to a verified first run |
| [12. Choosing a model](./12-choosing-a-model.md) | The four gates, the measured field, sizes and speed |
| [13. CLI reference](./13-cli-reference.md) | Every subcommand, flag, environment variable and exit code |
| [14. Expectations](./14-expectations.md) | Stating post-conditions, and what the verdict means |
| [15. Shell and Landlock](./15-shell-and-landlock.md) | The opaque tool: what confinement buys and what it does not |
| [16. Configuration](./16-configuration.md) | Four sources, their precedence, and the config file |
| [17. Undo and backups](./17-undo-and-backups.md) | The store, restore, retention — and what undo cannot do |
| [18. Troubleshooting](./18-troubleshooting.md) | Symptoms, causes, and fixes |

### Part III — Calling Hermit

| chapter | what it answers |
|---|---|
| [20. MCP and Kiro](./20-mcp-and-kiro.md) | The programmatic front door — built |

### Part IV — The measurements

| chapter | what it answers |
|---|---|
| [30. The measurements](./30-benchmarks.md) | What fsops and the delta experiments each ask, and how to reproduce them |

### Appendix

| chapter | what it answers |
|---|---|
| [90. Glossary](./90-glossary.md) | R1–R9, D1–D19, and the project's working vocabulary |

## How this book relates to the design documents

The repository's root documents — [REQUIREMENTS.md](../REQUIREMENTS.md),
[SCOPE.md](../SCOPE.md), [ROUTING.md](../ROUTING.md), [DECISIONS.md](../DECISIONS.md),
[FLOW.md](../FLOW.md), [FAQ.md](../FAQ.md), [ROADMAP.md](../ROADMAP.md) — are written for
someone evaluating the design: they say why each choice was made, what evidence forced it, and
what would overturn it. This book is written for someone *using* the result. It restates
nothing as binding; where a chapter summarises a decision, the D-number or section link beside
it is the authority.
