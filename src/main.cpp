// Manual harness for the pieces that exist so far.
//
//   hermes-cpp resolve   --root DIR <path>...   R1 sandbox resolution
//   hermes-cpp preflight --model NAME           R9 model preflight
//   hermes-cpp session   --model NAME           context accounting, against a real model
//   hermes-cpp config                           the resolved settings, and their origins
//
// Not the product's CLI -- that arrives with the agent loop. This is how each piece is
// looked at by hand against the real filesystem and the real daemon, which is where
// the last two bugs came from.
//
// Argument parsing moved out to hermes::app::Config on 2026-08-13. It is not shaped
// like a CLI parser because it is not one: the same settings have to arrive from a
// file and the environment when the MCP-over-stdio frontend lands (D7), and a second
// implementation of "where does the sandbox root come from" is exactly the divergence
// R1 is about.
//
// `config` is a real subcommand rather than a debugging aid. Four sources feed these
// settings and two of them are safety limits, so "what is actually in force, and which
// source set it?" needs an answer that does not involve reading the code.

#include <hermes/app/config.h>
#include <hermes/app/toolset.h>
#include <hermes/core/sandbox.h>
#include <hermes/ollama/preflight.h>
#include <hermes/supervisor/loop.h>
#include <hermes/supervisor/session.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <iostream>
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
      "usage: hermes-cpp resolve   --root DIR <path>...\n"
      "       hermes-cpp preflight --model NAME\n"
      "       hermes-cpp session   --model NAME   (try --max-num-ctx 2048 to force compaction)\n"
      "       hermes-cpp agent     --root DIR --model NAME <instruction>\n"
      "       hermes-cpp config\n"
      "\n"
      "settings, in increasing precedence: defaults < --config file < environment < flags\n"
      "  --config PATH          a JSON config file. Never searched for implicitly.\n"
      "  --root DIR             sandbox root (R1). No default, ever.\n"
      "  --model NAME           the Ollama tag to drive\n"
      "  --url URL              Ollama base URL; loopback only (D7)\n"
      "  --max-num-ctx N        hard ceiling on any num_ctx sent (D8 safety clamp)\n"
      "  --min-context N        R9 architectural context floor\n"
      "  --connect-timeout N    seconds\n"
      "  --metadata-timeout N   seconds\n"
      "  --chat-timeout N       seconds\n"
      "  --warmup / --no-warmup     R9 inference check; off by default\n"
      "  --tools   / --no-tools     R9 tools-capability gate; on by default\n"
      "\n"
      "agent only:\n"
      "  --max-turns N          turn bound for one run (default 12)\n"
      "  --budget N             wall-clock seconds for one run, checked between turns (R8)\n"
      "  --backups DIR          where undo data goes; must be outside --root (R4)\n"
      "\n"
      "environment: HERMES_CONFIG (absolute), HERMES_SANDBOX_ROOT (absolute), HERMES_MODEL,\n"
      "             HERMES_OLLAMA_URL, HERMES_MAX_NUM_CTX\n";
  return 2;
}

int report(const hermes::app::ConfigProblem& problem) {
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
      hermes::app::load(args, {.sandbox_root = true, .model = false, .ollama = false}, paths);
  if (!config) return report(config.error());
  if (paths.empty()) {
    std::cerr << "error: resolve needs at least one path to resolve\n";
    return usage();
  }

  auto box = hermes::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermes::to_string(box.error()) << ": " << config->sandbox_root << '\n';
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
                << "          -> " << hermes::to_string(p.error()) << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

// R9 --- the gates a model must pass before it is worth sending a first request.
int preflight_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> extra;
  const auto config =
      hermes::app::load(args, {.sandbox_root = false, .model = true, .ollama = true}, extra);
  if (!config) {
    // `preflight qwen35-agent` -- the old positional shape -- otherwise reports "no
    // model", which is true and unhelpful when the model name is right there.
    if (config.error().kind == hermes::app::ConfigError::MissingRequired && !extra.empty()) {
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

  const auto client = hermes::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  const auto report_out = hermes::ollama::preflight(*client, config->model, config->preflight);

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
      hermes::app::load(args, {.sandbox_root = false, .model = true, .ollama = true}, extra);
  if (!config) return report(config.error());
  if (!extra.empty()) {
    std::cerr << "error: session takes no positional arguments, got: " << extra.front() << '\n';
    return 2;
  }

  const auto client = hermes::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  // The architecture is the one ceiling no request can raise (R9), so the session is
  // told about it rather than left to discover it from a collapsed prompt. A model whose
  // card cannot be read is not a reason to refuse -- the clamp still bounds the window.
  hermes::supervisor::SessionOptions options;
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

  auto session = hermes::supervisor::Session::open(
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

  std::cout << "model  : " << config->model << '\n'
            << "window : " << session->window() << " tokens (least of the request, the D8 clamp"
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

  std::cout << "\ngenerated " << session->generated_tokens() << " tokens across "
            << session->completed_turns() << " turns; " << session->dropped()
            << " turns dropped to stay inside the window\n";
  return 0;
}

// Everything that is in force, and where each value came from.
// Phase 2 --- the loop, end to end: one instruction, the eight Tier 0 tools, a real
// model. This is the first subcommand where all four layers run at once.
int agent_command(std::span<const std::string_view> args) {
  // Pulled out before `load` sees the arguments, because these are this subcommand's own
  // knobs rather than settings with a config-file and environment precedence chain. A
  // flag config does not know about is an error there, deliberately.
  std::vector<std::string_view> passthrough;
  std::size_t max_turns = 12;
  std::uint64_t budget_seconds = 300;
  std::optional<std::filesystem::path> backup_dir;

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
      max_turns = std::strtoull(std::string{value}.c_str(), nullptr, 10);
      if (max_turns == 0) {
        std::cerr << "error: --max-turns needs a positive number, got: " << value << '\n';
        return 2;
      }
      continue;
    }
    if (takes_value("--budget", value)) {
      budget_seconds = std::strtoull(std::string{value}.c_str(), nullptr, 10);
      if (budget_seconds == 0) {
        std::cerr << "error: --budget needs a positive number of seconds, got: " << value << '\n';
        return 2;
      }
      continue;
    }
    if (takes_value("--backups", value)) {
      backup_dir = std::filesystem::path{value};
      continue;
    }
    passthrough.push_back(args[i]);
  }
  if (malformed) return 2;

  std::vector<std::string_view> words;
  const auto config = hermes::app::load(
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

  auto box = hermes::Sandbox::open(config->sandbox_root);
  if (!box) {
    std::cerr << "error: " << hermes::to_string(box.error()) << ": " << config->sandbox_root
              << '\n';
    return 1;
  }

  // R4: outside the root, always. Defaulted beside it rather than inside it -- a store
  // under the root would be listable, readable and editable by the model, which is the
  // whole point of the rule.
  if (!backup_dir) {
    backup_dir = box->root().parent_path() /
                 (".hermes-backups-" + box->root().filename().string());
  }
  {
    // Cheap containment check on the *lexical* paths, which is enough for an operator
    // typo. It is not a security boundary and is not claimed as one: both paths are
    // host-side operator configuration, not model input.
    //
    // Compared with a separator appended, not as a bare prefix. A bare prefix test
    // makes `/tmp/sandbox-backups` look "inside" `/tmp/sandbox`, which would refuse the
    // most natural place to put the store -- right beside the root, which is where this
    // command's own default puts it.
    std::string root = box->root().lexically_normal().string();
    while (root.size() > 1 && root.back() == std::filesystem::path::preferred_separator) {
      root.pop_back();
    }
    const std::string store =
        std::filesystem::absolute(*backup_dir).lexically_normal().string();
    if (store == root || store.starts_with(root + std::filesystem::path::preferred_separator)) {
      std::cerr << "error: --backups must be outside --root (R4): " << store << " is inside "
                << root << '\n';
      return 2;
    }
  }

  const auto client = hermes::ollama::Client::open(config->client);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  auto tools = hermes::app::ToolSet::tier0(*backup_dir);
  if (!tools) {
    std::cerr << "error: composing the tool set failed: "
              << hermes::to_string(tools.error().kind) << '\n';
    return 1;
  }

  hermes::supervisor::SessionOptions options;
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
  auto session = hermes::supervisor::Session::open(
      options, *client,
      "You do filesystem work by calling the tools you were given. Read a file before "
      "describing it; never guess its contents. Make one tool call per step, and stop "
      "calling tools only when the work is actually finished.");
  if (!session) {
    std::cerr << "error: " << session.error().message() << '\n';
    return 1;
  }

  hermes::supervisor::LoopOptions loop_options;
  loop_options.max_turns = max_turns;
  loop_options.budget = std::chrono::seconds{static_cast<long>(budget_seconds)};

  // One line per turn, then one indented line per call. The result is truncated for
  // display only -- a `read` of a real file would otherwise bury the trace, and what is
  // wanted here is the shape of the run, not its payload.
  loop_options.observer = [](const hermes::supervisor::TurnEvent& event) {
    std::cout << "  turn " << event.turn << "  prompt " << event.prompt_tokens
              << "  generated " << event.generated_tokens;
    if (event.reasoning_chars > 0) std::cout << "  thinking " << event.reasoning_chars << "ch";
    if (event.truncated) std::cout << "  [hit the generation budget]";
    if (event.dropped > 0) std::cout << "  dropped " << event.dropped;
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
  };

  hermes::supervisor::AgentLoop loop{*client, tools->registry(), *box, loop_options};

  std::cout << "root    : " << box->root() << '\n'
            << "backups : " << *backup_dir << "  (outside the root, R4)\n"
            << "model   : " << config->model << '\n'
            << "window  : " << session->window() << " tokens, " << session->prompt_budget()
            << " for the prompt\n"
            << "tools   : " << loop.definitions().size() << " offered\n"
            << "bounds  : " << max_turns << " turns, " << budget_seconds << "s\n\n"
            << "instruction: " << instruction << "\n\n";

  const auto outcome = loop.run(*session, instruction);

  std::cout << "\nstopped : " << hermes::supervisor::to_string(outcome.reason) << '\n'
            << "turns   : " << outcome.turns << " of " << max_turns << '\n'
            << "calls   : " << outcome.calls << " (" << outcome.refusals << " refused)\n"
            << "dropped : " << outcome.dropped << " turns of history\n"
            << "elapsed : " << outcome.elapsed.count() << " ms\n";
  if (!outcome.detail.empty()) std::cout << "detail  : " << outcome.detail << '\n';
  if (!outcome.final_content.empty()) {
    std::cout << "\nreply:\n" << outcome.final_content << '\n';
  }

  // R6, said out loud rather than left to be inferred from a zero exit status: the loop
  // stopping is not evidence the work is right. Phase 3's state verification is what
  // will make an exit status mean something.
  if (outcome.ran_to_completion()) {
    std::cout << "\nnote: the model stopped asking for tools. Whether the work is correct is\n"
                 "      not checked here -- that is Phase 3 (R6 state verification).\n";
  }
  return outcome.ran_to_completion() ? 0 : 1;
}

int config_command(std::span<const std::string_view> args) {
  std::vector<std::string_view> extra;
  // Nothing is required, and nothing is dialled: printing the defaults is a legitimate
  // thing to want, and refusing to show a config because it is incomplete is unhelpful
  // precisely when it would be most useful. `.ollama = false` is what makes that true
  // of a bad URL too -- `render()` marks it rather than this command refusing to run.
  const auto config =
      hermes::app::load(args, {.sandbox_root = false, .model = false, .ollama = false}, extra);
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
  if (command == "config") return config_command(args);
  if (command == "--help" || command == "-h" || command == "help") {
    usage(std::cout);
    return 0;
  }
  std::cerr << "error: unknown command: " << command << '\n';
  return usage();
}
