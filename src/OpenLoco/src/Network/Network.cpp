#include "Network/Network.h"
#include "CommandLine.h"
#include "Environment.h"
#include "GameCommands/CommandSerialization.h"
#include "GameCommands/GameCommands.h"
#include "GameState.h"
#include "Graphics/Gfx.h"
#include "Logging.h"
#include "Network/NetworkClient.h"
#include "Network/NetworkServer.h"
#include "Network/Socket.h"
#include "S5/S5.h"
#include "Scenario/ScenarioManager.h"
#include "SceneManager.h"
#include "Ui/WindowManager.h"
#include "World/CompanyManager.h"
#include <OpenLoco/Core/BinaryStream.h>
#include <OpenLoco/Core/FileStream.h>
#include <OpenLoco/Core/MemoryStream.h>
#include <OpenLoco/Utility/String.hpp>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fmt/format.h>
#include <stdexcept>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco::Network
{
    std::string resolveDisplayName(std::string_view raw, client_id_t id)
    {
        // A wire buffer or config value may be padded with trailing/embedded
        // NUL bytes; take only the part before the first one.
        auto nul = raw.find('\0');
        if (nul != std::string_view::npos)
        {
            raw = raw.substr(0, nul);
        }

        auto trimmed = Utility::trim(raw);
        if (trimmed.empty())
        {
            return fmt::format("Player #{}", id);
        }
        return std::string(trimmed);
    }

    bool toWirePacket(const std::vector<PlayerRosterEntry>& roster, RosterUpdatePacket& packet)
    {
        if (roster.size() > kMaxRosterEntries)
        {
            Logging::error("Player roster has {} entries, only {} fit in a RosterUpdatePacket; truncating", roster.size(), kMaxRosterEntries);
        }

        packet.count = static_cast<uint8_t>(std::min(roster.size(), kMaxRosterEntries));
        for (uint8_t i = 0; i < packet.count; i++)
        {
            const auto& src = roster[i];
            auto& dst = packet.entries[i];
            dst.id = src.id;
            dst.company = src.company;
            auto nameLength = std::min(src.name.size(), kMaxRosterNameLength);
            dst.nameLength = static_cast<uint8_t>(nameLength);
            std::memcpy(dst.name, src.name.data(), nameLength);
        }
        return roster.size() <= kMaxRosterEntries;
    }

    std::vector<PlayerRosterEntry> fromWirePacket(const RosterUpdatePacket& packet)
    {
        std::vector<PlayerRosterEntry> roster;
        auto count = std::min<uint8_t>(packet.count, kMaxRosterEntries);
        roster.reserve(count);
        for (uint8_t i = 0; i < count; i++)
        {
            const auto& src = packet.entries[i];
            PlayerRosterEntry entry;
            entry.id = src.id;
            entry.company = src.company;
            auto nameLength = std::min<size_t>(src.nameLength, kMaxRosterNameLength);
            entry.name.assign(src.name, nameLength);
            roster.push_back(std::move(entry));
        }
        return roster;
    }

    bool toWirePacket(const QueuedGameCommand& command, GameCommandPacket& packet)
    {
        MemoryStream ms;
        if (!GameCommands::encodeCommandArgs(command.command, command.regs, ms))
        {
            return false;
        }
        if (ms.getLength() > sizeof(packet.data))
        {
            Logging::error("Serialized game command {} is too large for a packet ({} bytes)", static_cast<uint32_t>(command.command), ms.getLength());
            return false;
        }

        packet.index = command.index;
        packet.tick = command.tick;
        packet.company = command.company;
        packet.flags = command.flags;
        packet.commandId = static_cast<uint8_t>(command.command);
        packet.dataSize = static_cast<uint16_t>(ms.getLength());
        std::memcpy(packet.data, ms.data(), ms.getLength());
        return true;
    }

    bool fromWirePacket(const GameCommandPacket& packet, QueuedGameCommand& command)
    {
        if (packet.dataSize > sizeof(packet.data))
        {
            Logging::error("Malformed game command packet: data size {} exceeds packet capacity", packet.dataSize);
            return false;
        }

        command.index = packet.index;
        command.tick = packet.tick;
        command.company = packet.company;
        command.flags = packet.flags;
        command.command = static_cast<GameCommands::GameCommand>(packet.commandId);

        BinaryStream bs(packet.data, packet.dataSize);
        try
        {
            if (!GameCommands::decodeCommandArgs(command.command, bs, command.regs))
            {
                return false;
            }
        }
        catch (const std::exception& e)
        {
            Logging::error("Malformed game command packet for command {}: {}", packet.commandId, e.what());
            return false;
        }

        // Reconstruct the dispatcher's view of command id and flags
        command.regs.esi = static_cast<int32_t>(command.command);
        command.regs.bl = command.flags;
        return true;
    }

    enum class NetworkMode
    {
        none,
        server,
        client
    };

    static NetworkMode _mode;
    static std::unique_ptr<NetworkServer> _server;
    static std::unique_ptr<NetworkClient> _client;

    static NetworkBase* getServerOrClient()
    {
        switch (_mode)
        {
            case NetworkMode::server:
                return _server.get();
            case NetworkMode::client:
                return _client.get();
            default:
                return nullptr;
        }
    }

    void openServer()
    {
        assert(_mode == NetworkMode::none);

        try
        {
            const auto& cmdlineOptions = getCommandLineOptions();
            auto& bind = cmdlineOptions.bind;
            auto port = cmdlineOptions.port.value_or(kDefaultPort);

            _server = std::make_unique<NetworkServer>();
            _server->listen(bind, port);

            _mode = NetworkMode::server;
            SceneManager::addSceneFlags(SceneManager::Flags::networked);
            SceneManager::addSceneFlags(SceneManager::Flags::networkHost);

            // The host is always a human company; record it in the
            // deterministic human-company set alongside the vanilla
            // playerCompanies[] membership so both mechanisms agree.
            CompanyManager::markCompanyAsHuman(CompanyManager::getControllingId());

            Gfx::invalidateScreen();
        }
        catch (...)
        {
            _server = nullptr;
            throw;
        }
    }

    bool joinServer(std::string_view host)
    {
        return joinServer(host, kDefaultPort);
    }

    bool joinServer(std::string_view host, port_t port)
    {
        assert(_mode == NetworkMode::none);

        try
        {
            _client = std::make_unique<NetworkClient>();
            _client->connect(host, port);
            _mode = NetworkMode::client;
            return true;
        }
        catch (...)
        {
            _client = nullptr;
            throw;
        }
    }

    void close()
    {
        _server = nullptr;
        _client = nullptr;
        _mode = NetworkMode::none;
    }

    void tick()
    {
        auto serverOrClient = getServerOrClient();
        if (serverOrClient != nullptr)
        {
            serverOrClient->update();
            if (serverOrClient->isClosed())
            {
                close();
            }
        }
    }

    void sendChatMessage(std::string_view message)
    {
        auto serverOrClient = getServerOrClient();
        if (serverOrClient != nullptr)
        {
            serverOrClient->sendChatMessage(message);
        }
    }

    std::vector<PlayerRosterEntry> getPlayerRoster()
    {
        switch (_mode)
        {
            case NetworkMode::server:
                return _server->buildRoster();
            case NetworkMode::client:
                return _client->getRoster();
            default:
                return {};
        }
    }

    static std::string resolveChatSenderName(client_id_t client)
    {
        auto roster = getPlayerRoster();
        for (const auto& entry : roster)
        {
            if (entry.id == client)
            {
                return entry.name;
            }
        }
        // Roster not available yet (e.g. very first messages before a
        // RosterUpdatePacket has arrived) - fall back to the old behaviour.
        return fmt::format("Player #{}", client);
    }

    void receiveChatMessage(client_id_t client, std::string_view message)
    {
        auto senderName = resolveChatSenderName(client);
        Logging::info("{}: {}", senderName, message);
        Ui::Windows::Chat::addMessage(senderName, message);
    }

    void queueGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags)
    {
        if (_mode == NetworkMode::server)
        {
            _server->queueGameCommand(company, regs, flags);
        }
        else
        {
            _client->sendGameCommand(company, regs, flags);
        }
    }

    bool shouldProcessTick(uint32_t tick)
    {
        if (_mode == NetworkMode::client)
        {
            return _client->shouldProcessTick(tick);
        }
        else
        {
            return true;
        }
    }

    void processGameCommands(uint32_t tick)
    {
        switch (_mode)
        {
            case NetworkMode::none:
                break;
            case NetworkMode::server:
                _server->runGameCommands();
                break;
            case NetworkMode::client:
                _client->runGameCommandsForTick(tick);
                break;
        }
    }

    void onTickProcessed(uint32_t tick)
    {
        if (_mode == NetworkMode::client)
        {
            _client->onTickProcessed(tick);
        }
    }

    void saveDesyncDump(std::string_view role, uint32_t mismatchTick, uint32_t currentTick)
    {
        try
        {
            auto directory = Environment::getPath(Environment::PathId::save) / "desync";
            Environment::autoCreateDirectory(directory);

            auto path = directory / fmt::format("desync_{}_tick{}_at{}.sv5", role, mismatchTick, currentTick);
            FileStream fs(path, StreamMode::write);
            S5::exportGameStateToFile(fs, S5::SaveFlags::noWindowClose);

            auto path8 = path.u8string();
            Logging::info("Saved desync dump to {}", path8.c_str());
        }
        catch (const std::exception& e)
        {
            Logging::error("Unable to save desync dump: {}", e.what());
        }
    }

    bool isConnected()
    {
        switch (_mode)
        {
            default:
            case NetworkMode::none:
                return false;
            case NetworkMode::server:
                return true;
            case NetworkMode::client:
                return _client->getStatus() == NetworkClientStatus::connected;
        }
    }

    uint32_t getServerTick()
    {
        if (_mode == NetworkMode::client)
        {
            return _client->getLocalTick();
        }
        return ScenarioManager::getScenarioTicks();
    }
}
