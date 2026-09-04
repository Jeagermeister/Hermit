// Manual harness for the pieces that exist so far.
//
//   hermit resolve   --root DIR <path>...   R1 sandbox resolution
//   hermit preflight --model NAME           R9 model preflight
//   hermit session   --model NAME           context accounting, against a real model
//   hermit config                           the resolved settings, and their origins
//
// Not the product's CLI -- that arrives with the agent loop. This is how each piece is
// looked at by hand against the real filesystem and the real daemon, which is where
// the last two bugs came from.
//
// Argument parsing moved out to hermit::app::Config on 2026-08-13. It is not shaped
// like a CLI parser because it is not one: the same settings have to arrive from a
// file and the environment when the MCP-over-stdio frontend lands (D7), and a second
// implementation of "where does the sandbox root come from" is exactly the divergence
// R1 is about.
//
// `config` is a real subcommand rather than a debugging aid. Four sources feed these
// settings and two of them are safety limits, so "what is actually in force, and which
// source set it?" needs an answer that does not involve reading the code.

#include <hermit/app/config.h>
#include <hermit/app/expect.h>
#include <hermit/app/mcp.h>
#include <hermit/app/toolset.h>
#include <hermit/app/usage.h>
#include <hermit/core/confine.h>
#include <hermit/core/sandbox.h>
#include <hermit/ollama/preflight.h>
#include <hermit/supervisor/loop.h>
#include <hermit/supervisor/reinvoke.h>
#include <hermit/supervisor/session.h>
#include <hermit/supervisor/undo.h>
#include <hermit/supervisor/verify.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <iostream>
#include <map>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

// `to` is stdout when help was asked for and stderr when it is a complaint: help the
// operator requested is output, not a diagnostic, and piping it should work.
int usage(std::ostream& to = std::cerr) {
  to <<
      "usage: hermit resolve   --root DIR <path>...\n"
      "       hermit preflight --model NAME\n"
      "       hermit session   --model NAME   (try --max-num-ctx 2048 to force a trim)\n"
      "       hermit agent     --root DIR --model NAME <instruction>\n"
      "       hermit undo      --root DIR    (lists what can be restored; --restore/--last/--prune act)\n"
      "       hermit usage     --root DIR    (D18: estimated Cloud spend logged for this root)\n"
      "       hermit mcp       --root DIR    (D7: an MCP server over stdio; no positional args)\n"
      "       hermit config\n"
      "\n"
      "settings, in increasing precedence: defaults < --config file < environment < flags\n"
      "  --config PATH          a JSON config file. Never searched for implicitly.\n"
      "  --root DIR             sandbox root (R1). No default, ever.\n"
      "  --model NAME           the Ollama tag to drive\n"
      "  --allow-cloud / --no-allow-cloud   permit a Cloud-tagged model (D18); off by\n"
      "                             default, and required before one is accepted\n"
      "  --url URL              Ollama base URL; loopback only (D7)\n"
      "  --max-num-ctx N        hard ceiling on any num_ctx sent (D8 safety clamp)\n"
      "  --min-context N        R9 architectural context floor\n"
      "  --connect-timeout N    seconds\n"
      "  --metadata-timeout N   seconds\n"
      "  --chat-timeout N       seconds\n"
      "  --warmup / --no-warmup     R9 inference check; off by default\n"
      "  --tools   / --no-tools     R9 tools-capability gate; on by default\n"
      "  --shell / --no-shell       register the shell tool, kernel-confined (D10); off by\n"
      "                             default, and refused at startup unless this machine's\n"
      "                             confinement probe reports Enforced\n"
      "  --shell-timeout N          seconds; R8 per-call wall-clock bound for shell\n"
      "                             (default 60)\n"
      "  --delete / --no-delete     register the delete tool (D19): one file the model has\n"
      "                             already read or listed, backed up before the name goes;\n"
      "                             off by default\n"
      "\n"
      "agent only:\n"
      "  --max-turns N          turn bound for one run (default 12)\n"
      "  --budget N             wall-clock seconds for one run, checked between turns (R8)\n"
      "  --attempts N           R7: total attempts at the stated post-conditions, each in\n"
      "                         a fresh session re-invoked with the one concrete remaining\n"
      "                         failure (default 3; 1 disables re-invocation; without\n"
      "                         --expect there is nothing to retry and one attempt runs)\n"
      "  --no-verify            skip the per-turn hash diff of the tree (R6); verification\n"
      "                         is on by default\n"
      "  --compact-at PCT       rebuild the window from the tree at this percentage of the\n"
      "                         prompt budget (default 80; 0 falls back to trimming, which\n"
      "                         drops history without telling the model)\n"
      "  --read-record          list the paths already named in a call in a rebuilt window.\n"
      "                         Off by default: it did not stop a model re-reading them in\n"
      "                         the one paired run so far, and on a tight window it can\n"
      "                         stop rebuilds firing at all\n"
      "  --judge-model NAME     who decides satisfies: expectations (default: the working\n"
      "                         model, in a fresh session that never sees the transcript)\n"
      "\n"
      "agent and undo:\n"
      "  --backups DIR          where undo data goes; must be outside --root (R4).\n"
      "                         Defaults to .hermit-backups-<root name> beside the root\n"
      "  --keep-hours N         retention: generations older than N hours are pruned at\n"
      "                         agent start and by undo --prune (default 72 -- supervised\n"
      "                         trees live under git; the store covers the gap between a\n"
      "                         bad mutation and the operator noticing, not the archive)\n"
      "\n"
      "undo only (no flag lists the generations; each action is explicit):\n"
      "  --restore N            write generation N's preserved bytes back to the file they\n"
      "                         came from; the file's current bytes are preserved first,\n"
      "                         so a restore is itself undoable\n"
      "  --last                 restore the newest generation\n"
      "  --prune                apply retention now, without running a job\n"
      "\n"
      "environment: HERMIT_CONFIG (absolute), HERMIT_SANDBOX_ROOT (absolute), HERMIT_MODEL,\n"
      "             HERMIT_OLLAMA_URL, HERMIT_MAX_NUM_CTX\n";
  return 2;
}

/// A whole non-negative number, or nothing.
///
/// `std::strtoull` is the wrong tool: it *negates and wraps*, so `-1` parses cleanly as
/// UINT64_MAX, and it reports overflow only through `errno`, which is easy to forget to
/// read. `--max-turns -1` would silently become a bound of 2^64-1 -- no bound at all --
/// and `--budget -1` would wrap back to a negative `long`, making the loop's first
/// wall-clock check fire immediately and report a spent budget after zero seconds. Both
/// turn a bound into its opposite, which is the failure direction this project keeps
/// refusing.
///
/// `from_chars` has none of those properties: the unsigned grammar has no sign, so `-1` is
/// `invalid_argument`; overflow is `result_out_of_range`; and requiring `ptr == end` is
/// what rejects `5abc`, which `strtoull` would have read as 5.
/// A whole number in [1, max], or [0, max] when `allow_zero`.
///
/// Zero is refused by default because every bound this parses -- turns, seconds, attempts,
/// retention -- reads as "no bound at all" or "never run" at zero, and neither is a request
/// anyone makes on purpose. `--compact-at` is the exception: zero there names the trim-only
/// arm, which is a policy rather than the absence of one.
std::optional<std::uint64_t> whole_number(std::string_view text, std::uint64_t max,
                                          bool allow_zero = false) {
  std::uint64_t value = 0;
  const auto* const end = text.data() + text.size();
  const auto [stopped, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || stopped != end) return std::nullopt;
  if (value == 0 && !allow_zero) return std::nullopt;
  if (value > max) return std::nullopt;
  return value;
}

int report(const hermit::app::ConfigProblem& problem) {
  std::cerr << "error: " << problem.message() << '\n';
  return 2;
}

// R1 --- resolution, run from wherever you invoke it: resolution must not move when
// the working directory does, and this is how you see that by hand.
int resolve_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> paths;
  // No model and no network: resolution is pure filesystem work, so an Ollama URL
  // exported for something else must not be able to break it.
  const auto config =
      hermit::app::load(args, {.sandbox_root = true, .model = false, .ollama = false}, paths);
  if (!config) return report(config.error());
  if (paths.empty()) {
    std::cerr << "error: resolve needs at least one path to resolve\n";
    return usage();
  }

  auto box = hermit::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermit::to_string(box.error()) << ": " << config->sandbox_root << '\n';
    return 1;
  }

  // error_code overload: the throwing one aborts if the working directory has been
  // deleted, which would crash on the very line claiming the cwd does not matter.
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  std::cout << "root : " << box->root() << '\n'
            << "cwd  : " << (ec ? "<unavailable>" : cwd.string())
            << "  (never consulted)\n\n";

  int failures = 0;
  for (const std::string_view raw : paths) {
    if (auto p = box->resolve(raw)) {
      std::cout << "  OK      " << raw << "\n"
                << "          -> " << p->path() << "  [" << p->relative().string() << "]\n";
    } else {
      ++failures;
      std::cout << "  REJECT  " << raw << "\n"
                << "          -> " << hermit::to_string(p.error()) << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

// R9 --- the gates a model must pass before it is worth sending a first request.
int preflight_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> extra;
  const auto config =
      hermit::app::load(args, {.sandbox_root = false, .model = true, .ollama = true}, extra);
  if (!config) {
    // `preflight qwen35-agent` -- the old positional shape -- otherwise reports "no
    // model", which is true and unhelpful when the model name is right there.
    if (config.error().kind == hermit::app::ConfigError::MissingRequired && !extra.empty()) {
      std::cerr << "error: preflight takes the model as a flag; did you mean --model "
                << extra.front() << "?\n";
      return 2;
    }
    return report(config.error());
  }
  if (!extra.empty()) {
    std::cerr << "error: preflight takes no positional arguments, got: " << extra.front() << '\n';
    return 2;
  }

  const auto client = hermit::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  const auto report_out = hermit::ollama::preflight(*client, config->model, config->preflight);

  // To stderr: the report is a startup diagnostic, and a caller redirecting stdout is
  // collecting the model's output, not this.
  std::cerr << report_out.render();
  return report_out.ok ? 0 : 1;
}

// The context accounting, driven against a real model.
//
// Here because the estimate is the one thing in Session that cannot be settled by a
// unit test: there is no tokenizer in this process, so "is the guess conservative
// enough?" is a question only the daemon can answer. Each turn prints what the session
// expected against what Ollama actually evaluated, so a bad constant shows up as a
// column that does not line up rather than as a silent discard three turns later.
//
// `--max-num-ctx 2048` is the interesting way to run it: it forces the session into
// compaction and shows it dropping turns deliberately, which is the behaviour the whole
// class exists to have instead of the server's.
int session_command(std::span<const std::string_view> args) {
  // Ollama's `num_predict` is an int on the wire. Nothing else here bounds it, and the
  // value derived below comes from `max_num_ctx`, which config deliberately leaves
  // unbounded above so a bigger card can be used (D8).
  constexpr int kMaxGenerationTokens = 1 << 20;

  std::vector<std::string_view> extra;
  const auto config =
      hermit::app::load(args, {.sandbox_root = false, .model = true, .ollama = true}, extra);
  if (!config) return report(config.error());
  if (!extra.empty()) {
    std::cerr << "error: session takes no positional arguments, got: " << extra.front() << '\n';
    return 2;
  }

  const auto client = hermit::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  // The architecture is the one ceiling no request can raise (R9), so the session is
  // told about it rather than left to discover it from a collapsed prompt. A model whose
  // card cannot be read is not a reason to refuse -- the clamp still bounds the window.
  hermit::supervisor::SessionOptions options;
  options.model = config->model;
  options.num_ctx = config->client.max_num_ctx;
  if (const auto card = client->show(config->model)) {
    options.architecture_context = card->architecture_context;
  }

  // A quarter of the window, so a small `--max-num-ctx` stays usable rather than failing
  // at open() for a reserve larger than the window it is reserving from. Bounded before
  // the cast: `num_predict` is an int, `max_num_ctx` has no upper limit in config, and a
  // quarter of a large enough one wraps negative -- which Ollama reads as "generate
  // without limit", turning the reserve into its own opposite.
  const std::uint64_t window = std::min(options.num_ctx,
                                        options.architecture_context.value_or(options.num_ctx));
  options.reply_reserve =
      std::clamp<std::uint64_t>(window / 4, 1, static_cast<std::uint64_t>(kMaxGenerationTokens));
  options.max_tokens = static_cast<int>(options.reply_reserve);

  auto session = hermit::supervisor::Session::open(
      options, *client,
      "You are a terse test fixture. Answer in one short sentence and nothing more.");
  if (!session) {
    std::cerr << "error: " << session.error().message() << '\n';
    return 1;
  }

  // Grows on purpose: the later turns carry enough filler to push a small window into
  // compaction, which is the case worth watching.
  const std::string filler(1500, 'x');
  const std::vector<std::string> script = {
      "Name a primary colour.",
      "Name a second one.",
      "Here is some material to ignore: " + filler + "\nName a third one.",
      "Here is more material to ignore: " + filler + "\nWhich colours have you named?",
      "Here is still more: " + filler + "\nName a fourth.",
  };

  std::cout << "model  : " << config->model << '\n';
  if (hermit::ollama::is_cloud_tag(config->model)) {
    std::cout << "cloud  : yes -- routed via the local daemon to Ollama Cloud (D18)\n";
  }
  std::cout << "window : " << session->window() << " tokens (least of the request, the D8 clamp"
            << (options.architecture_context ? ", and the model's architecture)" : ")") << '\n'
            << "budget : " << session->prompt_budget() << " tokens for the prompt, "
            << options.reply_reserve << " reserved for the reply\n\n"
            << "  turn  msgs  estimated  measured  ratio  dropped  reply\n";

  int turn = 0;
  for (const std::string& prompt : script) {
    ++turn;
    session->add_user(prompt);

    const auto request = session->prepare();
    if (!request) {
      std::cerr << "error: turn " << turn << " refused: " << request.error().message() << '\n';
      return 1;
    }
    const std::uint64_t estimated = session->estimated_prompt_tokens();
    const std::size_t messages = request->messages.size();

    const auto reply = client->chat(*request);
    if (!reply) {
      std::cerr << "error: " << reply.error().message() << '\n';
      return 1;
    }

    const auto recorded = session->record(*reply);

    std::string answer = reply->content;
    if (answer.size() > 34) {
      // Back off to a character boundary before cutting. A model naming a colour is
      // unlikely to need this; a model quoting a filename is exactly what R1's evidence
      // is about, and half a UTF-8 sequence in the table is a worse bug to chase than a
      // slightly shorter line.
      std::size_t cut = 31;
      while (cut > 0 && (static_cast<unsigned char>(answer[cut]) & 0xC0) == 0x80) --cut;
      answer = answer.substr(0, cut) + "...";
    }
    for (char& c : answer) {
      if (c == '\n') c = ' ';
    }
    std::cout << "  " << turn << "     " << messages << "     " << estimated << "\t   "
              << reply->prompt_tokens << "\t  " << session->chars_per_token() << "   "
              << session->dropped() << "        " << answer << '\n';

    if (!recorded) {
      // The check that matters. If this fires, the estimate was optimistic and the
      // model has been answering a conversation with holes in it.
      std::cout << "        ⚠ " << recorded.error().message() << '\n';
    }
  }

  std::cout << "\ngenerated " << session->generated_tokens() << " tokens, "
            << session->prompt_tokens_seen() << " prompt tokens seen, across "
            << session->completed_turns() << " turns; " << session->dropped()
            << " turns dropped to stay inside the window\n";
  return 0;
}

// Phase 2 --- the loop, end to end: one instruction, the eight Tier 0 tools, a real
// model. This is the first subcommand where all four layers run at once.
int agent_command(std::span<const std::string_view> args) {
  // Pulled out before `load` sees the arguments, because these are this subcommand's own
  // knobs rather than settings with a config-file and environment precedence chain. A
  // flag config does not know about is an error there, deliberately.
  std::vector<std::string_view> passthrough;
  std::size_t max_turns = 12;
  std::uint64_t budget_seconds = 300;
  std::size_t attempts = 3;
  std::uint64_t keep_hours = 72;
  std::optional<std::filesystem::path> backup_dir;
  std::optional<std::string> judge_model;
  bool verify = true;
  // Percent rather than a fraction, so the flag takes a whole number like every other one
  // here and there is no "0.8 or 80?" ambiguity at the command line. 0 turns compaction
  // off, which is the trim-only control arm D17's measurement needs.
  std::uint64_t compact_at_percent = 80;
  bool carry_read_paths = false;

  // A flag whose value is missing is reported as such. Letting it fall through to
  // `load` would report it as an unknown flag instead, which sends the operator looking
  // for a typo in a flag that was spelled correctly.
  bool malformed = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto takes_value = [&](std::string_view flag, std::string_view& out) {
      if (args[i] != flag) return false;
      if (i + 1 >= args.size()) {
        std::cerr << "error: " << flag << " needs a value\n";
        malformed = true;
        return false;
      }
      out = args[++i];
      return true;
    };
    std::string_view value;
    if (malformed) break;
    if (takes_value("--max-turns", value)) {
      // 10000 turns is far past anything a bounded session means, and keeps the figure
      // printable and the arithmetic obviously safe.
      const auto parsed = whole_number(value, 10000);
      if (!parsed) {
        std::cerr << "error: --max-turns needs a whole number from 1 to 10000, got: " << value
                  << '\n';
        return 2;
      }
      max_turns = *parsed;
      continue;
    }
    if (takes_value("--budget", value)) {
      // A day. Bounded well inside `long` so the chrono::seconds cast below cannot wrap a
      // budget into a negative one, which is how a bound becomes its own opposite.
      const auto parsed = whole_number(value, 86400);
      if (!parsed) {
        std::cerr << "error: --budget needs a whole number of seconds from 1 to 86400, got: "
                  << value << '\n';
        return 2;
      }
      budget_seconds = *parsed;
      continue;
    }
    if (takes_value("--attempts", value)) {
      // Room for far more than the ~67%->~96% arithmetic ever needs; a three-digit
      // attempt count is an operator mistake, not a plan. `whole_number` already refuses
      // zero, so "never run" cannot be asked for.
      const auto parsed = whole_number(value, 100);
      if (!parsed) {
        std::cerr << "error: --attempts needs a whole number from 1 to 100, got: " << value
                  << '\n';
        return 2;
      }
      attempts = *parsed;
      continue;
    }
    if (takes_value("--backups", value)) {
      backup_dir = std::filesystem::path{value};
      continue;
    }
    if (takes_value("--judge-model", value)) {
      judge_model = std::string{value};
      continue;
    }
    if (takes_value("--keep-hours", value)) {
      // A year, and `whole_number` refuses zero -- "keep nothing" is not a retention
      // setting, it is `rm -rf` spelled confusingly, and an accidental 0 would prune
      // the entire history the moment the job starts.
      const auto parsed = whole_number(value, 8760);
      if (!parsed) {
        std::cerr << "error: --keep-hours needs a whole number from 1 to 8760, got: "
                  << value << '\n';
        return 2;
      }
      keep_hours = *parsed;
      continue;
    }
    if (args[i] == "--no-verify") {
      verify = false;
      continue;
    }
    if (takes_value("--compact-at", value)) {
      // 100 is the ceiling because past it the trim has already run: compaction is checked
      // before prepare(), so a threshold above the budget describes a moment that only
      // exists on the way into a trim.
      const auto parsed = whole_number(value, 100, /*allow_zero=*/true);
      if (!parsed) {
        std::cerr << "error: --compact-at needs a percentage from 0 to 100, got: " << value
                  << '\n';
        return 2;
      }
      compact_at_percent = *parsed;
      continue;
    }
    if (args[i] == "--read-record") {
      carry_read_paths = true;
      continue;
    }
    passthrough.push_back(args[i]);
  }
  if (malformed) return 2;

  std::vector<std::string_view> words;
  const auto config = hermit::app::load(
      passthrough, {.sandbox_root = true, .model = true, .ollama = true}, words);
  if (!config) return report(config.error());
  if (words.empty()) {
    std::cerr << "error: agent needs an instruction\n";
    return usage();
  }

  // Several words are joined rather than refused, so an unquoted instruction works the
  // way anyone would expect at a shell.
  std::string instruction;
  for (const std::string_view word : words) {
    if (!instruction.empty()) instruction += ' ';
    instruction += word;
  }

  auto box = hermit::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermit::to_string(box.error()) << ": " << config->sandbox_root
              << '\n';
    return 1;
  }

  // Parsed here and nowhere earlier: a path is only checkable against an open root, and
  // `hermit::app::load` deliberately does no filesystem work. Refused rather than carried
  // -- a mis-spelled path is not a harmless typo here, it becomes a permanent `Unmet` that
  // R7 would hand to the model as a concrete failure to go and fix.
  hermit::app::Expectations expectations;
  {
    const nlohmann::json settings{{"expectations", config->expectations},
                                  {"unjudged", config->unjudged_requirements}};
    auto parsed = hermit::app::parse_expectations_json(settings, *box);
    if (!parsed) {
      std::cerr << "error: " << parsed.error().render() << '\n';
      return 2;
    }
    expectations = std::move(*parsed);
  }

  // Both checks sit here, immediately after the root opens and before a client is built:
  // this is the earliest point a path can be checked at all, and the latest point at
  // which being wrong is still free. Left where the loop options are assembled, a typo
  // would first cost a socket and a preflight round trip against a live daemon.
  if ((!expectations.set.empty() || !expectations.semantic.empty()) && !verify) {
    std::cerr << "error: --no-verify leaves nothing to judge the expectations against.\n"
                 "       Drop --no-verify, or drop the expectations.\n";
    return 2;
  }
  if (judge_model && expectations.semantic.empty()) {
    // Beside the other usage gates, deliberately: this refusal needs nothing built,
    // and a usage error reported only after retention has already pruned would be an
    // exit 2 with side effects. Accepting the flag silently would be worse still -- a
    // flag that looks like it arranged judging while nothing gets judged.
    std::cerr << "error: --judge-model was given, but no satisfies: expectation states "
                 "what it would judge.\n";
    return 2;
  }


  const auto store_dir = hermit::app::resolve_backup_dir(*box, std::move(backup_dir));
  if (!store_dir) {
    std::cerr << "error: --backups must be outside --root (R4): "
              << hermit::app::to_string(store_dir.error()) << '\n';
    return 2;
  }

  // A retention failure is a note, not a stop (D14): the job the operator asked for
  // must not be blocked by archive bookkeeping, and the likely cause (a pre-marker
  // store) comes with its own instructions.
  {
    const auto pruned = hermit::supervisor::prune(
        *store_dir, std::chrono::hours{keep_hours},
        std::filesystem::file_time_type::clock::now());
    if (!pruned) {
      std::cerr << "note: retention skipped: " << pruned.error() << '\n';
    } else if (pruned->generations > 0) {
      std::cout << "retention: pruned " << pruned->generations
                << (pruned->generations == 1 ? " undo generation older than "
                                             : " undo generations older than ")
                << keep_hours << "h from " << store_dir->string() << '\n';
    }
  }

  const auto client = hermit::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  // Registration is gated on a LIVE probe, never on the config flag alone: an operator
  // who turned shell on and got a run that quietly never had it is the exact "believed
  // it was checked when it was not" failure R9 exists to prevent (ROUTING.md section 8:
  // "gate on the probe, never on the platform"). Hard refusal, not a warn-and-continue,
  // for the same reason --judge-model without satisfies: refuses above rather than
  // silently judging nothing.
  std::optional<hermit::app::ShellOptions> shell_options;
  if (config->shell.enabled) {
    auto probe = hermit::probe_confinement();
    if (!probe || *probe != hermit::ConfinementProbeResult::Enforced) {
      std::cerr << "error: shell is enabled in configuration, but kernel confinement could "
                   "not be confirmed enforced on this machine ("
                << (probe ? std::string{hermit::to_string(*probe)} : hermit::to_string(probe.error()))
                << "). Refusing to start rather than expose shell unconfined (DECISIONS.md, D11).\n";
      return 1;
    }
    shell_options = hermit::app::ShellOptions{
        box->root(), std::chrono::duration_cast<std::chrono::milliseconds>(config->shell.timeout)};
  }

  auto tools = hermit::app::ToolSet::tier0(*store_dir, shell_options, hermit::kDefaultMaxReadBytes,
                                           config->delete_tool.enabled);
  if (!tools) {
    std::cerr << "error: composing the tool set failed: "
              << hermit::to_string(tools.error().kind) << '\n';
    return 1;
  }
  if (shell_options && !config->delete_tool.enabled) {
    // D19's named trap: with shell on and delete off, the only removal path the model has
    // is `rm` under the confined shell -- no gate, no backup. A note, not a refusal: the
    // two flags are deliberately independent, and this is the operator's call to make.
    std::cerr << "note: shell is enabled and delete is not; a removal the model decides on "
                 "can only go through `rm` under shell, which is neither gated nor backed "
                 "up. --delete adds a path that is both (DECISIONS.md, D19).\n";
  }

  hermit::supervisor::SessionOptions options;
  options.model = config->model;
  options.num_ctx = config->client.max_num_ctx;
  if (const auto card = client->show(config->model)) {
    options.architecture_context = card->architecture_context;
  }
  const std::uint64_t window =
      std::min(options.num_ctx, options.architecture_context.value_or(options.num_ctx));
  options.reply_reserve = std::clamp<std::uint64_t>(window / 4, 1, 1u << 20);
  options.max_tokens = static_cast<int>(options.reply_reserve);

  // Terse and specific about the two things these models get wrong most: inventing file
  // contents rather than reading them, and announcing completion without doing the work.
  // Not a fix for either -- R6 is why the supervisor exists -- just not an invitation.
  //
  // A factory rather than a session, because R7 runs every attempt in a fresh one --
  // "break larger work into fresh sessions" is the tournament's own recommendation, and
  // the probe behind it is in ROADMAP.md: handed a mid-session refusal, three of four
  // models stopped calling tools and addressed the human instead. The probe session below
  // exists to fail fast and to give the banner real numbers; the driver never sees it.
  const auto make_session = [&options, &client] {
    return hermit::supervisor::Session::open(
        options, *client,
        "You do filesystem work by calling the tools you were given. Read a file before "
        "describing it; never guess its contents. Make one tool call per step, and stop "
        "calling tools only when the work is actually finished.");
  };
  const auto probe = make_session();
  if (!probe) {
    std::cerr << "error: " << probe.error().message() << '\n';
    return 1;
  }

  hermit::supervisor::LoopOptions loop_options;
  loop_options.max_turns = max_turns;
  loop_options.compact_at = static_cast<double>(compact_at_percent) / 100.0;
  loop_options.carry_observed_paths = carry_read_paths;
  loop_options.expected = expectations.set;
  loop_options.unjudged_requirements = expectations.unjudged;
  loop_options.budget = std::chrono::seconds{static_cast<long>(budget_seconds)};

  // D18: summed here rather than read off the Session afterwards, because R7 gives each
  // retry attempt a fresh Session (make_session above) -- no single Session's counters
  // cover a whole job, but every turn from every attempt passes through this observer.
  std::uint64_t job_prompt_tokens = 0;
  std::uint64_t job_completion_tokens = 0;

  // One line per turn, then one indented line per call. The result is truncated for
  // display only -- a `read` of a real file would otherwise bury the trace, and what is
  // wanted here is the shape of the run, not its payload.
  loop_options.observer = [&job_prompt_tokens, &job_completion_tokens](
                              const hermit::supervisor::TurnEvent& event) {
    job_prompt_tokens += event.prompt_tokens;
    job_completion_tokens += event.generated_tokens;
    std::cout << "  turn " << event.turn << "  prompt " << event.prompt_tokens
              << "  generated " << event.generated_tokens;
    if (event.reasoning_chars > 0) std::cout << "  thinking " << event.reasoning_chars << "ch";
    if (event.truncated) std::cout << "  [hit the generation budget]";
    if (event.dropped > 0) std::cout << "  dropped " << event.dropped;
    if (event.compactions > 0) std::cout << "  rebuilt " << event.compactions;
    std::cout << '\n';
    for (const auto& call : event.calls) {
      constexpr std::size_t kPreview = 100;
      std::string preview = call.result.substr(0, kPreview);
      if (call.result.size() > kPreview) preview += "...";
      // One line, always: a newline inside a result would break the alignment and make
      // a long trace unreadable.
      for (char& c : preview) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      }
      std::cout << "        " << (call.refused ? "REFUSED " : "ok      ") << call.tool << "  "
                << preview << '\n';
    }
    if (!event.content.empty() && event.calls.empty()) {
      std::cout << "        answered in " << event.content.size() << " characters\n";
    }
    // R6: what the filesystem shows, printed beside what the model did. Nothing here is
    // derived from the reply.
    for (const auto& change : event.changes.changes) {
      // Scrubbed like the result preview above it, and for a sharper reason: this is the
      // R6 line the operator is told to trust *because* nothing in it comes from the
      // reply. A filename carrying a newline forges a second line under that heading,
      // indistinguishable from a real one. The preview was already scrubbed; the path,
      // which is the more trusted of the two, was not.
      std::cout << "        ~ " << hermit::supervisor::to_string(change.kind) << "  "
                << hermit::supervisor::one_line(change.path) << '\n';
    }
  };

  // Held here so it outlives the loop, which holds it by pointer. force_rehash tracks
  // whether `shell` actually got registered above -- not just whether the flag was set --
  // since `shell_options` only carries a value once the confinement probe also passed;
  // D13's amendment: shell is what makes a MAP_SHARED write to the tree reachable at all,
  // and the incremental hash-reuse optimisation cannot tell that kind of write from no
  // write at all.
  hermit::supervisor::TreeVerifier verifier{*box, shell_options.has_value()};
  if (verify) loop_options.verifier = &verifier;

  hermit::supervisor::ReinvokeOptions reinvoke_options;
  reinvoke_options.attempts = attempts;
  if (!expectations.semantic.empty()) {
    std::uint64_t judge_window = probe->window();
    if (judge_model && *judge_model != config->model) {
      // D18: --judge-model is parsed by this command's own flag loop, outside
      // hermit::app::load(), so validate()'s gate never sees it -- confirmed by a real
      // review pass finding real sandbox file content (semantic.cpp:156-157) reaching
      // judge.chat() (semantic.cpp:171) with a Cloud-tagged judge and no --allow-cloud
      // at all. The working model's own cloud status is covered by validate(); a judge
      // named separately needs the same check repeated here, or it is not covered by
      // anything.
      if (hermit::ollama::is_cloud_tag(*judge_model) && !config->allow_cloud) {
        std::cerr << "error: judge model \"" << *judge_model
                  << "\" is a Cloud-tagged model, which the local daemon proxies to "
                     "Ollama Cloud. D18 permits this only with allow_cloud set "
                     "explicitly: pass --allow-cloud, or put \"allow_cloud\": true in "
                     "a config file.\n";
        return 2;
      }
      // The working model was preflighted through the probe; a judge model named by
      // flag has passed no gate at all, and a typo here would run the whole job and
      // then leave every criterion Undecidable -- reported, but only after the tokens
      // are spent. Refused up front instead, the same reason R9 exists.
      const auto card = client->show(*judge_model);
      if (!card) {
        std::cerr << "error: the judge model " << *judge_model
                  << " is not known to the daemon: " << card.error().detail << '\n';
        return 2;
      }
      // The card also carries the judge's own architecture context, which bounds its
      // window below: handing a 16K-architecture judge the working model's 64K num_ctx
      // would push it past what it was built for, and that degrades silently -- no
      // truncation, so not even the boundary tell can see it.
      if (card->architecture_context) {
        judge_window = std::min(judge_window, *card->architecture_context);
      }
    }
    reinvoke_options.semantic = expectations.semantic;
    reinvoke_options.judge = hermit::supervisor::SemanticJudge{
        .chat = [&client](const hermit::ollama::ChatRequest& r) { return client->chat(r); },
        .model = judge_model.value_or(config->model),
        .num_ctx = judge_window,
    };
  }
  // The first attempt's banner is the run header below; a retry announces itself with
  // the one sentence the model is being re-invoked over, before its turns start.
  reinvoke_options.on_attempt = [](std::size_t attempt, std::size_t of,
                                   const hermit::supervisor::Finding* retrying) {
    if (retrying == nullptr) return;
    std::cout << "\nattempt " << attempt << " of " << of
              << " -- re-invoking with the one concrete remaining failure (R7):\n"
              << "  " << retrying->reason << "\n\n";
  };

  std::cout << "root    : " << box->root() << '\n'
            << "backups : " << *store_dir << "  (outside the root, R4; kept "
            << keep_hours << "h)\n"
            << "model   : " << config->model << '\n';
  if (hermit::ollama::is_cloud_tag(config->model)) {
    std::cout << "cloud   : yes -- routed via the local daemon to Ollama Cloud (D18)\n";
  }
  std::cout << "window  : " << probe->window() << " tokens, " << probe->prompt_budget()
            << " for the prompt\n"
            << "tools   : " << tools->registry().tools().size() << " offered"
            << (config->delete_tool.enabled ? " (delete on)" : "")
            << (shell_options ? " (shell on)" : "") << '\n'
            << "bounds  : " << max_turns << " turns, " << budget_seconds << "s, "
            << loop_options.max_calls_per_turn << " calls/turn\n"
            << "verify  : " << (verify ? "per-turn hash diff of the tree (R6)" : "off")
            << '\n'
            << "expect  : "
            << (expectations.empty()
                    ? std::string{"nothing stated -- report only"}
                    : std::to_string(expectations.set.size()) + " structural, " +
                          std::to_string(expectations.semantic.size()) +
                          " judged for meaning, " + std::to_string(expectations.unjudged) +
                          " unjudged, up to " + std::to_string(attempts) + " attempts (R7)")
            << '\n';
  if (!expectations.semantic.empty()) {
    const std::string& judge_name = judge_model.value_or(config->model);
    std::cout << "judge   : " << judge_name
              << "  (a fresh session per attempt; its verdicts are the model's judgment,"
                 " not a measurement)\n";
    if (hermit::ollama::is_cloud_tag(judge_name)) {
      std::cout << "cloud   : yes -- the judge routes real file content to Ollama Cloud "
                   "via the local daemon (D18)\n";
    }
  }
  std::cout << "\n"
            << "instruction: " << instruction << "\n\n";

  const auto job = hermit::supervisor::reinvoke(*client, tools->registry(), *box,
                                                loop_options, reinvoke_options, make_session,
                                                instruction);

  // D18: recorded regardless of job.error below -- tokens already spent are billed
  // whether or not the job finished cleanly, and before the early return so an error
  // path cannot skip it.
  if (hermit::ollama::is_cloud_tag(config->model)) {
    hermit::app::record_cloud_usage(*box, config->model, job_prompt_tokens,
                                    job_completion_tokens);
  }

  if (!job.error.empty()) {
    std::cerr << "error: " << job.error << '\n';
    return 1;
  }
  const auto& outcome = job.last();

  // The job at a glance, one line per attempt, before the detail of the final one. Only
  // when R7 actually re-invoked -- a single attempt's story is already told below.
  if (job.attempts.size() > 1) {
    std::cout << "\nre-invocation (R7): " << job.attempts.size() << " of " << attempts
              << " attempts used\n";
    for (std::size_t i = 0; i < job.attempts.size(); ++i) {
      const auto& made = job.attempts[i].outcome;
      std::cout << "  attempt " << (i + 1) << ": ";
      if (const auto unmet = made.verdict.first_unmet()) {
        std::cout << "unmet -- " << unmet->reason;
      } else if (made.verdict.met()) {
        std::cout << "met";
      } else {
        std::cout << "undecidable";
      }
      std::cout << "  (" << made.turns << (made.turns == 1 ? " turn, " : " turns, ")
                << made.elapsed.count() << " ms)\n";
    }
  }

  std::cout << "\nstopped : " << hermit::supervisor::to_string(outcome.reason) << '\n'
            << "turns   : " << outcome.turns << " of " << max_turns << '\n'
            << "calls   : " << outcome.calls << " (" << outcome.refusals << " refused)\n"
            << "dropped : " << outcome.dropped << " turns of history\n"
            // Reported on its own line rather than added to the one above. The two mean
            // different things -- history lost in silence against history replaced by
            // re-observed state -- and a single number would hide the distinction the
            // feature exists to make.
            << "rebuilt : " << outcome.compactions
            << (outcome.compactions == 1 ? " time from the tree\n" : " times from the tree\n")
            << "elapsed : " << outcome.elapsed.count() << " ms\n";
  if (!outcome.detail.empty()) std::cout << "detail  : " << outcome.detail << '\n';
  if (!outcome.final_content.empty()) {
    std::cout << "\nreply:\n" << outcome.final_content << '\n';
  } else if (!outcome.final_reasoning.empty() && outcome.ran_to_completion()) {
    // The model answered into its scratchpad and stopped -- measured behaviour, not a
    // guess (LoopOutcome::final_reasoning). The channel is shown because the operator
    // asked the model a question and this is where its answer went; the label keeps it
    // from reading as an ordinary reply. R6/D13 unaffected: nothing here is judged.
    std::cout << "\nreply: none -- the model emitted no content and answered into its "
                 "thinking channel,\n       which follows verbatim:\n"
              << outcome.final_reasoning << '\n';
  }

  if (verify) {
    // With one attempt this is the whole run's effect. With several it is the final
    // attempt's only -- each attempt's changes were printed under its own turns, and
    // claiming run-level truth for an attempt-level measurement would be false. The
    // *verdict* below is job-level either way: every attempt is judged against the one
    // baseline taken before the first.
    std::cout << (job.attempts.size() > 1
                      ? "\nwhat the final attempt changed (hash-verified, R6):\n"
                      : "\nwhat actually changed (hash-verified, R6):\n");
    if (outcome.net_changes.empty()) {
      std::cout << "  nothing\n";
    } else {
      const std::string rendered = outcome.net_changes.render();
      std::string line;
      for (const char c : rendered) {
        if (c != '\n') {
          line.push_back(c);
          continue;
        }
        std::cout << "  " << line << '\n';
        line.clear();
      }
      // substantive() counts changes, not bytes -- PermissionsChanged is one of the kinds
      // it counts, and a chmod moves none.
      std::cout << "  (" << outcome.net_changes.substantive() << " of "
                << outcome.net_changes.changes.size() << " are substantive)\n";
    }
  }

  if (!expectations.set.empty() || !expectations.semantic.empty() ||
      expectations.unjudged > 0) {
    std::cout << (expectations.semantic.empty()
                      ? "\nwhat was asked for (structural post-conditions, R6):\n"
                      : "\nwhat was asked for (structure measured per R6; meaning judged "
                        "per D15):\n");
    std::string line;
    for (const char c : outcome.verdict.render()) {
      if (c != '\n') {
        line.push_back(c);
        continue;
      }
      std::cout << "  " << line << '\n';
      line.clear();
    }

    if (const auto vacuous = outcome.verdict.vacuous(); vacuous > 0) {
      const bool one = vacuous == 1;
      std::cout << "  (" << vacuous << (one ? " of these was" : " of these were")
                << " already true before the run began,\n"
                   "   so the model could have passed "
                << (one ? "it" : "them") << " by doing nothing)\n";
    }
    if (const auto blind = outcome.verdict.count(hermit::supervisor::Outcome::Undecidable);
        blind > 0) {
      // Never counted as met, and never sent to the model: restating "one side could not
      // be read" spends a retry on a sentence it cannot act on (judge.h).
      std::cout << "  (" << blind
                << " could not be decided -- the judge was not able to look, which is\n"
                   "   reported to you and deliberately withheld from the model)\n";
    }
    if (const auto unjudged = expectations.unjudged; unjudged > 0) {
      const bool one = unjudged == 1;
      std::cout << "  (" << unjudged << (one ? " requirement was" : " requirements were")
                << " declared unjudgeable and " << (one ? "was" : "were")
                << " never examined\n"
                   "   here at all -- \"all met\" above does not cover "
                << (one ? "it" : "them") << ")\n";
    }
  }

  // Said out loud rather than left to be inferred from a zero exit status. What exists now
  // is the *observation* half of R6: the changeset above owes nothing to the reply, so a
  // model announcing success over an untouched tree is contradicted by it. What does not
  // exist is the judgment half -- deciding whether those are the *right* changes needs a
  // post-condition, and a free-text instruction does not carry one (see verify.h).
  //
  // Every sentence below is gated on `verify`, and that gate is the whole point. With
  // --no-verify, `net_changes` is empty because nothing was ever looked at -- so an
  // ungated "nothing moved" would report a successful run as the exact R6 failure this
  // command exists to detect, asserting a measurement that was never taken. Same rule as
  // D13 (a run that cannot be verified must not look like one that was), applied here at
  // the CLI rather than in the loop.
  if (outcome.ran_to_completion()) {
    if (!verify) {
      std::cout << "\nnote: the model stopped asking for tools. Nothing about the tree was\n"
                   "      checked -- --no-verify was given, so this run is unverified and\n"
                   "      a clean stop is not evidence that anything happened.\n";
    } else if (expectations.set.empty() && expectations.semantic.empty()) {
      std::cout << "\nnote: the model stopped asking for tools, and the changes above are what\n"
                   "      the filesystem shows. Whether they are the *correct* changes is not\n"
                   "      decided here -- that needs a post-condition this command was not given.\n";
      if (outcome.net_changes.substantive() == 0) {
        std::cout << "      Nothing moved. For a read-only instruction that is right; for one\n"
                     "      that asked for work, it is the failure R6 was written about.\n";
      }
    } else {
      std::cout << "\nnote: the model stopped asking for tools, and the structure you stated was\n"
                   "      checked against the tree rather than against the reply. Structure is\n"
                   "      not meaning: a file can exist, hash correctly, and hold the wrong\n"
                   "      thing -- which is a real recorded run, not a caveat (DECISIONS.md,\n"
                   "      D13). What is claimed here is exactly what is printed above.\n";
    }
  }
  // The exit code answers "did the stated work hold", not merely "did the loop finish".
  // Three states, because collapsing them loses the distinction the whole judgment half is
  // built on:
  //
  //   1  the run did not complete -- a bound, a transport failure, an unreadable tree
  //   3  it completed, and something stated was measurably not done (R7's trigger)
  //   0  it completed, and nothing stated was found undone
  //
  // Undecidable does *not* reach 3. The judge could not look, which is an operator problem
  // rather than a failed run, and `first_unmet()` skips it for the same reason -- it is
  // not a finding anything can act on. It is still reported above, so it cannot pass
  // silently.
  if (!outcome.ran_to_completion()) return 1;
  return outcome.verdict.first_unmet().has_value() ? 3 : 0;
}

// R4's operator side: list what the store preserved, put a generation back, or apply
// retention by hand. No flag lists -- the safe reading is the default, and both
// mutations are an explicit flag away.
int undo_command(std::span<const std::string_view> args) {
  std::optional<std::filesystem::path> backup_dir;
  std::optional<std::uint64_t> restore_seq;
  bool restore_last = false;
  bool prune_now = false;
  std::uint64_t keep_hours = 72;
  bool keep_hours_given = false;

  std::vector<std::string_view> passthrough;
  bool malformed = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto takes_value = [&](std::string_view flag, std::string_view& out) {
      if (args[i] != flag) return false;
      if (i + 1 >= args.size()) {
        std::cerr << "error: " << flag << " needs a value\n";
        malformed = true;
        return false;
      }
      out = args[++i];
      return true;
    };
    std::string_view value;
    if (malformed) break;
    if (takes_value("--backups", value)) {
      backup_dir = std::filesystem::path{value};
      continue;
    }
    if (takes_value("--restore", value)) {
      // Not `whole_number`: that helper refuses zero because zero is never a valid
      // bound, but generation 0000 is the first thing every store writes, and the
      // operator will type the zero-padded name straight off the listing.
      std::uint64_t seq = 0;
      const auto* const end = value.data() + value.size();
      const auto [stopped, ec] = std::from_chars(value.data(), end, seq);
      if (ec != std::errc{} || stopped != end || value.empty()) {
        std::cerr << "error: --restore needs a generation number from the listing, got: "
                  << value << '\n';
        return 2;
      }
      restore_seq = seq;
      continue;
    }
    if (takes_value("--keep-hours", value)) {
      const auto parsed = whole_number(value, 8760);
      if (!parsed) {
        std::cerr << "error: --keep-hours needs a whole number from 1 to 8760, got: "
                  << value << '\n';
        return 2;
      }
      keep_hours = *parsed;
      keep_hours_given = true;
      continue;
    }
    if (args[i] == "--last") {
      restore_last = true;
      continue;
    }
    if (args[i] == "--prune") {
      prune_now = true;
      continue;
    }
    passthrough.push_back(args[i]);
  }
  if (malformed) return 2;

  const int actions = (restore_seq ? 1 : 0) + (restore_last ? 1 : 0) + (prune_now ? 1 : 0);
  if (actions > 1) {
    std::cerr << "error: --restore, --last and --prune are one decision each; "
                 "pick one per invocation\n";
    return 2;
  }
  if (keep_hours_given && !prune_now) {
    std::cerr << "error: --keep-hours only means something with --prune here; "
                 "the agent applies it automatically at start\n";
    return 2;
  }

  std::vector<std::string_view> words;
  // No model and no network, same as resolve: undo is pure filesystem work.
  const auto config = hermit::app::load(
      passthrough, {.sandbox_root = true, .model = false, .ollama = false}, words);
  if (!config) return report(config.error());
  if (!words.empty()) {
    std::cerr << "error: undo takes no positional arguments, got: " << words.front() << '\n';
    return 2;
  }

  auto box = hermit::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermit::to_string(box.error()) << ": " << config->sandbox_root
              << '\n';
    return 1;
  }
  const auto store_dir = hermit::app::resolve_backup_dir(*box, std::move(backup_dir));
  if (!store_dir) {
    std::cerr << "error: --backups must be outside --root (R4): "
              << hermit::app::to_string(store_dir.error()) << '\n';
    return 2;
  }

  if (prune_now) {
    const auto pruned = hermit::supervisor::prune(
        *store_dir, std::chrono::hours{keep_hours},
        std::filesystem::file_time_type::clock::now());
    if (!pruned) {
      std::cerr << "error: " << pruned.error() << '\n';
      return 1;
    }
    std::cout << "pruned " << pruned->generations
              << (pruned->generations == 1 ? " generation" : " generations")
              << " older than " << keep_hours << "h from " << store_dir->string() << '\n';
    return 0;
  }

  const auto generations = hermit::supervisor::enumerate(*store_dir);
  if (!generations) {
    std::cerr << "error: " << generations.error() << '\n';
    return 1;
  }
  if (generations->empty()) {
    // A listing of an empty history is a fact; an explicitly requested restore that
    // cannot happen is a failure, and a scripted caller must be able to tell them
    // apart by the exit code.
    if (restore_seq || restore_last) {
      std::cerr << "error: nothing to restore: no generations in " << store_dir->string()
                << '\n';
      return 1;
    }
    std::cout << "nothing to undo: no generations in " << store_dir->string() << '\n';
    return 0;
  }

  if (restore_last) restore_seq = generations->back().seq;
  if (restore_seq) {
    const auto restored =
        hermit::supervisor::restore(*store_dir, *box, *restore_seq);
    if (!restored) {
      std::cerr << "error: " << restored.error() << '\n';
      return 1;
    }
    std::cout << "restored " << restored->generation.relative.string()
              << " from generation " << restored->generation.name << '\n';
    if (restored->preserved) {
      // The listing names generations by directory; say it the same way, so the redo
      // the operator may want next is a copy-paste from this line.
      const std::filesystem::path as_generation =
          restored->preserved->lexically_relative(*store_dir);
      std::cout << "(its previous content is preserved as generation "
                << as_generation.begin()->string() << ")\n";
    } else {
      std::cout << "(the file did not exist, so nothing needed preserving)\n";
    }
    return 0;
  }

  // The listing, newest first: the mistake being recovered from is almost always the
  // most recent one.
  std::cout << "undo data in " << store_dir->string() << " (newest first):\n\n"
            << "  generation  preserved at         bytes       file\n";
  for (auto it = generations->rbegin(); it != generations->rend(); ++it) {
    const auto sys_when =
        std::chrono::clock_cast<std::chrono::system_clock>(it->when);
    const std::time_t t = std::chrono::system_clock::to_time_t(sys_when);
    char stamp[32] = "unknown";
    if (std::tm tm{}; ::localtime_r(&t, &tm) != nullptr) {
      std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    }
    std::cout << "  " << it->name << "        " << stamp << "  ";
    std::string size = std::to_string(it->bytes);
    if (size.size() < 10) size.append(10 - size.size(), ' ');
    std::cout << size << "  " << it->relative.string() << '\n';
  }
  std::cout << "\nrestore one with: hermit undo --root " << box->root().string()
            << " --restore <generation>\n";
  return 0;
}

// D18: what Cloud-routed jobs against this root are estimated to have cost. Grouped by
// model rather than listed per job -- a long-lived root accumulates many jobs against
// the same handful of models, and the number worth seeing first is "how much did
// deepseek-v4-flash cost me", not fifty individual lines.
int usage_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> extra;
  // Same shape as undo: pure filesystem/log reading, no model, no network.
  const auto config =
      hermit::app::load(args, {.sandbox_root = true, .model = false, .ollama = false}, extra);
  if (!config) return report(config.error());
  if (!extra.empty()) {
    std::cerr << "error: usage takes no positional arguments, got: " << extra.front() << '\n';
    return 2;
  }

  auto box = hermit::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermit::to_string(box.error()) << ": " << config->sandbox_root
              << '\n';
    return 1;
  }

  const auto dir = hermit::app::resolve_usage_dir(*box);
  std::vector<std::string> warnings;
  const auto records = hermit::app::read_usage_records(dir, warnings);
  for (const auto& warning : warnings) {
    std::cerr << "warning: skipped a damaged usage-log line: " << warning << '\n';
  }

  if (records.empty()) {
    std::cout << "no Cloud usage logged for " << box->root().string() << " (checked "
              << dir.string() << ")\n";
    return 0;
  }

  struct Aggregate {
    std::size_t jobs = 0;
    std::uint64_t prompt_tokens = 0;
    std::uint64_t completion_tokens = 0;
  };
  std::map<std::string, Aggregate> by_model;
  for (const auto& record : records) {
    auto& agg = by_model[record.model];
    ++agg.jobs;
    agg.prompt_tokens += record.prompt_tokens;
    agg.completion_tokens += record.completion_tokens;
  }

  std::cout << "usage in " << dir.string() << " (" << records.size()
            << (records.size() == 1 ? " job" : " jobs") << ", " << by_model.size()
            << (by_model.size() == 1 ? " model" : " models") << "):\n\n"
            << "  model                       jobs   prompt tok  completion tok    est. $\n";

  double total = 0.0;
  std::vector<std::string> unpriced;
  for (const auto& [model, agg] : by_model) {
    hermit::app::UsageRecord synthetic;
    synthetic.model = model;
    synthetic.prompt_tokens = agg.prompt_tokens;
    synthetic.completion_tokens = agg.completion_tokens;
    const auto cost = hermit::app::price_usage(synthetic);

    std::string name = model;
    if (name.size() < 26) name.append(26 - name.size(), ' ');
    std::string jobs = std::to_string(agg.jobs);
    if (jobs.size() < 6) jobs.insert(0, 6 - jobs.size(), ' ');
    std::string prompt = std::to_string(agg.prompt_tokens);
    if (prompt.size() < 12) prompt.insert(0, 12 - prompt.size(), ' ');
    std::string completion = std::to_string(agg.completion_tokens);
    if (completion.size() < 14) completion.insert(0, 14 - completion.size(), ' ');

    std::cout << "  " << name << jobs << "  " << prompt << "  " << completion << "  ";
    if (cost) {
      std::cout << "$" << std::fixed << std::setprecision(4) << *cost << '\n';
      total += *cost;
    } else {
      std::cout << "  n/a\n";
      unpriced.push_back(model);
    }
  }

  std::cout << "\ntotal (priced models only): $" << std::fixed << std::setprecision(4) << total
            << '\n';
  if (!unpriced.empty()) {
    std::cout << "\nno rate-card entry, not counted in the total above:\n";
    for (const auto& model : unpriced) std::cout << "  " << model << '\n';
    std::cout << "add it to kRateCard in usage.cpp (and docs/31-ollama-cloud-economics.md)\n"
                 "if it is worth tracking.\n";
  }

  std::cout << "\nEstimate only, from this root's local log -- not the account balance.\n"
               "Ollama Cloud has no usage API (ollama/ollama#15132, #15663). Cross-check at\n"
               "ollama.com/settings, especially before trusting this for a budget decision.\n"
               "Priced at full input rate: Cloud's cache discount is not reliably applied\n"
               "(ollama/ollama#16714), so this assumes none is credited.\n";
  return 0;
}

// Everything that is in force, and where each value came from.
int config_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> extra;
  // Nothing is required, and nothing is dialled: printing the defaults is a legitimate
  // thing to want, and refusing to show a config because it is incomplete is unhelpful
  // precisely when it would be most useful. `.ollama = false` is what makes that true
  // of a bad URL too -- `render()` marks it rather than this command refusing to run.
  const auto config =
      hermit::app::load(args, {.sandbox_root = false, .model = false, .ollama = false}, extra);
  if (!config) return report(config.error());
  if (!extra.empty()) {
    std::cerr << "error: config takes no positional arguments, got: " << extra.front() << '\n';
    return 2;
  }

  std::cout << config->render();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();

  std::vector<std::string_view> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);

  const std::string_view command{argv[1]};
  if (command == "resolve") return resolve_command(args);
  if (command == "preflight") return preflight_command(args);
  if (command == "session") return session_command(args);
  if (command == "agent") return agent_command(args);
  if (command == "undo") return undo_command(args);
  if (command == "usage") return usage_command(args);
  if (command == "mcp") return hermit::app::mcp_command(args);
  if (command == "config") return config_command(args);
  if (command == "--help" || command == "-h" || command == "help") {
    usage(std::cout);
    return 0;
  }
  std::cerr << "error: unknown command: " << command << '\n';
  return usage();
}
