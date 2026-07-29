#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
    };

    std::optional<CommandLineOptions> parseCommandLine(std::vector<std::string>&& argv);
    const CommandLineOptions& getCommandLineOptions();
    void setCommandLineOptions(const CommandLineOptions& options);
}
