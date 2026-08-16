#include <hermes/core/tool.h>

#include <algorithm>
#include <utility>

namespace hermes {

std::string_view to_string(SpecError e) noexcept {
  switch (e) {
    case SpecError::EmptyToolName:    return "tool name is empty";
    case SpecError::EmptyArgName:     return "argument name is empty";
    case SpecError::DuplicateArgName: return "two arguments share a name";
  }
  return "unknown spec error";
}

std::string_view to_string(ArgErrorKind e) noexcept {
  switch (e) {
    case ArgErrorKind::UnknownArg:      return "not an argument of this tool";
    case ArgErrorKind::DuplicateArg:    return "argument supplied more than once";
    case ArgErrorKind::MissingRequired: return "required argument missing";
    case ArgErrorKind::WrongShape:      return "wrong shape (scalar vs list)";
    case ArgErrorKind::EmptyPathList:   return "path list is empty";
    case ArgErrorKind::BadPath:         return "path refused by the sandbox";
  }
  return "unknown argument error";
}

std::string_view to_string(RegistryErrorKind e) noexcept {
  switch (e) {
    case RegistryErrorKind::NullTool:      return "null tool";
    case RegistryErrorKind::BadSpec:       return "tool spec failed validation";
    case RegistryErrorKind::DuplicateName: return "a tool with this name is already registered";
  }
  return "unknown registry error";
}

std::string to_string(const ArgError& e) {
  std::string out{e.arg};
  out += ": ";
  out += to_string(e.kind);
  if (e.path_error) {
    out += " (";
    out += to_string(*e.path_error);
    out += ")";
  }
  if (!e.detail.empty()) {
    out += ": ";
    out += e.detail;
  }
  return out;
}

std::expected<void, SpecError> validate(const ToolSpec& spec) {
  if (spec.name.empty()) return std::unexpected{SpecError::EmptyToolName};
  for (std::size_t i = 0; i < spec.args.size(); ++i) {
    if (spec.args[i].name.empty()) return std::unexpected{SpecError::EmptyArgName};
    for (std::size_t j = i + 1; j < spec.args.size(); ++j) {
      if (spec.args[i].name == spec.args[j].name) {
        return std::unexpected{SpecError::DuplicateArgName};
      }
    }
  }
  return {};
}

namespace {

const ArgSpec* find_spec(const ToolSpec& spec, std::string_view name) {
  auto it = std::ranges::find_if(spec.args,
                                 [&](const ArgSpec& a) { return a.name == name; });
  return it == spec.args.end() ? nullptr : &*it;
}

}  // namespace

std::expected<ToolArgs, ArgError> parse_args(const ToolSpec& spec,
                                             const RawArgs& raw,
                                             const Sandbox& sandbox) {
  // Input order first: unknown and duplicated names are properties of the call,
  // and rejecting them before looking at absences reports the caller's actual
  // mistake rather than a downstream symptom of it.
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (find_spec(spec, raw[i].name) == nullptr) {
      return std::unexpected{ArgError{raw[i].name, ArgErrorKind::UnknownArg, {}, {}}};
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (raw[j].name == raw[i].name) {
        return std::unexpected{ArgError{raw[i].name, ArgErrorKind::DuplicateArg, {}, {}}};
      }
    }
  }

  ToolArgs parsed;
  parsed.entries_.reserve(raw.size());

  for (const ArgSpec& a : spec.args) {
    auto it = std::ranges::find_if(raw,
                                   [&](const RawArg& r) { return r.name == a.name; });
    if (it == raw.end()) {
      if (a.required) {
        return std::unexpected{
            ArgError{std::string{a.name}, ArgErrorKind::MissingRequired, {}, {}}};
      }
      continue;
    }

    switch (a.type) {
      case ArgType::String: {
        const auto* s = std::get_if<std::string>(&it->value);
        if (s == nullptr) {
          return std::unexpected{
              ArgError{std::string{a.name}, ArgErrorKind::WrongShape, {}, {}}};
        }
        parsed.entries_.push_back({std::string{a.name}, *s});
        break;
      }
      case ArgType::Path: {
        const auto* s = std::get_if<std::string>(&it->value);
        if (s == nullptr) {
          return std::unexpected{
              ArgError{std::string{a.name}, ArgErrorKind::WrongShape, {}, {}}};
        }
        auto resolved = sandbox.resolve(*s);
        if (!resolved) {
          return std::unexpected{
              ArgError{std::string{a.name}, ArgErrorKind::BadPath, resolved.error(), *s}};
        }
        parsed.entries_.push_back({std::string{a.name}, std::move(*resolved)});
        break;
      }
      case ArgType::PathList: {
        const auto* list = std::get_if<std::vector<std::string>>(&it->value);
        if (list == nullptr) {
          return std::unexpected{
              ArgError{std::string{a.name}, ArgErrorKind::WrongShape, {}, {}}};
        }
        if (list->empty()) {
          return std::unexpected{
              ArgError{std::string{a.name}, ArgErrorKind::EmptyPathList, {}, {}}};
        }
        std::vector<SandboxPath> resolved_list;
        resolved_list.reserve(list->size());
        for (const std::string& s : *list) {
          auto resolved = sandbox.resolve(s);
          if (!resolved) {
            return std::unexpected{
                ArgError{std::string{a.name}, ArgErrorKind::BadPath, resolved.error(), s}};
          }
          resolved_list.push_back(std::move(*resolved));
        }
        parsed.entries_.push_back({std::string{a.name}, std::move(resolved_list)});
        break;
      }
    }
  }

  return parsed;
}

const std::string* ToolArgs::string(std::string_view name) const noexcept {
  for (const Entry& e : entries_) {
    if (e.name == name) return std::get_if<std::string>(&e.value);
  }
  return nullptr;
}

const SandboxPath* ToolArgs::path(std::string_view name) const noexcept {
  for (const Entry& e : entries_) {
    if (e.name == name) return std::get_if<SandboxPath>(&e.value);
  }
  return nullptr;
}

std::span<const SandboxPath> ToolArgs::paths(std::string_view name) const noexcept {
  for (const Entry& e : entries_) {
    if (e.name == name) {
      if (const auto* v = std::get_if<std::vector<SandboxPath>>(&e.value)) return *v;
      return {};
    }
  }
  return {};
}

std::expected<void, RegistryError> ToolRegistry::add(std::unique_ptr<Tool> tool) {
  if (tool == nullptr) {
    return std::unexpected{RegistryError{RegistryErrorKind::NullTool, {}}};
  }
  const ToolSpec& spec = tool->spec();
  if (auto ok = validate(spec); !ok) {
    return std::unexpected{RegistryError{RegistryErrorKind::BadSpec, ok.error()}};
  }
  if (find(spec.name) != nullptr) {
    return std::unexpected{RegistryError{RegistryErrorKind::DuplicateName, {}}};
  }
  tools_.push_back(std::move(tool));
  return {};
}

Tool* ToolRegistry::find(std::string_view name) noexcept {
  for (const auto& t : tools_) {
    if (t->spec().name == name) return t.get();
  }
  return nullptr;
}

const Tool* ToolRegistry::find(std::string_view name) const noexcept {
  for (const auto& t : tools_) {
    if (t->spec().name == name) return t.get();
  }
  return nullptr;
}

}  // namespace hermes
