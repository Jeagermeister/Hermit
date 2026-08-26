#include <hermit/core/sandbox.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using hermit::PathError;
using hermit::Sandbox;
using hermit::SandboxError;

namespace {

// Layout built for every test:
//
//   <tmp>/root/            <- the sandbox root
//   <tmp>/root/inside.txt
//   <tmp>/root/a/b/deep.txt
//   <tmp>/root/link_in  -> a/b            (symlink, stays inside)
//   <tmp>/root/link_out -> <tmp>/outside  (symlink, escapes)
//   <tmp>/outside/secret.txt
//   <tmp>/root2/sibling.txt               <- shares a string prefix with root
//   <tmp>/root_link     -> root           (symlinked root)
class SandboxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_sbx_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::canonical(fs::path(buf.data()));

    root_ = tmp_ / "root";
    outside_ = tmp_ / "outside";

    fs::create_directories(root_ / "a" / "b");
    fs::create_directories(outside_);
    fs::create_directories(tmp_ / "root2");

    write_file(root_ / "inside.txt", "in");
    write_file(root_ / "a" / "b" / "deep.txt", "deep");
    write_file(outside_ / "secret.txt", "secret");
    write_file(tmp_ / "root2" / "sibling.txt", "sibling");

    fs::create_directory_symlink("a/b", root_ / "link_in");      // two levels deep
    fs::create_directory_symlink(outside_, root_ / "link_out");   // absolute, escapes
    fs::create_directory_symlink("../outside", root_ / "link_rel_out");  // relative, escapes
    fs::create_directory_symlink("link_out", root_ / "link_chain");      // link -> link
    fs::create_directory_symlink(outside_, root_ / "a" / "mid_out");     // mid-path
    fs::create_directory_symlink(root_, tmp_ / "root_link");
    fs::create_directory_symlink(tmp_ / "root_link", tmp_ / "root_link2");  // chain
    fs::create_symlink("loop_b", root_ / "loop_a");               // ELOOP pair
    fs::create_symlink("loop_a", root_ / "loop_b");

    write_file(root_ / "deep.txt", "root-level");
    write_file(root_ / "a" / "deep.txt", "a-level");
    write_file(tmp_ / "inside.txt", "outside-sibling");
    write_file(root_ / "name with spaces.txt", "spaced");

    auto sb = Sandbox::open(root_);
    ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
    box_ = std::make_unique<Sandbox>(std::move(*sb));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  static void write_file(const fs::path& p, std::string_view contents) {
    std::ofstream out(p);
    out << contents;
  }

  fs::path tmp_, root_, outside_;
  std::unique_ptr<Sandbox> box_;
};

// --- opening -----------------------------------------------------------------

TEST_F(SandboxTest, OpenRejectsMissingRoot) {
  auto sb = Sandbox::open(tmp_ / "nope");
  ASSERT_FALSE(sb.has_value());
  EXPECT_EQ(sb.error(), SandboxError::RootNotFound);
}

TEST_F(SandboxTest, OpenRejectsFileAsRoot) {
  auto sb = Sandbox::open(root_ / "inside.txt");
  ASSERT_FALSE(sb.has_value());
  EXPECT_EQ(sb.error(), SandboxError::RootNotDirectory);
}

TEST_F(SandboxTest, OpenCanonicalisesSymlinkedRoot) {
  // A root reached through a symlink must still compare equal to resolved paths,
  // or every containment check silently fails open.
  auto sb = Sandbox::open(tmp_ / "root_link");
  ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
  EXPECT_EQ(sb->root(), root_);

  auto p = sb->resolve("inside.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
}

// --- the R1 regression -------------------------------------------------------

TEST_F(SandboxTest, IgnoresProcessWorkingDirectory) {
  // This is the failure that cost llama32-3b 0/3 on 01_create_file: correct content,
  // correct filename, resolved against the process working directory instead of the
  // sandbox root. Resolution must not move when the working directory does.
  const fs::path saved = fs::current_path();
  fs::current_path(outside_);

  auto p = box_->resolve("inside.txt");
  fs::current_path(saved);  // restore before any assertion can abort the test

  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
}

// --- ordinary resolution -----------------------------------------------------

TEST_F(SandboxTest, RelativeResolvesUnderRoot) {
  auto p = box_->resolve("inside.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
  EXPECT_EQ(p->relative(), fs::path("inside.txt"));
}

TEST_F(SandboxTest, NestedRelativeResolves) {
  auto p = box_->resolve("a/b/deep.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b" / "deep.txt");
  EXPECT_EQ(p->relative(), fs::path("a/b/deep.txt"));
}

TEST_F(SandboxTest, DotResolvesToRootItself) {
  auto p = box_->resolve(".");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_);
  EXPECT_EQ(p->relative(), fs::path("."));
}

TEST_F(SandboxTest, LeadingSlashRelativeToRootIsAccepted) {
  auto p = box_->resolve((root_ / "a/b/deep.txt").string());
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b" / "deep.txt");
}

TEST_F(SandboxTest, TrailingSlashIsAccepted) {
  auto p = box_->resolve("a/b/");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b");
}

TEST_F(SandboxTest, RedundantSeparatorsAreNormalised) {
  auto p = box_->resolve("a//b///deep.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b" / "deep.txt");
}

// --- paths that do not exist yet ---------------------------------------------

TEST_F(SandboxTest, NonexistentPathAcceptedForWrites) {
  auto p = box_->resolve("brand_new.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "brand_new.txt");
  EXPECT_FALSE(fs::exists(p->path()));
}

TEST_F(SandboxTest, NonexistentNestedPathAccepted) {
  auto p = box_->resolve("new/dir/tree/file.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "new" / "dir" / "tree" / "file.txt");
}

// --- interior dot-dot is legal, escaping dot-dot is not ----------------------

TEST_F(SandboxTest, InteriorDotDotStaysInside) {
  auto p = box_->resolve("a/../inside.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
}

TEST_F(SandboxTest, LeadingDotDotIsRejected) {
  auto p = box_->resolve("../outside/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, DeepDotDotEscapeIsRejected) {
  auto p = box_->resolve("a/b/../../../outside/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, BareDotDotIsRejected) {
  auto p = box_->resolve("..");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

// --- absolute paths ----------------------------------------------------------

TEST_F(SandboxTest, AbsoluteOutsideRootIsRejected) {
  auto p = box_->resolve("/etc/passwd");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, AbsoluteIntoSiblingSharingPrefixIsRejected) {
  // "<tmp>/root2" starts with "<tmp>/root" as a string but is not inside it.
  // A starts_with containment check would wrongly accept this.
  auto p = box_->resolve((tmp_ / "root2" / "sibling.txt").string());
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

// --- symlinks ----------------------------------------------------------------

TEST_F(SandboxTest, SymlinkStayingInsideIsAccepted) {
  auto p = box_->resolve("link_in/deep.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b" / "deep.txt");
}

TEST_F(SandboxTest, SymlinkEscapingRootIsRejected) {
  // Textual normalisation cannot see this one: "link_out/secret.txt" looks like it is
  // inside the root until the link is expanded.
  auto p = box_->resolve("link_out/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, SymlinkedDirectoryItselfIsRejected) {
  auto p = box_->resolve("link_out");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

// --- malformed input ---------------------------------------------------------

TEST_F(SandboxTest, EmptyPathIsRejected) {
  auto p = box_->resolve("");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::Empty);
}

TEST_F(SandboxTest, EmbeddedNulIsRejected) {
  auto p = box_->resolve(std::string_view("inside.txt\0/etc/passwd", 22));
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EmbeddedNul);
}

TEST_F(SandboxTest, ANewlineInAPathIsRejectedBecauseReportsAreLineOriented) {
  // The kernel accepts a newline in a filename. The supervisor's changeset report is
  // newline-delimited, so a model that can name a file could otherwise write lines an
  // operator reads as findings -- `created notes.txt` followed by a fabricated
  // `touched  config.ini`. Refused at resolve time, which covers every reader at once
  // rather than escaping it at each one. Found by review 2026-08-17.
  auto p = box_->resolve("notes.txt\ntouched  config.ini");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::ControlCharacter);
}

TEST_F(SandboxTest, OtherControlCharactersAreRejectedToo) {
  for (const std::string_view raw : {"a\tb.txt", "a\rb.txt", "a\x1b[31m.txt", "a\x7f.txt"}) {
    auto p = box_->resolve(raw);
    ASSERT_FALSE(p.has_value()) << raw;
    EXPECT_EQ(p.error(), PathError::ControlCharacter) << raw;
  }
}

TEST_F(SandboxTest, OrdinaryAwkwardNamesAreStillAccepted) {
  // The check is control characters only. Spaces, quotes, newline-adjacent punctuation and
  // non-ASCII are all legal names a person may genuinely have, and refusing them would be
  // the adjacent-success section 3 forbids -- a refusal that reads as a bug.
  for (const std::string_view raw : {"a file.txt", "quote'd.txt", "\"quoted\".txt", "n.txt"}) {
    auto p = box_->resolve(raw);
    EXPECT_TRUE(p.has_value()) << raw << ": " << (p ? "" : to_string(p.error()));
  }
}

TEST_F(SandboxTest, TildeIsNotExpanded) {
  // "~" names a literal directory. Inferring a home directory would reintroduce
  // exactly the implicit root R1 forbids.
  auto p = box_->resolve("~/notes.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "~" / "notes.txt");
}

// --- symlinks followed by ".." (POSIX order, not textual) --------------------

TEST_F(SandboxTest, DotDotAfterSymlinkFollowsThePosixTarget) {
  // link_in -> a/b, so "link_in/.." is "a", not the root. Collapsing "link_in/.."
  // textually yields root/deep.txt -- right name, wrong directory, which is exactly
  // the R1 failure this class exists to prevent.
  auto p = box_->resolve("link_in/../deep.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "deep.txt");
  EXPECT_NE(p->path(), root_ / "deep.txt");
}

TEST_F(SandboxTest, DotDotAfterEscapingSymlinkIsRejectedNotRebased) {
  // link_out -> <tmp>/outside, so this names <tmp>/inside.txt -- outside the root.
  // Textual collapse would rebase it to root/inside.txt: a silent escape *inward*,
  // onto a real and different file. R1 says reject, never rebase.
  auto p = box_->resolve("link_out/../inside.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, RelativeEscapingSymlinkIsRejected) {
  auto p = box_->resolve("link_rel_out/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, SymlinkChainEscapingIsRejected) {
  auto p = box_->resolve("link_chain/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, MidPathSymlinkEscapingIsRejected) {
  // The escaping link is an interior component, not the first.
  auto p = box_->resolve("a/mid_out/secret.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, EscapingSymlinkWithNonexistentTailIsRejected) {
  auto p = box_->resolve("link_out/not_created_yet.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

TEST_F(SandboxTest, LeavingAndReenteringTheRootIsAccepted) {
  // Judged on where it ends up, not the route it took.
  auto p = box_->resolve(("../" + root_.filename().string() + "/inside.txt"));
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
}

// --- an uninspectable component must not be assumed harmless -----------------

TEST_F(SandboxTest, UnreadableAncestorIsRejectedNotAssumedAbsent) {
  // "Not there" and "I cannot tell what is there" are different answers. Merging them
  // let an unexpanded symlink reach contains(), which assumes a resolved path and so
  // fails open: this exact input returned a SandboxPath reporting "sub/esc/secret.txt"
  // that read a file outside the root. A model can create this state itself.
  fs::create_directories(root_ / "sub");
  fs::create_directory_symlink(outside_, root_ / "sub" / "esc");
  fs::permissions(root_ / "sub", fs::perms::none);

  auto p = box_->resolve("sub/esc/secret.txt");
  fs::permissions(root_ / "sub", fs::perms::owner_all);  // restore before asserting

  ASSERT_FALSE(p.has_value()) << "accepted a path it could not resolve";
  EXPECT_EQ(p.error(), PathError::FilesystemError);
}

// --- relative symlink targets resolve against the link, not the root ---------

TEST_F(SandboxTest, RelativeTargetResolvesAgainstTheLinkNotTheRoot) {
  // Every relative-target link in the fixture sits directly in the root, where those
  // two bases are indistinguishable. This one is nested, so it tells them apart.
  fs::create_directories(root_ / "a" / "b" / "c");
  write_file(root_ / "a" / "b" / "c" / "target.txt", "nested");
  write_file(root_ / "c" / "target.txt", "decoy");   // what the root-based bug would hit
  fs::create_directory_symlink("../b/c", root_ / "a" / "b" / "inner");

  auto p = box_->resolve("a/b/inner/target.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "a" / "b" / "c" / "target.txt");
}

// --- dangling symlinks: the normal shape of writing through a link -----------

TEST_F(SandboxTest, DanglingSymlinkResolvesToItsTarget) {
  fs::create_symlink("not_created_yet.txt", root_ / "dangling");
  auto p = box_->resolve("dangling");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "not_created_yet.txt");
}

TEST_F(SandboxTest, DanglingSymlinkPointingOutIsRejected) {
  fs::create_symlink("../outside/not_created_yet.txt", root_ / "dangling_out");
  auto p = box_->resolve("dangling_out");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::EscapesRoot);
}

// --- the symlink budget is where the kernel puts it --------------------------

TEST_F(SandboxTest, SymlinkChainBoundaryMatchesTheKernel) {
  write_file(root_ / "chain_target.txt", "end");
  fs::create_symlink("chain_target.txt", root_ / "s0");
  for (int i = 1; i <= 45; ++i) {
    fs::create_symlink("s" + std::to_string(i - 1), root_ / ("s" + std::to_string(i)));
  }
  EXPECT_TRUE(box_->resolve("s38").has_value()) << "a legal chain was rejected";

  auto over = box_->resolve("s44");
  ASSERT_FALSE(over.has_value()) << "an over-long chain was accepted";
  EXPECT_EQ(over.error(), PathError::FilesystemError);
}

// --- input bounds ------------------------------------------------------------

TEST_F(SandboxTest, OverlongPathIsRejectedCheaply) {
  // A model stuck in a generation loop can emit a megabyte-long argument; resolution
  // allocates roughly 60x the input, so this is rejected before any of that happens.
  auto p = box_->resolve(std::string(64 * 1024, 'x'));
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::TooLong);
}

// --- filesystem-level failures fail closed -----------------------------------

TEST_F(SandboxTest, SymlinkLoopIsReportedNotHung) {
  auto p = box_->resolve("loop_a/x.txt");
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), PathError::FilesystemError);
}

// --- remaining surface -------------------------------------------------------

TEST_F(SandboxTest, LeadingDotSlashIsAccepted) {
  auto p = box_->resolve("./inside.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->path(), root_ / "inside.txt");
}

TEST_F(SandboxTest, NamesWithSpacesAreAccepted) {
  auto p = box_->resolve("name with spaces.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->relative(), fs::path("name with spaces.txt"));
}

TEST_F(SandboxTest, RootReachedThroughASymlinkChainIsCanonicalised) {
  auto sb = Sandbox::open(tmp_ / "root_link2");
  ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
  EXPECT_EQ(sb->root(), root_);
}

TEST_F(SandboxTest, RelativeRootIsResolvedAgainstTheWorkingDirectory) {
  // Documented, deliberate exception: open() is the one place the working directory
  // is read, and only at startup. resolve() never is -- see
  // IgnoresProcessWorkingDirectory. Pinned here so the asymmetry stays intentional.
  const fs::path saved = fs::current_path();
  fs::current_path(tmp_);
  auto sb = Sandbox::open("root");
  fs::current_path(saved);

  ASSERT_TRUE(sb.has_value()) << to_string(sb.error());
  EXPECT_EQ(sb->root(), root_);
}

TEST_F(SandboxTest, ResolvedPathCarriesItsOwnSandboxRoot) {
  // fsio.h's I/O primitives anchor their openat walk on this instead of taking a
  // Sandbox& -- see ROUTING.md section 12 step 5.
  auto p = box_->resolve("inside.txt");
  ASSERT_TRUE(p.has_value()) << to_string(p.error());
  EXPECT_EQ(p->sandbox_root(), root_);
}

}  // namespace
