#pragma once

// D4 --- Tool interface: virtual dispatch, declarative arguments.
//
// One descriptor list per tool feeds everything that must agree about its
// arguments: the tool definition offered to Ollama in its `tools` array (D12),
// the MCP tool definition a programmatic caller reads (D7), and the parser that
// turns a model-supplied call into typed values. Hand-maintained copies of that
// information disagree eventually -- a field added to one and not the other
// fails silently -- so R2 drift is made unrepresentable instead: there is one
// declaration, and everything else is derived from it (ROUTING.md section 7).
//
// The first of those renderers exists as of 2026-08-17, in
// `supervisor/wire.cpp`. Until then this paragraph said the descriptors became
// "the JSON Schema sent to Ollama as `format` (D5)", which D12 falsified:
// `format` cannot be sent beside `tools` -- it breaks tool calling outright on
// four of the seven models measured. The destination was wrong, not the
// guarantee.
//
// Everything in this header is JSON-free on purpose. D2 put nlohmann in the
// build with the Ollama client, and hermes_core links no JSON library.
// Descriptors, arguments and results are pure data here; rendering them to and
// from JSON happens in a layer that already has the library. The tier boundary
// stays enforced by the build graph, not by discipline.
//
// R1 is structural, not procedural: parsing an argument declared Path produces
// a SandboxPath, which only Sandbox::resolve constructs. The argument channel
// therefore cannot hand a tool a file outside the root, and the containment
// check cannot be forgotten at tool #37. Stated at its real width: a tool body
// is still first-party C++ and could reach the filesystem by other means --
// that is what D10's kernel confinement backstops, not what this header claims
// to prevent.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
/// struct -- the Ollama `tools` entry and the MCP tool definition both derive
/// from here, so there is no second copy to fall out of sync. The first of those
/// renderers exists as of 2026-08-17: `supervisor/wire.cpp`'s
/// `tool_definition()`. The second, MCP's, is still to come and will call it.
///
/// `args` is a view: it must reference storage that outlives the spec, in
/// practice a static array beside the tool. C++23 span will happily bind a
/// temporary vector here and dangle at the next sequence point -- do not.
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
// incoming JSON is the decoder's problem to reject.
//
// This said such input "should not arise" under D5's constrained decoding.
// After D12 there is no decode-time constraint on tool arguments at all -- the
// model's own tool-call channel is what shapes them -- so the decoder's
// rejection is the only thing standing there, and `wire.cpp`'s `raw_args_from`
// refuses a number, a boolean or a null rather than coercing it. Measured, that
// input does arise: llama3.2-3b sent `paths` as a scalar where a list belongs,
// and a Python-style list inside a string before that.

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

/// A refused call: fails closed per R9, reported as data in the vocabulary
/// ROUTING.md section 8 established -- which argument, what went wrong, and
/// for a path, what the sandbox said about which raw value. The model sees
/// this and gets to try again; nothing here throws.
struct ArgError {
  std::string arg;
  ArgErrorKind kind;
  std::optional<PathError> path_error;  // set iff kind == BadPath
  std::string detail;                   // set only with path_error: the raw
                                        // value the sandbox refused
};

std::string to_string(const ArgError& e);

/// Arguments after validation. Only parse_args constructs one -- the same move
/// SandboxPath makes for R1: every instance descends from a validated parse,
/// so the checks cannot be skipped at the dispatch site. Each instance also
/// records which ToolSpec it was parsed against, and Tool::invoke refuses a
/// mismatch -- so arguments parsed for one tool and dispatched to another are
/// a loud refusal, not a null dereference.
///
/// What is deliberately NOT bound: the Sandbox. The paths in here are proven
/// inside *a* root, not necessarily this call's root; a caller running several
/// roots at once (ROUTING.md section 10) must keep that pairing straight
/// itself, until SandboxPath carries its provenance.
///
/// Accessors return null (or an empty span) when the argument is absent,
/// which for an argument the tool's own spec marks required cannot happen once
/// invoke has admitted the call. They are lvalue-qualified because their
/// results alias this object: `(*parse_args(...)).paths(...)` and `.value()`
/// chains on a temporary fail to compile. The language cannot catch the
/// `parse_args(...)->paths(...)` spelling, so bind the result to a name first.
class ToolArgs {
 public:
  [[nodiscard]] const std::string* string(std::string_view name) const& noexcept;
  [[nodiscard]] const SandboxPath* path(std::string_view name) const& noexcept;
  [[nodiscard]] std::span<const SandboxPath> paths(std::string_view name) const& noexcept;

  const std::string* string(std::string_view) const&& = delete;
  const SandboxPath* path(std::string_view) const&& = delete;
  std::span<const SandboxPath> paths(std::string_view) const&& = delete;

  /// The declaration this was parsed against. Null only in a moved-from
  /// instance, which Tool::invoke therefore also refuses.
  [[nodiscard]] const ToolSpec* parsed_against() const noexcept { return spec_; }

  ToolArgs(const ToolArgs&) = default;
  ToolArgs& operator=(const ToolArgs&) = default;
  ToolArgs(ToolArgs&& other) noexcept
      : spec_(std::exchange(other.spec_, nullptr)),
        entries_(std::move(other.entries_)) {}
  ToolArgs& operator=(ToolArgs&& other) noexcept {
    if (this != &other) {
      spec_ = std::exchange(other.spec_, nullptr);
      entries_ = std::move(other.entries_);
    }
    return *this;
  }

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

  const ToolSpec* spec_ = nullptr;
  std::vector<Entry> entries_;
};

/// Validate `raw` against `spec` and resolve every Path/PathList argument
/// through `sandbox`. Fails closed on the first problem found: unknown and
/// duplicated names are refused first, in input order; everything else --
/// absences, shapes, refused paths -- is found walking the spec, so those
/// report in spec order.
///
/// `spec` must outlive the returned ToolArgs, which records its address for
/// Tool::invoke's identity check. A Tool's own spec() satisfies that by
/// contract.
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

  /// The declaration everything else is derived from. Must return the same
  /// object every call, and one that outlives the tool -- in practice, a
  /// static beside the subclass. invoke's identity check leans on this.
  [[nodiscard]] virtual const ToolSpec& spec() const noexcept = 0;

  /// The dispatch entry point. Admits `args` only if it was parsed against
  /// THIS tool's declaration -- pointer identity, which the stable-spec()
  /// contract above makes meaningful -- and refuses otherwise, including the
  /// moved-from case. A cross-tool wiring bug is thereby a loud refusal at
  /// the point it happens, the same shape of failure ToolRegistry::add gives
  /// a duplicate name, rather than a null dereference inside run().
  [[nodiscard]] std::expected<ToolOutput, ToolError> invoke(const ToolArgs& args) {
    if (args.parsed_against() != &spec()) {
      return std::unexpected{ToolError{
          "arguments were not parsed against this tool's declaration"}};
    }
    return run(args);
  }

 protected:
  Tool() = default;

  /// Execute the one whole job (ROUTING.md section 4). Reached only through
  /// invoke, so `args` is guaranteed parsed against spec(); run() does not
  /// re-validate. Override privately: dispatch goes through invoke.
  [[nodiscard]] virtual std::expected<ToolOutput, ToolError> run(const ToolArgs& args) = 0;
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

  /// Registration order, stable, so schema emission is deterministic. The
  /// span itself is invalidated by a later add(), like any view of a growing
  /// vector; a Tool* from find() is not -- the tools do not move, and it
  /// dangles only when the registry does.
  [[nodiscard]] std::span<const std::unique_ptr<Tool>> tools() const noexcept {
    return tools_;
  }

 private:
  std::vector<std::unique_ptr<Tool>> tools_;
};

}  // namespace hermes
