#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace OpenLoco
{
    enum class CommandLineAction
    {
        none,
        host,
        join,
        uncompress,
        simulate,
        compare,
        gensave,
        help,
        version,
        intro,
    };

    // What a hosting server does with joining players (session model Phase C)
    enum class JoinPolicy : uint8_t
    {
        ownCompany, // each joining player gets a freshly created company (competitive)
        coop,       // joining players share the host's company
        spectator,  // joining players can only watch (and chat)
    };

    struct CommandLineOptions
    {
        CommandLineAction action = CommandLineAction::none;
        std::string address;
        std::string path;
        std::string path2;
        std::optional<int32_t> ticks;
        std::string outputPath;
        std::string bind;
        std::optional<uint16_t> port{};
        std::string logLevels;
        std::string all;
        std::optional<std::string> locomotionDataPath{};
        std::optional<uint32_t> seed{};
        bool headless{};
        JoinPolicy joinPolicy = JoinPolicy::ownCompany;
        // Hidden headless test hook (deliberately omitted from --help): when
        // set on a joining client, drives a self-verifying client-issued
        // game command round-trip through the lockstep pipeline once the
        // client is assigned a real company. See NetworkClient's test-rename
        // hook and KNOWLEDGEBASE.md § Client round-trip test hook.
        std::optional<std::string> testRename{};
        // Hidden headless test hook (deliberately omitted from --help): on
        // the host, after <seconds> of server uptime and at least one
        // client's join assignment resolved, programmatically reloads the
        // same save the host was started with (the `host <path>` CLI path,
        // via `options.path`) and requests a full resync of every client --
        // the same effect as the file-browse-driven mid-session Load flow in
        // Game::loadGame, without the dialog. See NetworkServer's
        // test-host-load hook and KNOWLEDGEBASE.md § Host-driven mid-session
        // load test hook.
        std::optional<int32_t> testHostLoad{};
        // Hidden headless test hook (deliberately omitted from --help): on
        // the host, after <seconds> of server uptime, gracefully closes the
        // server (sends ServerClosingPacket, tears down sockets) and keeps
        // running as a single-player session afterwards -- exercises a
        // client's reaction to a clean shutdown under --headless, which has
        // no UI quit flow to trigger it otherwise. See NetworkServer's
        // test-shutdown hook and KNOWLEDGEBASE.md § Graceful shutdown test
        // hook.
        std::optional<int32_t> testShutdownAfter{};
        // Hidden headless test hook (deliberately omitted from --help),
        // client-side, formatted "<start>,<duration>" (seconds): makes
        // NetworkClient::onReceivePacket silently discard every incoming
        // packet for <duration> seconds starting <start> seconds after this
        // process's first connect() call, simulating a genuine network
        // outage (both this client and the server independently time each
        // other out via their normal 15s connection timeout) rather than a
        // scripted fake disconnect. Drives the reconnect smoke test. See
        // KNOWLEDGEBASE.md § Reconnect test hook.
        std::optional<std::pair<int32_t, int32_t>> testBlackhole{};
    };

    std::optional<CommandLineOptions> parseCommandLine(std::vector<std::string>&& argv);
    const CommandLineOptions& getCommandLineOptions();
    void setCommandLineOptions(const CommandLineOptions& options);
}
