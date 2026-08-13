// Manual harness for R1 sandbox resolution.
//
//   hermes-cpp <root> <path>...
//
// Prints how each path resolves against the root, or why it was rejected. Deliberately
// runs from whatever directory you invoke it in: resolution must not move when the
// working directory does, and this is how you see that by hand.

#include <hermes/core/sandbox.h>

#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: hermes-cpp <sandbox-root> <path>...\n";
    return 2;
  }

  auto box = hermes::Sandbox::open(argv[1]);
  if (!box) {
    std::cerr << "error: " << hermes::to_string(box.error()) << ": " << argv[1] << '\n';
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
  for (int i = 2; i < argc; ++i) {
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
