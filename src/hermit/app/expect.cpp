#include <hermit/app/expect.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace hermit::app {
namespace {

namespace fs = std::filesystem;

std::unexpected<ExpectError> fail(std::string_view spec, std::string message) {
  return std::unexpected(ExpectError{.spec = std::string{spec}, .message = std::move(message)});
}

/// Rule 2 from expect.h: the literal normalised spelling, produced the way verify.cpp
/// keys the snapshot. Never `resolve()`'s output.
std::expected<std::string, std::string> relative_key(std::string_view raw) {
  if (raw.empty()) return std::unexpected(std::string{"empty path"});

  fs::path p{raw};
  if (p.is_absolute()) {
    return std::unexpected(std::string{"absolute path; expectations are relative to --root"});
  }

  // Refused before normalisation, and this is correctness rather than tidiness. The gate
  // resolves POSIX-order, expanding each symlink as it is met; the key folds ".."
  // textually. The two disagree, and sandbox.h says so outright. Measured, not reasoned:
  // with `dirlink -> a/b`, `hermit resolve` prints [a/x] for "dirlink/../x" while
  // lexically_normal gives "x" -- so containment would pass on one path while the key
  // named another, which is the silent mis-key this file exists to prevent.
  //
  // Compared as whole components, so the legal filename "..notes" is untouched.
  for (const auto& component : p) {
    if (component == "..") {
      return std::unexpected(
          std::string{"contains \"..\"; expectations name paths as spelled from the root"});
    }
  }

  // With no ".." left there is nothing for normalisation to get wrong: it only drops "."
  // and redundant separators, and the walker builds its keys from literal component
  // names without ever following a link.
  p = p.lexically_normal();

  // A trailing separator survives lexically_normal as an empty final element ("sub/"
  // stays "sub/"), and no snapshot key carries one.
  if (!p.has_filename()) p = p.parent_path();

  std::string key = p.string();
  if (key.empty() || key == ".") {
    return std::unexpected(
        std::string{"names the sandbox root, which has no entry in a snapshot"});
  }
  return key;
}

/// Rule 1 gates, rule 2 keys. The resolved path is deliberately discarded.
std::expected<std::string, std::string> checked_path(std::string_view raw,
                                                     const Sandbox& sandbox) {
  auto key = relative_key(raw);
  if (!key) return key;
  if (auto resolved = sandbox.resolve(raw); !resolved) {
    return std::unexpected(std::string{to_string(resolved.error())});
  }
  return key;
}

constexpr bool one_sided(std::string_view kind) {
  return kind == "exists" || kind == "dir" || kind == "absent";
}

constexpr bool two_sided(std::string_view kind) {
  return kind == "preserved" || kind == "identical";
}

/// Path on the left, free text on the right. Not two_sided: the right half is a
/// criterion, never a path, so it takes no gate and may contain anything -- including
/// the '=' that would make a two-sided kind refuse.
constexpr bool criterion_kind(std::string_view kind) { return kind == "satisfies"; }

/// The one place a kind becomes an Expectation, shared by the flag and object forms so
/// the two cannot drift.
std::expected<supervisor::Expectation, ExpectError> build(std::string_view kind,
                                                          std::string_view path,
                                                          std::string_view other,
                                                          bool has_other, bool as_directory,
                                                          std::string_view spec,
                                                          const Sandbox& sandbox) {
  if (!one_sided(kind) && !two_sided(kind) && !criterion_kind(kind)) {
    return fail(spec, "unknown kind \"" + std::string{kind} +
                          "\"; expected exists, dir, absent, preserved, identical or satisfies");
  }
  if (one_sided(kind) && has_other) {
    return fail(spec, std::string{kind} + " takes one path, but a second was given");
  }
  if (two_sided(kind) && !has_other) {
    return fail(spec, std::string{kind} + " needs two paths, written FROM=TO");
  }
  if (criterion_kind(kind) && (!has_other || other.empty())) {
    return fail(spec, "satisfies needs a criterion, written satisfies:PATH=what the "
                      "content must satisfy");
  }
  if (as_directory && kind != "exists" && kind != "dir") {
    return fail(spec, "\"as_directory\" applies to exists only");
  }

  auto first = checked_path(path, sandbox);
  if (!first) {
    return fail(spec, (two_sided(kind) ? "first path: " : "") + first.error());
  }

  if (one_sided(kind)) {
    if (kind == "absent") return supervisor::Expectation::absent(std::move(*first));
    return supervisor::Expectation::exists(std::move(*first), as_directory || kind == "dir");
  }

  // The criterion is the operator's words, carried verbatim -- it is what the judge is
  // asked and what an unmet finding quotes back, so no gate rewrites it.
  if (criterion_kind(kind)) {
    return supervisor::Expectation::satisfies(std::move(*first), std::string{other});
  }

  auto second = checked_path(other, sandbox);
  if (!second) return fail(spec, "second path: " + second.error());

  if (kind == "preserved") {
    return supervisor::Expectation::preserved(std::move(*first), std::move(*second));
  }
  return supervisor::Expectation::identical(std::move(*first), std::move(*second));
}

/// Which paths a set constrains, and how, when the run ends. First statement wins, so a
/// message names what the author wrote first.
///
/// Read off each predicate's implementation in judge.cpp rather than off its name:
///
///   * `Exists(p)` requires p present; with `must_be_dir`, present *and* a directory.
///   * `Preserved(from, to)` requires `to` present and holding bytes. `from` is read from
///     the **baseline** and is unconstrained at the end -- which is the point, since a move
///     leaves it gone. `Preserved(p -> p)` is the "do not touch p" spelling, and there
///     `to` is p.
///   * `Identical(a, b)` reads both sides from the current tree, so both must be present
///     and hold bytes -- `holds_no_bytes` on either side is never `Met`.
struct Constraints {
  std::map<std::string, const supervisor::Expectation*> present;
  std::map<std::string, const supervisor::Expectation*> absent;
  std::map<std::string, const supervisor::Expectation*> directory;
  std::map<std::string, const supervisor::Expectation*> bytes;
};

Constraints constraints_of(const supervisor::ExpectationSet& set) {
  Constraints out;
  for (const auto& e : set) {
    switch (e.kind) {
      case supervisor::ExpectationKind::Exists:
        out.present.emplace(e.path, &e);
        if (e.must_be_dir) out.directory.emplace(e.path, &e);
        break;
      case supervisor::ExpectationKind::Absent:
        out.absent.emplace(e.path, &e);
        break;
      case supervisor::ExpectationKind::Preserved:
        out.present.emplace(e.other, &e);
        out.bytes.emplace(e.other, &e);
        break;
      case supervisor::ExpectationKind::Identical:
        out.present.emplace(e.path, &e);
        out.present.emplace(e.other, &e);
        out.bytes.emplace(e.path, &e);
        out.bytes.emplace(e.other, &e);
        break;
      case supervisor::ExpectationKind::Satisfies:
        // The semantic judge answers a missing file Unmet and a directory cannot hold
        // the judged bytes, so a criterion requires its path present and holding
        // content -- which is what lets `absent:x` or `dir:x` beside `satisfies:x=...`
        // be refused here instead of failing every run.
        out.present.emplace(e.path, &e);
        out.bytes.emplace(e.path, &e);
        break;
    }
  }
  return out;
}

/// A set no tree could satisfy, refused where it was authored.
///
/// Not pedantry about tidy input. An unsatisfiable set is `Unmet` on every turn, so R7
/// restates the same impossible failure until a bound stops the run -- the whole retry
/// budget spent on an errand that could never have succeeded, and the operator handed
/// "unmet" with nothing pointing at the *set* as the broken thing.
///
/// Two axes, both decidable from the set alone:
///
///   existence  a path required present and required absent
///   kind       a path required to be a directory and required to hold bytes
///
/// **What this deliberately does not catch**, because it would need the baseline and this
/// function has none: two `Preserved` expectations naming different sources and the same
/// destination. That is unsatisfiable only when the sources differ in content, which no
/// amount of reading the set can tell you.
std::optional<ExpectError> unsatisfiable(const supervisor::ExpectationSet& set) {
  const auto constraints = constraints_of(set);

  using Table = std::map<std::string, const supervisor::Expectation*>;
  const auto clash = [](const Table& lhs, const Table& rhs,
                        std::string_view why) -> std::optional<ExpectError> {
    for (const auto& [path, expectation] : lhs) {
      const auto found = rhs.find(path);
      if (found == rhs.end()) continue;
      return ExpectError{.spec = path,
                         .message = "stated both as \"" + found->second->render() + "\" and \"" +
                                    expectation->render() + "\"; " + std::string{why}};
    }
    return std::nullopt;
  };

  if (auto broken = clash(constraints.absent, constraints.present,
                          "no tree can satisfy both, so every run would report it unmet")) {
    return broken;
  }
  return clash(constraints.bytes, constraints.directory,
               "a directory holds no bytes, so no tree can satisfy both");
}

}  // namespace

std::string ExpectError::render() const {
  return "expectation \"" + spec + "\": " + message;
}

std::expected<supervisor::Expectation, ExpectError> parse_expectation(
    std::string_view spec, const Sandbox& sandbox) {
  if (spec.empty()) return fail(spec, "empty expectation");

  const auto colon = spec.find(':');
  if (colon == std::string_view::npos) {
    return fail(spec,
                "no kind; expected exists:, dir:, absent:, preserved:, identical: or satisfies:");
  }

  const std::string_view kind = spec.substr(0, colon);
  const std::string_view rest = spec.substr(colon + 1);

  // A one-sided kind takes the whole remainder, so "exists:report=final.md" names a file
  // with an '=' in it rather than failing. A two-sided kind needs exactly one '=': a
  // second one makes the split a guess, and guessing here produces a wrong expectation
  // that R7 would go on to restate to the model as a concrete failure. Refused instead.
  if (one_sided(kind)) {
    return build(kind, rest, {}, /*has_other=*/false, /*as_directory=*/false, spec, sandbox);
  }
  // satisfies: splits at the FIRST '=' -- the right half is free text and may contain
  // any number of them; a path with '=' in it is the object form's job here.
  if (criterion_kind(kind)) {
    const auto eq = rest.find('=');
    if (eq == std::string_view::npos) {
      return build(kind, rest, {}, /*has_other=*/false, /*as_directory=*/false, spec, sandbox);
    }
    return build(kind, rest.substr(0, eq), rest.substr(eq + 1), /*has_other=*/true,
                 /*as_directory=*/false, spec, sandbox);
  }
  const auto eq = rest.find('=');
  if (eq == std::string_view::npos) {
    return build(kind, rest, {}, /*has_other=*/false, /*as_directory=*/false, spec, sandbox);
  }
  if (rest.find('=', eq + 1) != std::string_view::npos) {
    return fail(spec, std::string{kind} +
                          " has more than one '=', so which one splits the pair is a guess; "
                          "state it in the object form instead");
  }
  return build(kind, rest.substr(0, eq), rest.substr(eq + 1), /*has_other=*/true,
               /*as_directory=*/false, spec, sandbox);
}

std::expected<Expectations, ExpectError> parse_expectations_json(const nlohmann::json& value,
                                                                 const Sandbox& sandbox) {
  Expectations out;
  if (!value.is_object()) return fail("expectations", "settings must be a JSON object");

  if (const auto it = value.find("unjudged"); it != value.end()) {
    if (!it->is_number_unsigned()) {
      return fail("unjudged", "must be a non-negative integer");
    }
    out.unjudged = it->get<std::size_t>();
  }

  const auto it = value.find("expectations");
  if (it == value.end()) return out;
  if (!it->is_array()) return fail("expectations", "must be an array");

  for (const auto& element : *it) {
    if (element.is_string()) {
      auto parsed = parse_expectation(element.get<std::string>(), sandbox);
      if (!parsed) return std::unexpected(parsed.error());
      out.set.push_back(std::move(*parsed));
      continue;
    }
    if (!element.is_object()) {
      return fail(element.dump(), "must be a string or an object");
    }

    const std::string spec = element.dump();
    const auto kind = element.find("kind");
    if (kind == element.end() || !kind->is_string()) {
      return fail(spec, "missing string \"kind\"");
    }
    const auto path = element.find("path");
    if (path == element.end() || !path->is_string()) {
      return fail(spec, "missing string \"path\"");
    }

    std::string other;
    bool has_other = false;
    bool via_criterion_key = false;
    if (const auto found = element.find("other"); found != element.end()) {
      if (!found->is_string()) return fail(spec, "\"other\" must be a string");
      other = found->get<std::string>();
      has_other = true;
    }
    if (const auto found = element.find("criterion"); found != element.end()) {
      if (!found->is_string()) return fail(spec, "\"criterion\" must be a string");
      if (has_other) return fail(spec, "\"other\" and \"criterion\" together make no kind");
      other = found->get<std::string>();
      has_other = true;
      via_criterion_key = true;
    }
    // The key must match the kind: `other` is documented as a path and `criterion` as
    // free text, and accepting either for either would let a criterion slide through
    // the two-path grammar unchecked -- or a path masquerade as prose.
    const bool wants_criterion = kind->get<std::string>() == "satisfies";
    if (has_other && wants_criterion != via_criterion_key) {
      return fail(spec, wants_criterion
                            ? "satisfies takes \"criterion\", not \"other\""
                            : "\"criterion\" applies to satisfies only");
    }

    bool as_directory = false;
    if (const auto found = element.find("as_directory"); found != element.end()) {
      if (!found->is_boolean()) return fail(spec, "\"as_directory\" must be a boolean");
      as_directory = found->get<bool>();
    }

    auto parsed = build(kind->get<std::string>(), path->get<std::string>(), other, has_other,
                        as_directory, spec, sandbox);
    if (!parsed) return std::unexpected(parsed.error());
    out.set.push_back(std::move(*parsed));
  }
  if (auto broken = unsatisfiable(out.set)) return std::unexpected(std::move(*broken));

  // Split after the cross-check, which must see the whole set -- `absent:x` clashes with
  // `satisfies:x=...` exactly as with `exists:x`. Structure goes to the per-turn judge,
  // criteria to the per-attempt one; each keeps its own declaration order.
  supervisor::ExpectationSet structural;
  for (auto& e : out.set) {
    if (e.kind == supervisor::ExpectationKind::Satisfies) {
      out.semantic.push_back(std::move(e));
    } else {
      structural.push_back(std::move(e));
    }
  }
  out.set = std::move(structural);
  return out;
}

}  // namespace hermit::app
