#include "Network/NetworkClient.h"
#include "Config.h"
#include "GameCommands/GameCommands.h"
#include "GameState.h"
#include "Graphics/Gfx.h"
#include "Logging.h"
#include "Network/NetworkConnection.h"
#include "S5/S5.h"
#include "SceneManager.h"
#include "Ui/WindowManager.h"
#include "World/CompanyManager.h"
#include <OpenLoco/Core/BinaryStream.h>
#include <OpenLoco/Platform/Platform.h>
#include <algorithm>
#include <cstring>

using namespace OpenLoco;
using namespace OpenLoco::Network;
using namespace OpenLoco::Diagnostics;

NetworkClient::~NetworkClient()
{
    close();
}

NetworkClientStatus NetworkClient::getStatus() const
{
    return _status;
}

uint32_t NetworkClient::getLocalTick() const
{
    return _localTick;
}

void NetworkClient::connect(std::string_view host, port_t port)
{
    auto szHost = std::string(host);
    _serverEndpoint = Socket::resolve(Protocol::any, szHost, port);

    _sockets.push_back(Socket::createUdp());
    auto& socket = _sockets.back();
    _serverConnection = std::make_unique<NetworkConnection>(socket.get(), _serverEndpoint->clone());

    auto szHostIpAddress = _serverEndpoint->getIpAddress();
    Logging::info("Resolved endpoint for {}:{}", szHostIpAddress, port);

    beginReceivePacketLoop();

    _status = NetworkClientStatus::connecting;
    _timeout = Platform::getTime() + 5000;

    sendConnectPacket();

    initStatus("Connecting to " + szHost + "...");
}

void NetworkClient::onClose()
{
    _serverConnection = nullptr;
    if (_status != NetworkClientStatus::none && _status != NetworkClientStatus::connecting)
    {
        _status = NetworkClientStatus::closed;
        SceneManager::removeSceneFlags(SceneManager::Flags::networked);
        Logging::info("Disconnected from server");
    }
    else if (_status == NetworkClientStatus::connecting)
    {
        endStatus("Failed to connect to server");
        _status = NetworkClientStatus::closed;
    }
}

void NetworkClient::onUpdate()
{
    processReceivedPackets();
    if (_status == NetworkClientStatus::connecting)
    {
        if (Platform::getTime() >= _timeout)
        {
            close();
            Logging::info("Failed to connect to server");
            endStatus("Failed to connect to server");
        }
    }
    else
    {
        if (hasTimedOut())
        {
            Logging::info("Connection with server timed out");
            close();
        }
        else
        {
            switch (_status)
            {
                case NetworkClientStatus::connectedSuccessfully:
                    sendRequestStatePacket();
                    _status = NetworkClientStatus::waitingForState;
                    break;
                case NetworkClientStatus::waitingForState:
                    break;
                default:
                    break;
            }
        }
    }
}

void NetworkClient::processReceivedPackets()
{
    if (_serverConnection != nullptr)
    {
        while (auto packet = _serverConnection->takeNextPacket())
        {
            onReceivePacketFromServer(*packet);

            // A packet handler may close the connection
            if (_serverConnection == nullptr)
            {
                return;
            }
        }
        _serverConnection->update();
    }
}

bool NetworkClient::hasTimedOut() const
{
    if (_serverConnection != nullptr)
    {
        return _serverConnection->hasTimedOut();
    }
    return false;
}

void NetworkClient::onReceivePacket([[maybe_unused]] IUdpSocket& socket, std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet)
{
    // TODO do we really need the check, it is possible but unlikely
    //      for something else to hijack the UDP client port
    if (_serverEndpoint != nullptr && endpoint->equals(*_serverEndpoint))
    {
        _serverConnection->receivePacket(packet);
    }
}

void NetworkClient::onCancel()
{
    switch (_status)
    {
        case NetworkClientStatus::connecting:
            Logging::info("Connecting to server cancelled");
            close();
            break;
        default:
            break;
    }
}

void NetworkClient::onReceivePacketFromServer(const Packet& packet)
{
    switch (packet.header.kind)
    {
        case PacketKind::connectResponse:
            receiveConnectionResponsePacket(*reinterpret_cast<const ConnectResponsePacket*>(packet.data));
            break;
        case PacketKind::requestStateResponse:
            receiveRequestStateResponsePacket(*reinterpret_cast<const RequestStateResponse*>(packet.data));
            break;
        case PacketKind::requestStateResponseChunk:
            receiveRequestStateResponseChunkPacket(*reinterpret_cast<const RequestStateResponseChunk*>(packet.data));
            break;
        case PacketKind::receiveChatMessage:
            receiveChatMessagePacket(*reinterpret_cast<const ReceiveChatMessage*>(packet.data));
            break;
        case PacketKind::ping:
            receivePingPacket(*reinterpret_cast<const PingPacket*>(packet.data));
            break;
        case PacketKind::gameCommand:
            receiveGameCommandPacket(*reinterpret_cast<const GameCommandPacket*>(packet.data));
            break;
        case PacketKind::companyAssignment:
            receiveCompanyAssignmentPacket(*reinterpret_cast<const CompanyAssignmentPacket*>(packet.data));
            break;
        default:
            break;
    }
}

void NetworkClient::sendConnectPacket()
{
    const auto& config = Config::get();
    ConnectPacket packet;
    std::strncpy(packet.name, config.preferredOwnerName.c_str(), sizeof(packet.name));
    packet.version = kNetworkVersion;
    _serverConnection->sendPacket(packet);
}

void NetworkClient::sendRequestStatePacket()
{
    _requestStateCookie = (std::rand() << 16) | std::rand();
    _requestStateChunksReceived.clear();

    RequestStatePacket packet;
    packet.cookie = _requestStateCookie;
    _serverConnection->sendPacket(packet);
}

void NetworkClient::receiveConnectionResponsePacket(const ConnectResponsePacket& response)
{
    if (response.result == ConnectionResult::success)
    {
        _status = NetworkClientStatus::connectedSuccessfully;
        setStatus("Connected to server successfully");
        SceneManager::addSceneFlags(SceneManager::Flags::networked);
    }
    else
    {
        auto message = std::string_view(response.message, strnlen(response.message, sizeof(response.message)));
        Logging::error("Server rejected connection: {}", message);
        endStatus(message.empty() ? "Server rejected connection" : std::string(message));
        // Skip the generic "failed to connect" status from the connecting-state
        // close path; the rejection reason above is more useful
        _status = NetworkClientStatus::closed;
        close();
    }
}

void NetworkClient::receiveRequestStateResponsePacket(const RequestStateResponse& response)
{
    if (response.cookie == _requestStateCookie)
    {
        _requestStateNumChunks = response.numChunks;
        _requestStateTotalSize = response.totalSize;
        _requestStateReceivedChunks = 0;
    }
}

void NetworkClient::receiveRequestStateResponseChunkPacket(const RequestStateResponseChunk& responseChunk)
{
    if (responseChunk.cookie == _requestStateCookie)
    {
        if (_requestStateChunksReceived.size() <= responseChunk.index)
        {
            _requestStateChunksReceived.resize(responseChunk.index + 1);
        }

        auto& rchunk = _requestStateChunksReceived[responseChunk.index];
        if (rchunk.data.size() == 0)
        {
            rchunk.offset = responseChunk.offset;
            rchunk.data.assign(responseChunk.data, responseChunk.data + responseChunk.dataSize);
            _requestStateReceivedChunks++;

            _requestStateReceivedBytes += responseChunk.dataSize;
            setStatus("Receiving state: " + std::to_string(_requestStateReceivedBytes) + " / " + std::to_string(_requestStateTotalSize));
        }

        if (_requestStateReceivedChunks >= _requestStateNumChunks)
        {
            // Construct full data
            std::vector<uint8_t> fullData;
            for (size_t i = 0; i < _requestStateChunksReceived.size(); i++)
            {
                fullData.insert(fullData.end(), _requestStateChunksReceived[i].data.begin(), _requestStateChunksReceived[i].data.end());
            }

            clearStatus();
            _status = NetworkClientStatus::connected;

            processFullState(fullData);
        }
    }
}

void NetworkClient::processFullState(std::span<uint8_t const> fullData)
{
    auto* extra = reinterpret_cast<const ExtraState*>(fullData.data() + fullData.size() - sizeof(ExtraState));
    _localGameCommandIndex = extra->gameCommandIndex;
    _localTick = extra->tick;
    CompanyManager::setHumanCompanyMask(extra->humanCompanyMask);

    // Any commands received while the state transfer was in flight that are
    // already part of the snapshot must not be executed again. Recorded RNG
    // history predating the snapshot is likewise meaningless now.
    _receivedGameCommands.remove_if([this](const QueuedGameCommand& c) { return c.index <= _localGameCommandIndex; });
    _tickRngHistory.clear();
    _pendingServerStates.clear();

    updateLocalTick();

    BinaryStream bs(fullData.data(), fullData.size() - sizeof(ExtraState));
    if (S5::importSaveToGameState(bs, S5::LoadFlags::none))
    {
        SceneManager::requestScene(SceneManager::SceneId::gameplay);
    }
}

void NetworkClient::receiveChatMessagePacket(const ReceiveChatMessage& packet)
{
    Network::receiveChatMessage(packet.sender, packet.getText());
}

void NetworkClient::receiveCompanyAssignmentPacket(const CompanyAssignmentPacket& packet)
{
    if (packet.company == CompanyId::null)
    {
        Logging::info("Server did not assign a company; remaining a spectator");
        return;
    }

    // "Which company is mine" is per-machine (see KNOWLEDGEBASE.md): the
    // snapshot we just imported carries the host's playerCompanies[], and
    // overriding it locally here is correct and expected.
    CompanyManager::setControllingId(packet.company);
    CompanyManager::setSecondaryPlayerId(CompanyId::null);
    Logging::info("Assigned company {}", static_cast<uint32_t>(packet.company));
    Gfx::invalidateScreen();
}

void NetworkClient::receivePingPacket(const PingPacket& packet)
{
    if (_status != NetworkClientStatus::connected)
    {
        return;
    }

    // Update the latest knowledge of server state
    _serverTick = std::max(_serverTick, packet.tick);
    _serverGameCommandIndex = std::max(_serverGameCommandIndex, packet.gameCommandIndex);

    if (_localGameCommandIndex == _serverGameCommandIndex)
    {
        // No pending game commands, we can update to this tick
        _localTick = packet.tick;
    }

    // Queue the server's advertised PRNG state for verification once we have
    // simulated the tick it refers to
    _pendingServerStates.push_back(packet);
    checkForDesync();
}

void NetworkClient::onTickProcessed(uint32_t tick)
{
    if (_status != NetworkClientStatus::connected)
    {
        return;
    }

    // The server samples its PRNG state between ticks, so the state recorded
    // here (after the tick has fully simulated) is directly comparable
    constexpr size_t kMaxTickRngHistory = 4096;

    auto& gameState = getGameState();
    _tickRngHistory.push_back({ tick, gameState.rng.srand_0(), gameState.rng.srand_1() });
    while (_tickRngHistory.size() > kMaxTickRngHistory)
    {
        _tickRngHistory.pop_front();
    }

    checkForDesync();
}

void NetworkClient::checkForDesync()
{
    while (!_pendingServerStates.empty())
    {
        const auto& serverState = _pendingServerStates.front();
        if (_tickRngHistory.empty() || serverState.tick > _tickRngHistory.back().tick)
        {
            // We have not simulated this tick yet; verify once we have
            break;
        }

        // History is ordered by tick
        auto it = std::lower_bound(
            _tickRngHistory.begin(),
            _tickRngHistory.end(),
            serverState.tick,
            [](const TickRngState& entry, uint32_t tick) { return entry.tick < tick; });
        if (it != _tickRngHistory.end() && it->tick == serverState.tick)
        {
            if (it->srand0 != serverState.srand0 || it->srand1 != serverState.srand1)
            {
                onDesyncDetected(serverState, *it);
                return;
            }

            // Verified in sync at this tick; older history is no longer needed
            _tickRngHistory.erase(_tickRngHistory.begin(), it);
        }

        _pendingServerStates.pop_front();
    }
}

void NetworkClient::onDesyncDetected(const PingPacket& serverState, const TickRngState& localState)
{
    Logging::error(
        "Desync detected at tick {}: server rng = {:08X}/{:08X}, local rng = {:08X}/{:08X}",
        serverState.tick,
        serverState.srand0,
        serverState.srand1,
        localState.srand0,
        localState.srand1);

    auto currentTick = _tickRngHistory.empty() ? serverState.tick : _tickRngHistory.back().tick;
    Network::saveDesyncDump("client", serverState.tick, currentTick);

    // Tell the server so it can dump its state for offline comparison
    if (_serverConnection != nullptr)
    {
        DesyncReportPacket report;
        report.tick = serverState.tick;
        report.localTick = currentTick;
        report.srand0 = localState.srand0;
        report.srand1 = localState.srand1;
        _serverConnection->sendPacket(report);
    }

    beginResync();
}

void NetworkClient::beginResync()
{
    Logging::info("Requesting full state resync from server");

    _status = NetworkClientStatus::resyncing;
    _receivedGameCommands.clear();
    _tickRngHistory.clear();
    _pendingServerStates.clear();

    initStatus("Desync detected, resyncing with server...");
    sendRequestStatePacket();
}

void NetworkClient::receiveGameCommandPacket(const GameCommandPacket& packet)
{
    QueuedGameCommand command;
    if (!fromWirePacket(packet, command))
    {
        Logging::error("Dropping malformed game command packet from server");
        return;
    }

    // Update the latest knowledge of server state
    _serverTick = std::max(_serverTick, command.tick);
    _serverGameCommandIndex = std::max(_serverGameCommandIndex, command.index);

    // Catch old or repeated game command index
    assert(command.index > _localGameCommandIndex);

    // Insert into ordered game command queue
    for (auto it = _receivedGameCommands.begin(); it != _receivedGameCommands.end(); it++)
    {
        auto& p = *it;

        // Catch duplicate game command index
        assert(command.index != p.index);

        if (command.index <= p.index)
        {
            _receivedGameCommands.insert(it, command);
            return;
        }
    }
    _receivedGameCommands.push_back(command);

    updateLocalTick();
}

void NetworkClient::sendChatMessage(std::string_view message)
{
    if (_serverConnection != nullptr)
    {
        SendChatMessage packet;
        packet.length = static_cast<uint16_t>(message.size() + 1);
        std::memcpy(packet.text, message.data(), message.size());
        _serverConnection->sendPacket(packet);
    }
}

void NetworkClient::sendGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags)
{
    if (_serverConnection != nullptr && _status == NetworkClientStatus::connected)
    {
        QueuedGameCommand command;
        command.company = company;
        command.flags = flags;
        command.command = static_cast<GameCommands::GameCommand>(regs.esi);
        command.regs = regs;

        GameCommandPacket packet;
        if (!toWirePacket(command, packet))
        {
            Logging::error("Unable to serialize game command {} for sending", static_cast<uint32_t>(command.command));
            return;
        }
        _serverConnection->sendPacket(packet);
    }
}

void NetworkClient::updateLocalTick()
{
    // If we have the next game command, we can set local tick to the tick for that command
    if (!_receivedGameCommands.empty())
    {
        auto& nextGameCommand = _receivedGameCommands.front();
        if (nextGameCommand.index == _localGameCommandIndex + 1)
        {
            // We already have the next game command, so we can update to the tick for that command
            _localTick = nextGameCommand.tick;
        }
    }
}

bool NetworkClient::shouldProcessTick(uint32_t tick) const
{
    if (_status == NetworkClientStatus::resyncing)
    {
        // Freeze the simulation until the fresh state from the server has
        // been applied; simulating further would only diverge more
        return false;
    }

    if (_status != NetworkClientStatus::connected)
    {
        return true;
    }

    return _localTick >= tick;
}

void NetworkClient::runGameCommandsForTick(uint32_t tick)
{
    if (_status != NetworkClientStatus::connected)
    {
        return;
    }

    // Execute all following commands if previously received
    while (!_receivedGameCommands.empty())
    {
        auto& nextCommand = _receivedGameCommands.front();
        if (nextCommand.index == _localGameCommandIndex + 1 && nextCommand.tick == tick)
        {
            _localGameCommandIndex++;
            GameCommands::doCommandForReal(nextCommand.command, nextCommand.company, nextCommand.regs, nextCommand.flags);
            _receivedGameCommands.pop_front();
        }
        else
        {
            break;
        }
    }

    updateLocalTick();
}

void NetworkClient::initStatus(std::string_view text)
{
    Ui::Windows::NetworkStatus::open(text, [this]() { onCancel(); });
}

void NetworkClient::setStatus(std::string_view text)
{
    Ui::Windows::NetworkStatus::setText(text);
}

void NetworkClient::endStatus(std::string_view text)
{
    Ui::Windows::NetworkStatus::setText(text, nullptr);
}

void NetworkClient::clearStatus()
{
    Ui::Windows::NetworkStatus::close();
}
