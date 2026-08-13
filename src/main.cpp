// Manual harness for the pieces that exist so far.
//
//   hermes-cpp resolve <root> <path>...   R1 sandbox resolution
//   hermes-cpp preflight <model> [...]    R9 model preflight
//
// Not the product's CLI -- that arrives with the agent loop. This is how each piece is
// looked at by hand against the real filesystem and the real daemon, which is where
// the last two bugs came from.

#include <hermes/core/sandbox.h>
#include <hermes/ollama/preflight.h>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

int usage() {
  std::cerr << "usage: hermes-cpp resolve   <sandbox-root> <path>...\n"
               "       hermes-cpp preflight <model> [--url URL] [--min-context N] [--warmup]\n";
  return 2;
}

// R1 --- resolution, run from wherever you invoke it: resolution must not move when
// the working directory does, and this is how you see that by hand.
int resolve_command(int argc, char** argv) {
  if (argc < 4) return usage();

  auto box = hermes::Sandbox::open(argv[2]);
  if (!box) {
    std::cerr << "error: " << hermes::to_string(box.error()) << ": " << argv[2] << '\n';
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
  for (int i = 3; i < argc; ++i) {
    const std::string_view raw{argv[i]};
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
int preflight_command(int argc, char** argv) {
  if (argc < 3) return usage();
  const std::string_view model{argv[2]};

  hermes::ollama::Client::Options client_options;
  hermes::ollama::Policy policy;

  for (int i = 3; i < argc; ++i) {
    const std::string_view flag{argv[i]};
    const bool has_value = i + 1 < argc;
    if (flag == "--url" && has_value) {
      client_options.base_url = argv[++i];
    } else if (flag == "--min-context" && has_value) {
      const std::string_view raw{argv[++i]};
      const auto* const last = raw.data() + raw.size();
      // `stop != last` matters as much as the error code: without it "1abc" parses as
      // 1 and silently drops the R9 floor to a value that admits everything.
      const auto [stop, ec] = std::from_chars(raw.data(), last, policy.minimum_context);
      if (ec != std::errc{} || stop != last) {
        std::cerr << "error: --min-context expects a whole number, got: " << raw << '\n';
        return 2;
      }
    } else if (flag == "--warmup") {
      policy.warmup = true;
    } else if (flag == "--no-tools") {
      policy.require_tools = false;
    } else {
      std::cerr << "error: unrecognised argument: " << flag << '\n';
      return usage();
    }
  }

  const auto client = hermes::ollama::Client::open(client_options);
  if (!client) {
    std::cerr << "error: " << client.error().detail << '\n';
    return 1;
  }

  const auto report = hermes::ollama::preflight(*client, model, policy);

  // To stderr: the report is a startup diagnostic, and a caller redirecting stdout is
  // collecting the model's output, not this.
  std::cerr << report.render();
  return report.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();

  const std::string_view command{argv[1]};
  if (command == "resolve") return resolve_command(argc, argv);
  if (command == "preflight") return preflight_command(argc, argv);
  return usage();
}
