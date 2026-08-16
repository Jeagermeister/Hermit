#pragma once

// D4 --- Tool interface: virtual dispatch, declarative arguments.
//
// One descriptor list per tool feeds everything that must agree about its
// arguments: the JSON Schema sent to Ollama as `format` (D5), the MCP tool
// definition a programmatic caller reads (D7), and the parser that turns a
// model-supplied call into typed values. Hand-maintained copies of that
// information disagree eventually -- a field added to one and not the other
// fails silently -- so R2 drift is made unrepresentable instead: there is one
// declaration, and everything else is derived from it (ROUTING.md section 7).
//
// Everything in this header is JSON-free on purpose. D2 put nlohmann in the
// build with the Ollama client, and hermes_core links no JSON library.
// Descriptors, arguments and results are pure data here; rendering them to and
// from JSON happens in a layer that already has the library. The tier boundary
// stays enforced by the build graph, not by discipline.
//
// R1 is structural, not procedural: parsing an argument declared Path produces
// a SandboxPath, which only Sandbox::resolve constructs. A tool written against
// this interface cannot name a file outside the root, so the containment check
// cannot be forgotten at tool #37.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <hermes/core/sandbox.h>

namespace hermes {

// --- the declaration: pure data, written once per tool -----------------------

enum class ArgType {
  String,    // free text: a pattern, file content. Never a filesystem path.
  Path,      // one path, resolved through Sandbox::resolve at parse time
  PathList,  // one or more paths, each resolved the same way
};

/// One argument, declared. `name` and `doc` are string_views because they are
/// expected to reference literals beside the tool that owns them; a ToolSpec
/// must outlive every parse and every schema rendering derived from it.
struct ArgSpec {
  std::string_view name;
  ArgType type = ArgType::String;
  bool required = true;
  std::string_view doc;
};

/// A tool, declared: the single source the schema generators render and the
/// parser enforces. ROUTING.md section 7 extends D4's guarantee through this
/// struct -- the Ollama `format` schema and the MCP tool definition are both
/// rendered from here, so there is no second copy to fall out of sync.
struct ToolSpec {
  std::string_view name;
  std::string_view description;
  std::span<const ArgSpec> args;
};

enum class SpecError {
  EmptyToolName,
  EmptyArgName,
  DuplicateArgName,
};

std::string_view to_string(SpecError e) noexcept;

/// Structural validity: non-empty tool name, non-empty argument names, no two
/// arguments sharing one. ToolRegistry::add refuses an invalid spec, so a
/// malformed declaration fails at composition time, not mid-session.
[[nodiscard]] std::expected<void, SpecError> validate(const ToolSpec& spec);

// --- what the JSON layer hands over ------------------------------------------
//
// The layer that decodes a model's tool call has nlohmann; this one does not.
// It translates each argument into one of these before calling parse_args.
// Two shapes only, because the three ArgTypes above only need two: scalars
// arrive as a string, lists as a vector of strings. Anything else in the
// incoming JSON is the decoder's problem to reject -- with D5's constrained
// decoding it should not arise, but "should not" is that layer's claim to
// verify, not this one's assumption.

struct RawArg {
  std::string name;
  std::variant<std::string, std::vector<std::string>> value;
};
using RawArgs = std::vector<RawArg>;

// --- parsing: the one place arguments become typed ---------------------------

enum class ArgErrorKind {
  UnknownArg,       // not in the spec; rejected, never silently ignored
  DuplicateArg,     // supplied more than once
  MissingRequired,  // declared required, not supplied
  WrongShape,       // a list where a scalar belongs, or the reverse
  EmptyPathList,    // PathList means one or more; zero is a refusal, not success
  BadPath,          // Sandbox::resolve refused it; path_error says why
};

std::string_view to_string(ArgErrorKind e) noexcept;

/// A refused call, reported as data (R9's vocabulary): which argument, what
/// went wrong, and for a path, what the sandbox said about which raw value.
/// The model sees this and gets to try again; nothing here throws.
struct ArgError {
  std::string arg;
  ArgErrorKind kind;
  std::optional<PathError> path_error;  // set iff kind == BadPath
  std::string detail;                   // the refused raw value, when one exists
};

std::string to_string(const ArgError& e);

/// Arguments after validation. Only parse_args constructs one -- the same move
/// SandboxPath makes for R1: a Tool::run that takes ToolArgs cannot receive an
/// unvalidated call, so the checks cannot be skipped at the dispatch site.
///
/// Accessors return null (or an empty span) when the argument is absent, which
/// for an argument the tool's own spec marks required cannot happen -- parse_args
/// has already enforced presence -- so dereferencing is sound there. A mistyped
/// name in a tool shows up as a null in that tool's first test, loudly.
class ToolArgs {
 public:
  [[nodiscard]] const std::string* string(std::string_view name) const noexcept;
  [[nodiscard]] const SandboxPath* path(std::string_view name) const noexcept;
  [[nodiscard]] std::span<const SandboxPath> paths(std::string_view name) const noexcept;

 private:
  using Value = std::variant<std::string, SandboxPath, std::vector<SandboxPath>>;
  struct Entry {
    std::string name;
    Value value;
  };

  friend std::expected<ToolArgs, ArgError> parse_args(const ToolSpec&,
                                                      const RawArgs&,
                                                      const Sandbox&);
  ToolArgs() = default;

  std::vector<Entry> entries_;
};

/// Validate `raw` against `spec` and resolve every Path/PathList argument
/// through `sandbox`. Fails closed on the first problem found, in spec order
/// for absences and input order for everything else.
///
/// Precondition: validate(spec) has passed. ToolRegistry::add enforces that at
/// composition time, which is why it is not re-checked per call.
[[nodiscard]] std::expected<ToolArgs, ArgError> parse_args(const ToolSpec& spec,
                                                           const RawArgs& raw,
                                                           const Sandbox& sandbox);

// --- what tools return --------------------------------------------------------
//
// ROUTING.md section 5: no decoration, ever. Content travels exact; everything
// else -- hashes, line numbers, identity tuples -- travels in named sibling
// fields, never interleaved with the bytes. Every result in section 4's tables
// is rows of named fields: `read` is one row per file (path, content, hash),
// `grep` one per match (path, line, text), `list` one per entry. So that is
// the whole shape. Pure data; rendered to JSON above core, like the specs.

using FieldValue = std::variant<std::string, std::int64_t, std::uint64_t, bool>;

struct Field {
  std::string name;
  FieldValue value;
};

struct ToolRow {
  std::vector<Field> fields;
};

struct ToolOutput {
  std::vector<ToolRow> rows;
};

/// ROUTING.md section 3: a Tier 0 tool never silently falls back -- no answer
/// is a valid answer. A failure is one loud line the model can act on.
struct ToolError {
  std::string reason;
};

// --- dispatch -----------------------------------------------------------------

/// The D4 base: virtual dispatch over a declarative spec. A tool is dumb on
/// purpose (ROUTING.md section 4) -- one complete job, no judgment, no guessing
/// what was meant -- and it never reaches a model; hermes_core cannot link one.
class Tool {
 public:
  virtual ~Tool() = default;
  Tool(const Tool&) = delete;
  Tool& operator=(const Tool&) = delete;

  /// The declaration everything else is derived from. Must return a reference
  /// that outlives the tool -- in practice, to a static beside the subclass.
  [[nodiscard]] virtual const ToolSpec& spec() const noexcept = 0;

  /// Execute the one whole job. `args` was parsed against spec() by the
  /// dispatch site; run() does not re-validate.
  [[nodiscard]] virtual std::expected<ToolOutput, ToolError> run(const ToolArgs& args) = 0;

 protected:
  Tool() = default;
};

enum class RegistryErrorKind {
  NullTool,
  BadSpec,        // spec_error says which way it failed validate()
  DuplicateName,  // two tools answering to one name is a wiring bug; replacing
                  // the first would hide it, so it is refused instead
};

std::string_view to_string(RegistryErrorKind e) noexcept;

struct RegistryError {
  RegistryErrorKind kind;
  std::optional<SpecError> spec_error;  // set iff kind == BadSpec
};

/// The tool set one frontend exposes. Composition happens in the app layer and
/// exposure is a policy decision per frontend (ROUTING.md section 8); this
/// container just holds what was composed and finds tools by name.
class ToolRegistry {
 public:
  /// Takes ownership. Validates the spec and refuses duplicates, so a wiring
  /// mistake fails at composition time rather than mid-session.
  std::expected<void, RegistryError> add(std::unique_ptr<Tool> tool);

  /// nullptr when nothing answers to `name`. What that means -- a decline, a
  /// protocol error -- is the caller's policy question, not the registry's.
  [[nodiscard]] Tool* find(std::string_view name) noexcept;
  [[nodiscard]] const Tool* find(std::string_view name) const noexcept;

  /// Registration order, stable, so schema emission is deterministic.
  [[nodiscard]] std::span<const std::unique_ptr<Tool>> tools() const noexcept {
    return tools_;
  }

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
};

}  // namespace hermes
