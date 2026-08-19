#include <hermit/supervisor/undo.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hermit/core/backup.h>
#include <hermit/core/fsio.h>

namespace hermit::supervisor {
namespace {

namespace fs = std::filesystem;

std::unexpected<std::string> fail(std::string why) {
  return std::unexpected{std::move(why)};
}

/// The marker check every operation shares. "Missing store" and "directory
/// that is not a store" are different answers: the first is an empty history,
/// the second is a directory this module has no business touching.
enum class StoreState { Missing, Marked, Unmarked };

std::expected<StoreState, std::string> classify(const fs::path& store) {
  std::error_code ec;
  const bool present = fs::exists(store, ec);
  if (ec) return fail("cannot examine " + store.string() + ": " + ec.message());
  if (!present) return StoreState::Missing;
  const bool marked = fs::exists(store / kStoreMarker, ec);
  if (ec) return fail("cannot examine " + store.string() + ": " + ec.message());
  return marked ? StoreState::Marked : StoreState::Unmarked;
}

std::string unmarked_message(const fs::path& store) {
  return store.string() + " exists but carries no " + std::string{kStoreMarker} +
         " marker, so it is not recognisably a hermit backup store; refusing. "
         "If it truly is one, create an empty file named " +
         std::string{kStoreMarker} + " inside it.";
}

}  // namespace

std::expected<std::vector<Generation>, std::string> enumerate(
    const fs::path& store) {
  const auto state = classify(store);
  if (!state) return std::unexpected{state.error()};
  if (*state == StoreState::Missing) return std::vector<Generation>{};
  if (*state == StoreState::Unmarked) return fail(unmarked_message(store));

  std::vector<Generation> out;
  std::error_code ec;
  fs::directory_iterator it{store, ec};
  if (ec) return fail("cannot list " + store.string() + ": " + ec.message());
  const fs::directory_iterator end;
  while (it != end) {
    const fs::path gen_dir = it->path();
    const std::string name = gen_dir.filename().string();
    it.increment(ec);
    if (ec) return fail("cannot list " + store.string() + ": " + ec.message());
    const auto parsed_seq = parse_generation_name(name);
    if (!parsed_seq) continue;
    if (!fs::is_directory(fs::symlink_status(gen_dir, ec)) || ec) continue;
    const std::uint64_t seq = *parsed_seq;

    // One file per generation by construction; walk defensively anyway, and
    // never through a symlink -- a hand-edited store must not be able to make
    // this walk read the rest of the filesystem.
    fs::recursive_directory_iterator files{
        gen_dir, fs::directory_options::none, ec};
    if (ec) return fail("cannot list " + gen_dir.string() + ": " + ec.message());
    const fs::recursive_directory_iterator files_end;
    while (files != files_end) {
      const fs::path file = files->path();
      const auto status = fs::symlink_status(file, ec);
      if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
      if (fs::is_regular_file(status)) {
        Generation g;
        g.seq = seq;
        g.name = name;
        g.relative = file.lexically_relative(gen_dir);
        g.bytes = fs::file_size(file, ec);
        if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
        g.when = fs::last_write_time(file, ec);
        if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
        out.push_back(std::move(g));
      }
      files.increment(ec);
      if (ec) return fail("cannot list " + gen_dir.string() + ": " + ec.message());
    }
  }

  std::ranges::sort(out, [](const Generation& a, const Generation& b) {
    return a.seq != b.seq ? a.seq < b.seq : a.relative < b.relative;
  });
  return out;
}

std::expected<Restored, std::string> restore(const fs::path& store, Sandbox& box,
                                             std::uint64_t seq) {
  const auto all = enumerate(store);
  if (!all) return std::unexpected{all.error()};

  std::vector<Generation> matching;
  for (const Generation& g : *all) {
    if (g.seq == seq) matching.push_back(g);
  }
  if (matching.empty()) {
    return fail("no generation " + std::to_string(seq) + " in " + store.string());
  }
  if (matching.size() > 1) {
    return fail("generation " + matching.front().name + " holds " +
                std::to_string(matching.size()) +
                " files, which no preservation ever writes; refusing an "
                "ambiguous restore of a hand-edited store");
  }
  const Generation& gen = matching.front();

  // The recorded path goes back through R1 like any other path headed for the
  // tree. preserve() refused absolute and `..` paths on the way in, but the
  // store is operator-editable, and an edited row must fail here rather than
  // write outside the root.
  auto target = box.resolve(gen.relative.string());
  if (!target) {
    return fail("the recorded path " + gen.relative.string() +
                " does not resolve inside the sandbox root: " +
                std::string{to_string(target.error())});
  }

  const fs::path source = store / gen.name / gen.relative;
  const int src_fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (src_fd < 0) {
    return fail("cannot open " + source.string() + ": " +
                to_string(IoError{.code = errno}));
  }
  Fd src{src_fd};

  // Back up before mutating -- the rule applies to the supervisor too. The
  // target's current bytes become a new generation in the same store, so a
  // restore of the wrong generation is itself undoable.
  Restored result;
  result.generation = gen;
  ::mode_t mode = 0;
  if (auto current = open_regular(*target)) {
    BackupStore keeper{store};
    auto kept = keeper.preserve_fd(target->relative(), current->fd.get());
    if (!kept) {
      return fail("preserving the current content of " +
                  target->relative().string() + " failed: " +
                  to_string(kept.error()) + "; nothing was restored");
    }
    result.preserved = std::move(*kept);
    mode = current->meta.st_mode & 0777;
  } else if (current.error().kind == IoError::Kind::Kernel &&
             current.error().code == ENOENT) {
    // Absent: nothing to preserve, and the restored file gets what a fresh
    // create would get. umask has no read-only accessor; single-threaded D1
    // makes the empty window harmless.
    ::mode_t mask = ::umask(0);
    ::umask(mask);
    mode = 0666 & ~mask;
  } else {
    // A directory, device or unreadable object where the file was: refusing
    // beats overwriting something that is no longer what the store preserved.
    return fail(target->relative().string() + ": " + to_string(current.error()));
  }

  std::error_code ec;
  fs::create_directories(target->path().parent_path(), ec);
  if (ec) {
    return fail("cannot recreate parent directories for " +
                target->relative().string() + ": " + ec.message());
  }

  // Stream to a temp beside the target, then publish by rename -- the same
  // atomic pattern as the write tool, streamed because backups are the one
  // content in this codebase with no size cap.
  std::string temp =
      (target->path().parent_path() / ".hermit-tmp.XXXXXX").string();
  const int tfd = ::mkostemp(temp.data(), O_CLOEXEC);
  if (tfd < 0) {
    return fail("cannot create a temp file beside " +
                target->relative().string() + ": " +
                to_string(IoError{.code = errno}));
  }
  Fd out{tfd};
  auto abandon = [&](std::string why) {
    ::unlink(temp.c_str());
    return fail(std::move(why));
  };
  if (::fchmod(out.get(), mode) != 0) {
    return abandon("cannot set the mode of the restored file: " +
                   to_string(IoError{.code = errno}));
  }
  char buf[65536];
  off_t offset = 0;
  for (;;) {
    const ssize_t n = ::pread(src.get(), buf, sizeof buf, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      return abandon("reading " + source.string() + " failed: " +
                     to_string(IoError{.code = errno}));
    }
    if (n == 0) break;
    if (auto written = write_all(out.get(), {buf, static_cast<std::size_t>(n)});
        !written) {
      return abandon("writing the restored content failed: " +
                     to_string(written.error()));
    }
    offset += n;
  }
  if (::rename(temp.c_str(), target->path().c_str()) != 0) {
    return abandon("publishing the restored file failed: " +
                   to_string(IoError{.code = errno}));
  }
  return result;
}

std::expected<Pruned, std::string> prune(const fs::path& store,
                                         std::chrono::hours keep,
                                         fs::file_time_type now) {
  const auto state = classify(store);
  if (!state) return std::unexpected{state.error()};
  if (*state == StoreState::Missing) return Pruned{};
  if (*state == StoreState::Unmarked) return fail(unmarked_message(store));

  const fs::file_time_type cutoff = now - keep;
  Pruned result;

  // Two phases, and the order is D14's guarantee: decide what goes, RAISE THE
  // NUMBERING FLOOR, then remove. A store whose every generation is past the
  // cutoff -- the normal state of any store idle longer than the window --
  // would otherwise scan empty on the next run and restart at 0000, handing a
  // fresh backup a pruned one's identity in the listing. Floor first also
  // fails safe: a crash between the raise and the removals leaves the
  // generations on disk, where the scan still covers them.
  struct Doomed {
    fs::path dir;
    std::uint64_t seq;
    std::uintmax_t bytes;
  };
  std::vector<Doomed> doomed;

  std::error_code ec;
  fs::directory_iterator it{store, ec};
  if (ec) return fail("cannot list " + store.string() + ": " + ec.message());
  const fs::directory_iterator end;
  while (it != end) {
    const fs::path gen_dir = it->path();
    const std::string name = gen_dir.filename().string();
    it.increment(ec);
    if (ec) return fail("cannot list " + store.string() + ": " + ec.message());
    // Only generation-named children are ours to delete. The marker, anything
    // an operator dropped beside the generations, and a numeric name too long
    // to ever have been written by the store, are never touched.
    const auto seq = parse_generation_name(name);
    if (!seq) continue;
    if (!fs::is_directory(fs::symlink_status(gen_dir, ec)) || ec) continue;

    // A generation goes only when everything in it is past the cutoff, aged by
    // its FILES: the directory's own mtime is a statement about the directory
    // (restocking a reused store bumps it), so it is only the fallback for an
    // empty generation -- crash residue between mkdir and the copy.
    std::optional<fs::file_time_type> newest;
    std::uintmax_t held = 0;
    fs::recursive_directory_iterator files{
        gen_dir, fs::directory_options::none, ec};
    if (ec) return fail("cannot list " + gen_dir.string() + ": " + ec.message());
    const fs::recursive_directory_iterator files_end;
    while (files != files_end) {
      const fs::path file = files->path();
      const auto status = fs::symlink_status(file, ec);
      if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
      if (fs::is_regular_file(status)) {
        const auto when = fs::last_write_time(file, ec);
        if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
        newest = newest ? std::max(*newest, when) : when;
        held += fs::file_size(file, ec);
        if (ec) return fail("cannot examine " + file.string() + ": " + ec.message());
      }
      files.increment(ec);
      if (ec) return fail("cannot list " + gen_dir.string() + ": " + ec.message());
    }
    if (!newest) {
      newest = fs::last_write_time(gen_dir, ec);
      if (ec) return fail("cannot examine " + gen_dir.string() + ": " + ec.message());
    }
    if (*newest >= cutoff) continue;
    doomed.push_back({gen_dir, *seq, held});
  }

  if (doomed.empty()) return result;

  std::uint64_t highest = 0;
  for (const Doomed& d : doomed) highest = std::max(highest, d.seq);
  if (auto raised = raise_store_floor(store, highest + 1); !raised) {
    return fail("cannot record the numbering floor in " + store.string() + "/" +
                std::string{kStoreMarker} + ": " + to_string(raised.error()) +
                "; nothing was pruned");
  }

  for (const Doomed& d : doomed) {
    fs::remove_all(d.dir, ec);
    if (ec) return fail("removing " + d.dir.string() + " failed: " + ec.message());
    ++result.generations;
    result.bytes += d.bytes;
  }
  return result;
}

}  // namespace hermit::supervisor
