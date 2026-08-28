// The MCP JSON-RPC layer, exercised without a real stdin/stdout conversation.
//
// `handle_message` is pure over an already-parsed request, a registry and a sandbox --
// the same reason `wire.cpp`'s functions are testable without a daemon, and the same
// reason `dispatch_call` (loop_test.cpp) is. What is *not* covered here is the framing
// layer around it -- reading a line, deciding it isn't valid JSON at all -- which lives
// in `mcp_command`'s stdin loop and has no direct unit test, matching this codebase's
// own convention for frontend glue (there is no `main_test.cpp` either). See mcp.h's
// header comment.

#include <hermit/app/mcp.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <hermit/app/toolset.h>
#include <hermit/core/sandbox.h>
#include <hermit/supervisor/wire.h>

namespace fs = std::filesystem;
using hermit::Sandbox;
using hermit::app::handle_message;
using hermit::app::ToolSet;
using json = nlohmann::json;

namespace {

constexpr std::string_view kName = "hermit-test";
constexpr std::string_view kVersion = "0.0.0-test";

class McpFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string tpl = (fs::temp_directory_path() / "hermit_mcp_XXXXXX").string();
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    ASSERT_NE(::mkdtemp(buf.data()), nullptr) << "could not create temp dir";
    tmp_ = fs::path{buf.data()};
    fs::create_directories(tmp_ / "root");
    std::ofstream{tmp_ / "root" / "notes.txt"} << "alpha\nbeta\n";

    auto box = Sandbox::open(tmp_ / "root");
    ASSERT_TRUE(box.has_value());
    sandbox_ = std::make_unique<Sandbox>(std::move(*box));

    auto tools = ToolSet::tier0(tmp_ / "backups");
    ASSERT_TRUE(tools.has_value());
    tools_ = std::make_unique<ToolSet>(std::move(*tools));
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_, ec);
  }

  std::optional<json> handle(const json& request) {
    return handle_message(tools_->registry(), *sandbox_, kName, kVersion, request);
  }

  fs::path tmp_;
  std::unique_ptr<Sandbox> sandbox_;
  std::unique_ptr<ToolSet> tools_;
};

}  // namespace

// --- initialize ----------------------------------------------------------------

TEST_F(McpFixture, InitializeReturnsServerInfo) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                    {"params", json{{"protocolVersion", "2025-06-18"}}}});
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ((*response)["id"], 1);
  EXPECT_EQ((*response)["result"]["serverInfo"]["name"], kName);
  EXPECT_EQ((*response)["result"]["serverInfo"]["version"], kVersion);
  EXPECT_TRUE((*response)["result"]["capabilities"].contains("tools"));
}

TEST_F(McpFixture, InitializeFallsBackWhenTheClientAsksForAnUnknownVersion) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                    {"params", json{{"protocolVersion", "1999-01-01"}}}});
  ASSERT_TRUE(response.has_value());
  EXPECT_NE((*response)["result"]["protocolVersion"], "1999-01-01");
}

// --- tools/list ------------------------------------------------------------------

TEST_F(McpFixture, ToolsListMatchesWireMcpToolDefinitions) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
  ASSERT_TRUE(response.has_value());
  const json expected = hermit::supervisor::mcp_tool_definitions(tools_->registry());
  EXPECT_EQ((*response)["result"]["tools"], expected);
}

TEST_F(McpFixture, ToolsListHasNoShellByDefault) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
  ASSERT_TRUE(response.has_value());
  for (const auto& tool : (*response)["result"]["tools"]) {
    EXPECT_NE(tool["name"], "shell");
  }
}

// --- tools/call ------------------------------------------------------------------

TEST_F(McpFixture, ToolsCallDispatchesAndWrapsContentAsText) {
  const auto response =
      handle(json{{"jsonrpc", "2.0"},
                  {"id", 3},
                  {"method", "tools/call"},
                  {"params", json{{"name", "read"}, {"arguments", json{{"paths", json::array({"notes.txt"})}}}}}});
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE((*response)["result"]["isError"].get<bool>());
  const std::string text = (*response)["result"]["content"][0]["text"].get<std::string>();
  const json rows = json::parse(text);
  EXPECT_EQ(rows[0]["content"], "alpha\nbeta\n");
}

TEST_F(McpFixture, ToolsCallTreatsExplicitNullArgumentsAsAbsent) {
  // Some clients serialize an omitted optional field as an explicit null rather than
  // dropping the key. A no-argument call is what that means here, not Invalid Params.
  const auto response = handle(json{{"jsonrpc", "2.0"},
                                    {"id", 10},
                                    {"method", "tools/call"},
                                    {"params", json{{"name", "does-not-exist"}, {"arguments", nullptr}}}});
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE((*response).contains("result"));
  EXPECT_FALSE((*response).contains("error"));
}

TEST_F(McpFixture, ToolsCallOnAnUnknownToolIsErrorNotAJsonRpcError) {
  const auto response = handle(json{{"jsonrpc", "2.0"},
                                    {"id", 4},
                                    {"method", "tools/call"},
                                    {"params", json{{"name", "does-not-exist"}}}});
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE((*response).contains("result"));
  EXPECT_FALSE((*response).contains("error"));
  EXPECT_TRUE((*response)["result"]["isError"].get<bool>());
}

TEST_F(McpFixture, ToolsCallWithMissingParamsIsInvalidParams) {
  const auto response =
      handle(json{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "tools/call"}});
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE((*response).contains("error"));
  EXPECT_EQ((*response)["error"]["code"], -32602);
}

TEST_F(McpFixture, ToolsCallWithNonStringNameIsInvalidParams) {
  const auto response = handle(json{{"jsonrpc", "2.0"},
                                    {"id", 6},
                                    {"method", "tools/call"},
                                    {"params", json{{"name", 42}}}});
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ((*response)["error"]["code"], -32602);
}

TEST_F(McpFixture, ToolsCallContentSurvivesInvalidUtf8) {
  // Mirrors WireEncoding's reason for existing: a read of a binary file must not throw
  // or produce unparseable JSON on the way back to the client.
  std::ofstream out(tmp_ / "root" / "blob.bin", std::ios::binary);
  out.put(static_cast<char>(0xFF));
  out.put(static_cast<char>(0xFE));
  out.close();

  std::optional<json> response;
  ASSERT_NO_THROW(response = handle(
                      json{{"jsonrpc", "2.0"},
                           {"id", 7},
                           {"method", "tools/call"},
                           {"params", json{{"name", "read"},
                                           {"arguments", json{{"paths", json::array({"blob.bin"})}}}}}}));
  ASSERT_TRUE(response.has_value());
  EXPECT_FALSE((*response)["result"]["isError"].get<bool>());
}

// --- protocol framing --------------------------------------------------------------

TEST_F(McpFixture, UnknownMethodIsMethodNotFound) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 8}, {"method", "nonexistent"}});
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ((*response)["error"]["code"], -32601);
}

TEST_F(McpFixture, NotificationsInitializedReturnsNoResponse) {
  const auto response =
      handle(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
  EXPECT_FALSE(response.has_value());
}

TEST_F(McpFixture, AnUnrecognizedNotificationAlsoReturnsNoResponse) {
  // notifications/cancelled and friends: Hermit's tool calls are synchronous filesystem
  // ops with nothing long-running to interrupt, so acknowledging and ignoring is correct.
  const auto response =
      handle(json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}});
  EXPECT_FALSE(response.has_value());
}

TEST_F(McpFixture, ARequestMissingMethodIsInvalidRequest) {
  const auto response = handle(json{{"jsonrpc", "2.0"}, {"id", 9}});
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ((*response)["error"]["code"], -32600);
  EXPECT_EQ((*response)["id"], 9);
}

TEST_F(McpFixture, ANonObjectMessageIsInvalidRequestWithNullId) {
  const auto response = handle(json::array({"not", "a", "request"}));
  ASSERT_TRUE(response.has_value());
  EXPECT_EQ((*response)["error"]["code"], -32600);
  EXPECT_TRUE((*response)["id"].is_null());
}
