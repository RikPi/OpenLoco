#include "Network/Network.h"
#include "CommandLine.h"
#include "Config.h"
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
#include <OpenLoco/Platform/Platform.h>
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
            dst.reserved = src.reserved ? 1 : 0;
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
            entry.reserved = src.reserved != 0;
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

    // --- Reconnect (docs/multiplayer.md § Reconnect) ------------------------
    //
    // NetworkClient is destroyed whenever its connection is lost (see close()
    // below) - it cannot remember anything across that destruction. The
    // {endpoint, token, attempt count} needed to retry therefore has to live
    // one level up, in this facade, which outlives any individual
    // NetworkClient instance. This is the "facade-owned retry state" referred
    // to in the design doc.
    //
    // _joinHost/_joinPort are the address most recently passed to
    // joinServer() - kept for the whole session (cleared only by a real
    // close()), since they identify "the server we're joined to" independent
    // of any particular reconnect episode.
    static std::string _joinHost;
    static port_t _joinPort{};

    constexpr int kMaxReconnectAttempts = 5;
    constexpr uint32_t kReconnectIntervalMs = 5000;

    struct ReconnectState
    {
        bool active{};             // a retry episode is in progress (the NetworkClient object is temporarily gone)
        uint64_t token{};          // session token to reclaim the same seat
        int attempts{};            // attempts already made this episode
        uint32_t nextAttemptTime{}; // Platform::getTime() of the next attempt
    };
    static ReconnectState _reconnect;

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

    // Ends the reconnect episode (successfully or not) and hands control back
    // to the ordinary "not networked" state, exactly as if the player had
    // manually disconnected - the existing return-to-title behaviour.
    static void giveUpReconnecting()
    {
        Logging::error("Reconnect failed after {} attempt(s); giving up", _reconnect.attempts);
        Ui::Windows::NetworkStatus::close();
        SceneManager::requestScene(SceneManager::SceneId::title);
        close();
    }

    // Shows/refreshes reconnect progress in the status window. A fresh
    // episode has no window open (gameplay was showing); a later attempt
    // during the same episode reuses whatever window is already up (e.g. one
    // left open by a prior failed attempt, or by an in-flight resync).
    static void showReconnectStatus(const std::string& text)
    {
        if (Ui::WindowManager::find(Ui::WindowType::networkStatus) != nullptr)
        {
            Ui::Windows::NetworkStatus::setText(text, &giveUpReconnecting);
        }
        else
        {
            Ui::Windows::NetworkStatus::open(text, &giveUpReconnecting);
        }
    }

    // Creates a brand new NetworkClient bound to the previous session token
    // and points it at the same endpoint. This is a genuinely fresh
    // connection attempt (new socket, new local port) - the server cannot
    // recognise it by endpoint, only by the token carried in its
    // ConnectPacket (see NetworkServer::createNewClient's reclaim branch).
    static void attemptReconnect()
    {
        _reconnect.attempts++;
        Logging::info("Reconnecting (attempt {})...", _reconnect.attempts);
        showReconnectStatus(fmt::format("Reconnecting (attempt {}/{})...", _reconnect.attempts, kMaxReconnectAttempts));

        try
        {
            auto client = std::make_unique<NetworkClient>();
            client->setReconnectToken(_reconnect.token);
            client->connect(_joinHost, _joinPort);
            _client = std::move(client);
            _mode = NetworkMode::client;
        }
        catch (...)
        {
            // Could not even start the attempt (e.g. socket creation
            // failure) - stay in the "no client" state and try again once
            // the interval elapses, same as a timed-out attempt would.
            _client = nullptr;
        }

        _reconnect.nextAttemptTime = Platform::getTime() + kReconnectIntervalMs;
    }

    void openServer()
    {
        assert(_mode == NetworkMode::none);

        try
        {
            // CLI --bind/--port win when supplied (unchanged CLI behaviour);
            // otherwise fall back to the config values so UI-initiated
            // hosting isn't stuck with the CLI-only defaults.
            const auto& cmdlineOptions = getCommandLineOptions();
            const auto& networkConfig = Config::get().network;
            const auto& bind = !cmdlineOptions.bind.empty() ? cmdlineOptions.bind : networkConfig.bind;
            auto port = cmdlineOptions.port.value_or(networkConfig.port != 0 ? networkConfig.port : kDefaultPort);

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

            // Remember the endpoint for a possible future auto-reconnect
            // (see tick()); this is a brand new session, so any leftover
            // reconnect bookkeeping from an earlier one is stale.
            _joinHost = std::string(host);
            _joinPort = port;
            _reconnect = {};
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
        _reconnect = {};
        _joinHost.clear();
        _joinPort = 0;
    }

    void tick()
    {
        auto serverOrClient = getServerOrClient();
        if (serverOrClient != nullptr)
        {
            serverOrClient->update();

            // A reconnect episode ends the moment the new connection is
            // fully re-established (fresh state transfer complete) - clear
            // the bookkeeping so a later, unrelated disconnect starts a new
            // episode at attempt 1 rather than continuing this one's count.
            if (_reconnect.active && _mode == NetworkMode::client && _client->getStatus() == NetworkClientStatus::connected)
            {
                Logging::info("Reconnected successfully");
                _reconnect = {};
            }

            if (serverOrClient->isClosed())
            {
                if (_mode == NetworkMode::client && _client->shouldAutoRetry())
                {
                    // Either the first time an established connection has
                    // timed out (not a failed initial connect, not a
                    // graceful server shutdown - see
                    // NetworkClient::shouldAutoRetry), or a previous retry
                    // attempt that itself failed to (re)connect. Capture the
                    // token before the object goes away, then destroy it -
                    // this is the one place a lost NetworkClient's identity
                    // must survive its own destruction, which is exactly
                    // what _reconnect exists for.
                    if (!_reconnect.active)
                    {
                        _reconnect.active = true;
                        _reconnect.attempts = 0;
                    }
                    _reconnect.token = _client->getToken();
                    _client = nullptr;
                    _mode = NetworkMode::none;

                    if (_reconnect.attempts >= kMaxReconnectAttempts)
                    {
                        giveUpReconnecting();
                    }
                    else
                    {
                        _reconnect.nextAttemptTime = Platform::getTime() + kReconnectIntervalMs;
                    }
                }
                else
                {
                    close();
                }
            }
        }
        else if (_reconnect.active && Platform::getTime() >= _reconnect.nextAttemptTime)
        {
            attemptReconnect();
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

    void requestAllClientsResync()
    {
        if (_mode == NetworkMode::server)
        {
            // The freshly loaded world defines a new human-company set:
            // just the host's company until clients re-join it
            CompanyManager::clearHumanCompanies();
            CompanyManager::markCompanyAsHuman(CompanyManager::getControllingId());

            _server->requestAllClientsResync();
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
