#include "Network/NetworkServer.h"
#include "CommandLine.h"
#include "Config.h"
#include "GameCommands/GameCommands.h"
#include "GameState.h"
#include "Logging.h"
#include "Network/NetworkConnection.h"
#include "S5/S5.h"
#include "Scenario/ScenarioManager.h"
#include "SceneManager.h"
#include "World/CompanyManager.h"
#include <OpenLoco/Core/Exception.hpp>
#include <OpenLoco/Core/FileSystem.hpp>
#include <OpenLoco/Core/MemoryStream.h>
#include <OpenLoco/Platform/Platform.h>
#include <algorithm>
#include <span>

using namespace OpenLoco;
using namespace OpenLoco::Network;
using namespace OpenLoco::Diagnostics;

constexpr uint32_t kPingInterval = 30;

NetworkServer::~NetworkServer()
{
    close();
}

void NetworkServer::listen(const std::string& bind, port_t port)
{
    // IPv4
    try
    {
        auto socket4 = Socket::createUdp();
        socket4->listen(Protocol::ipv4, bind, port);
        _sockets.push_back(std::move(socket4));
    }
    catch (...)
    {
    }

    // IPv6
    try
    {
        auto socket6 = Socket::createUdp();
        socket6->listen(Protocol::ipv6, bind, port);
        _sockets.push_back(std::move(socket6));
    }
    catch (...)
    {
    }

    if (_sockets.empty())
    {
        throw Exception::RuntimeError("Unable to listen on " + bind + ", port " + std::to_string(port));
    }

    beginReceivePacketLoop();

    SceneManager::addSceneFlags(SceneManager::Flags::networked);
    SceneManager::addSceneFlags(SceneManager::Flags::networkHost);

    _startTime = Platform::getTime();

    Logging::info("Server opened");
    for (const auto& socket : _sockets)
    {
        auto ipAddress = socket->getIpAddress();
        if (socket->getProtocol() == Protocol::ipv6)
        {
            ipAddress = '[' + ipAddress + ']';
        }
        Logging::info("Listening for incoming connections on {}:{}...", ipAddress.c_str(), port);
    }
}

void NetworkServer::onClose()
{
    // Tell every still-connected client we're shutting down before the
    // sockets go away, so they can disconnect gracefully instead of timing
    // out. sendPacket writes to the socket synchronously, so this does not
    // depend on the receive thread (already stopped by the time onClose runs).
    if (!_clients.empty())
    {
        ServerClosingPacket packet;
        sendPacketToAll(packet);
    }

    SceneManager::removeSceneFlags(SceneManager::Flags::networked);
    SceneManager::removeSceneFlags(SceneManager::Flags::networkHost);
    Logging::info("Server closed");
}

Client* NetworkServer::findClient(const INetworkEndpoint& endpoint)
{
    for (auto& client : _clients)
    {
        if (client->connection->getEndpoint().equals(endpoint))
        {
            return client.get();
        }
    }
    return nullptr;
}

Client* NetworkServer::findClient(client_id_t id)
{
    for (auto& client : _clients)
    {
        if (client->id == id)
        {
            return client.get();
        }
    }
    return nullptr;
}

void NetworkServer::createNewClient(std::unique_ptr<NetworkConnection> conn, const ConnectPacket& packet)
{
    if (packet.version != kNetworkVersion)
    {
        Logging::info(
            "Rejecting client '{}': network version mismatch (client {}, server {})",
            resolveDisplayName(std::string_view(packet.name, sizeof(packet.name)), 0),
            packet.version,
            kNetworkVersion);

        ConnectResponsePacket response;
        response.result = ConnectionResult::error;
        std::snprintf(response.message, sizeof(response.message), "Network version mismatch (server is on version %u)", kNetworkVersion);
        conn->sendPacket(response);
        return;
    }

    auto newClient = std::make_unique<Client>();
    newClient->id = _nextClientId++;
    newClient->connection = std::move(conn);
    // Trim whitespace/NUL padding from the connect packet's fixed-size name
    // buffer; falls back to "Player #<id>" if it's empty after trimming
    // (fixes blank-padding in "Accepted new client"/assignment log lines).
    newClient->name = resolveDisplayName(std::string_view(packet.name, sizeof(packet.name)), newClient->id);
    _clients.push_back(std::move(newClient));

    auto& newClientPtr = *_clients.back();

    ConnectResponsePacket response;
    response.result = ConnectionResult::success;
    newClientPtr.connection->sendPacket(response);

    Logging::info("Accepted new client: {}", newClientPtr.name);

    broadcastRosterUpdate();
}

void NetworkServer::onReceivePacket(IUdpSocket& socket, std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet)
{
    auto client = findClient(*endpoint);
    if (client == nullptr)
    {
        auto connectPacket = packet.as<PacketKind::connect, ConnectPacket>();
        if (connectPacket != nullptr)
        {
            auto conn = std::make_unique<NetworkConnection>(&socket, std::move(endpoint));
            conn->receivePacket(packet);

            std::unique_lock<std::mutex> lk(_incomingConnectionsSync);
            _incomingConnections.push_back(std::move(conn));
        }
    }
    else
    {
        client->connection->receivePacket(packet);
    }
}

void NetworkServer::onReceivePacketFromClient(Client& client, const Packet& packet)
{
    switch (packet.header.kind)
    {
        case PacketKind::requestState:
            onReceiveStateRequestPacket(client, *packet.cast<RequestStatePacket>());
            break;
        case PacketKind::sendChatMessage:
            onReceiveSendChatMessagePacket(client, *packet.cast<SendChatMessage>());
            break;
        case PacketKind::gameCommand:
            onReceiveGameCommandPacket(client, *packet.cast<GameCommandPacket>());
            break;
        case PacketKind::desyncReport:
            onReceiveDesyncReportPacket(client, *packet.cast<DesyncReportPacket>());
            break;
        default:
            break;
    }
}

void NetworkServer::onReceiveStateRequestPacket(Client& client, const RequestStatePacket& request)
{
    constexpr uint16_t kChunkSize = 4000;

    // Dump S5 data to stream
    MemoryStream ms;
    S5::exportGameStateToFile(ms, S5::SaveFlags::noWindowClose);

    // Append extra state
    ExtraState extra;
    extra.gameCommandIndex = _gameCommandIndex;
    extra.tick = ScenarioManager::getScenarioTicks();
    extra.humanCompanyMask = CompanyManager::getHumanCompanyMask();
    ms.write(&extra, sizeof(extra));

    RequestStateResponse response;
    response.cookie = request.cookie;
    response.totalSize = static_cast<uint32_t>(ms.getLength());
    response.numChunks = static_cast<uint16_t>((ms.getLength() + (kChunkSize - 1)) / kChunkSize);
    client.connection->sendPacket(response);

    uint32_t offset = 0;
    uint32_t remaining = response.totalSize;
    uint16_t index = 0;
    while (index < response.numChunks)
    {
        RequestStateResponseChunk chunk;
        chunk.cookie = request.cookie;
        chunk.index = index;
        chunk.offset = offset;
        chunk.dataSize = std::min<uint32_t>(kChunkSize, remaining - offset);
        std::memcpy(chunk.data, reinterpret_cast<const uint8_t*>(ms.data()) + offset, chunk.dataSize);

        client.connection->sendPacket(chunk);

        offset += chunk.dataSize;
        index++;
    }

    // The client now has the full snapshot in flight. If its join assignment
    // was already resolved (this is a resync after a desync, not a first
    // join), only re-send the existing assignment so the client can restore
    // its controlling id after applying the fresh snapshot - running the
    // assignment again would create another company.
    if (client.assignmentResolved)
    {
        CompanyAssignmentPacket packet;
        packet.company = client.company;
        client.connection->sendPacket(packet);
        return;
    }

    // First join: resolve the assignment according to the host's policy.
    switch (getCommandLineOptions().joinPolicy)
    {
        case JoinPolicy::coop:
        {
            // Share the host's company; no new company, no game command
            auto hostCompany = CompanyManager::getControllingId();
            if (hostCompany != CompanyId::null)
            {
                client.company = hostCompany;
                Logging::info("Assigned host company {} to client '{}' (coop)", static_cast<uint32_t>(hostCompany), client.name);
            }
            else
            {
                Logging::info("Host has no company; client '{}' joins as spectator", client.name);
            }

            client.assignmentResolved = true;
            CompanyAssignmentPacket packet;
            packet.company = client.company;
            client.connection->sendPacket(packet);
            broadcastRosterUpdate();
            break;
        }
        case JoinPolicy::spectator:
        {
            Logging::info("Client '{}' joins as spectator (host join policy)", client.name);
            client.assignmentResolved = true;
            CompanyAssignmentPacket packet;
            packet.company = CompanyId::null;
            client.connection->sendPacket(packet);
            broadcastRosterUpdate();
            break;
        }
        case JoinPolicy::ownCompany:
        default:
        {
            // Queue a replicated company creation, tagged with the client's
            // id (not put on the wire - see ServerQueuedGameCommand) so
            // runGameCommands() can route the result back to this client
            // once the command has executed. CompanyId::null is used as the
            // acting company: createPlayerCompany doesn't check company
            // compatibility, it just allocates a fresh slot.
            GameCommands::registers regs;
            regs.esi = static_cast<int32_t>(GameCommands::GameCommand::createPlayerCompany);
            regs.bl = GameCommands::Flags::apply;
            queueGameCommand(CompanyId::null, regs, GameCommands::Flags::apply, client.id);
            break;
        }
    }
}

void NetworkServer::requestAllClientsResync()
{
    for (auto& client : _clients)
    {
        // The old world's assignments are meaningless in the newly loaded
        // one; clients get fresh assignments during their resync. Until
        // then they are spectators (their commands are dropped).
        client->company = CompanyId::null;
        client->assignmentResolved = false;

        ResyncRequiredPacket packet;
        client->connection->sendPacket(packet);
    }

    if (!_clients.empty())
    {
        Logging::info("Requested resync from {} client(s) after state reload", _clients.size());
        broadcastRosterUpdate();
    }
}

// Headless test hook (--test_host_load <seconds>). Driven from onUpdate(),
// i.e. the main-thread loop outside GameScene::tick() -- mirrors how the
// client-side --test_rename hook drives itself from its own onUpdate(), and
// keeps this out of the deterministic tick section entirely. Once armed
// (uptime elapsed and at least one client fully assigned), this is the
// programmatic equivalent of Game::loadGame's networked-host mid-session
// Load flow (see Game.cpp), minus the file-browse dialog: reload the same
// fixture the host was started with, request the gameplay scene, then tell
// every client to discard its state and resync.
void NetworkServer::updateTestHostLoadHook()
{
    const auto& options = getCommandLineOptions();
    if (!options.testHostLoad.has_value() || _testHostLoadDone)
    {
        return;
    }

    auto elapsedMs = Platform::getTime() - _startTime;
    if (elapsedMs < static_cast<uint32_t>(*options.testHostLoad) * 1000)
    {
        return;
    }

    // Wait for at least one client to have a resolved join assignment --
    // reloading before anyone has joined would prove nothing about the
    // resync path.
    auto anyAssigned = std::any_of(_clients.begin(), _clients.end(), [](const auto& client) { return client->assignmentResolved; });
    if (!anyAssigned)
    {
        return;
    }

    _testHostLoadDone = true;

    if (S5::importSaveToGameState(fs::u8path(options.path), S5::LoadFlags::none))
    {
        SceneManager::requestScene(SceneManager::SceneId::gameplay);
        Logging::info("[TEST] host reloaded save");
        // Fully-qualified: NetworkServer also has a member of this name
        // (called below by this facade); the facade additionally resets the
        // deterministic human-company set for the freshly loaded world
        // before resyncing clients (see Network.cpp).
        Network::requestAllClientsResync();
    }
    else
    {
        Logging::error("[TEST] host failed to reload save '{}'", options.path);
    }
}

// Headless test hook (--test_shutdown_after <seconds>). Driven from
// onUpdate(), same rationale as updateTestHostLoadHook() above. Calling the
// inherited close() (NetworkBase::close(), resolved unqualified here because
// class-member lookup hides the free Network::close() facade of the same
// name) is the already-established pattern for closing from inside this
// object's own update path -- see NetworkClient::receiveServerClosingPacket,
// which does the same thing from inside a packet handler. It only flips
// _isClosed and runs onClose() (which sends ServerClosingPacket and drops
// the networked scene flags); the owning unique_ptr is reset by
// Network::tick() only after this call returns, so there is no
// self-destruction hazard. The host process is not exited: with the
// networked flags gone it simply continues running as single-player.
void NetworkServer::updateTestShutdownHook()
{
    const auto& options = getCommandLineOptions();
    if (!options.testShutdownAfter.has_value() || _testShutdownTriggered)
    {
        return;
    }

    auto elapsedMs = Platform::getTime() - _startTime;
    if (elapsedMs < static_cast<uint32_t>(*options.testShutdownAfter) * 1000)
    {
        return;
    }

    _testShutdownTriggered = true;

    Logging::info("[TEST] closing server");
    close();
}

void NetworkServer::onReceiveSendChatMessagePacket(Client& client, const SendChatMessage& packet)
{
    std::unique_lock<std::mutex> lk(_chatMessageQueueSync);
    _chatMessageQueue.push({ client.id, std::string(packet.getText()) });
}

void NetworkServer::onReceiveGameCommandPacket(Client& client, const GameCommandPacket& packet)
{
    QueuedGameCommand command;
    if (!fromWirePacket(packet, command))
    {
        Logging::error("Dropping malformed game command packet from client '{}'", client.name);
        return;
    }

    if (client.company == CompanyId::null)
    {
        // Spectators (and clients not yet assigned a company) cannot act
        Logging::warn("Dropping game command {} from spectator client '{}'", static_cast<uint32_t>(command.command), client.name);
        return;
    }

    // The company a command acts as is decided by the server's assignment,
    // never by what the client put on the wire
    queueGameCommand(client.company, command.regs, command.flags);
}

void NetworkServer::onReceiveDesyncReportPacket(Client& client, const DesyncReportPacket& packet)
{
    auto& gameState = getGameState();
    Logging::error(
        "Client '{}' reported a desync at tick {} (client rng = {:08X}/{:08X}, client was at tick {}); server is now at tick {}",
        client.name,
        packet.tick,
        packet.srand0,
        packet.srand1,
        packet.localTick,
        gameState.scenarioTicks);

    // Dump our own state so the two sides can be compared offline. Note the
    // server has usually simulated past the mismatching tick by the time the
    // report arrives; the filename records both ticks.
    Network::saveDesyncDump("server", packet.tick, gameState.scenarioTicks);
}

void NetworkServer::removedTimedOutClients()
{
    bool anyRemoved = false;
    for (auto it = _clients.begin(); it != _clients.end();)
    {
        auto& client = *it;
        if (client->connection->hasTimedOut())
        {
            Logging::info("Client timed out: {}", client->name);
            it = _clients.erase(it);
            anyRemoved = true;
        }
        else
        {
            it++;
        }
    }

    if (anyRemoved)
    {
        broadcastRosterUpdate();
    }
}

void NetworkServer::sendPings()
{
    auto now = Platform::getTime();
    if (now - _lastPing > kPingInterval)
    {
        _lastPing = now;

        auto& gameState = getGameState();

        PingPacket packet;
        packet.gameCommandIndex = _gameCommandIndex;
        packet.tick = gameState.scenarioTicks;
        packet.srand0 = gameState.rng.srand_0();
        packet.srand1 = gameState.rng.srand_1();
        for (auto& client : _clients)
        {
            client->connection->sendPacket(packet);
        }
    }
}

void NetworkServer::sendChatMessages()
{
    std::unique_lock<std::mutex> lk(_chatMessageQueueSync);
    while (!_chatMessageQueue.empty())
    {
        const auto& message = _chatMessageQueue.front();

        Network::receiveChatMessage(message.sender, message.message);

        ReceiveChatMessage packet;
        packet.sender = message.sender;
        packet.length = static_cast<uint16_t>(message.message.size() + 1);
        std::memcpy(packet.text, message.message.data(), message.message.size());
        sendPacketToAll(packet);

        _chatMessageQueue.pop();
    }
}

void NetworkServer::processIncomingConnections()
{
    std::unique_lock<std::mutex> lk(_incomingConnectionsSync);
    for (auto& conn : _incomingConnections)
    {
        // The connect packet should be the first one
        while (auto packet = conn->takeNextPacket())
        {
            if (auto connectPacket = packet->as<PacketKind::connect, ConnectPacket>())
            {
                createNewClient(std::move(conn), *connectPacket);
                break;
            }
        }
    }

    _incomingConnections.clear();
}

void NetworkServer::processPackets()
{
    for (auto& client : _clients)
    {
        while (auto packet = client->connection->takeNextPacket())
        {
            onReceivePacketFromClient(*client, *packet);
        }
    }
}

void NetworkServer::onUpdate()
{
    processIncomingConnections();
    processPackets();
    updateClients();
    sendChatMessages();
    sendPings();
    removedTimedOutClients();
    updateTestHostLoadHook();
    updateTestShutdownHook();
}

void NetworkServer::updateClients()
{
    for (auto& client : _clients)
    {
        client->connection->update();
    }
}

void NetworkServer::sendChatMessage(std::string_view message)
{
    std::unique_lock<std::mutex> lk(_chatMessageQueueSync);
    _chatMessageQueue.push({ 0, std::string(message) });
}

std::vector<PlayerRosterEntry> NetworkServer::buildRoster() const
{
    std::vector<PlayerRosterEntry> roster;
    roster.reserve(_clients.size() + 1);

    // client_id_t 0 is reserved for the host (see sendChatMessage's sender).
    PlayerRosterEntry host;
    host.id = 0;
    host.company = CompanyManager::getControllingId();
    host.name = resolveDisplayName(Config::get().preferredOwnerName, 0);
    roster.push_back(std::move(host));

    for (auto& client : _clients)
    {
        PlayerRosterEntry entry;
        entry.id = client->id;
        entry.company = client->company;
        entry.name = client->name; // already resolved/trimmed at connect time
        roster.push_back(std::move(entry));
    }
    return roster;
}

void NetworkServer::broadcastRosterUpdate()
{
    RosterUpdatePacket packet;
    toWirePacket(buildRoster(), packet);
    sendPacketToAll(packet);
}

void NetworkServer::sendGameCommand(const QueuedGameCommand& command)
{
    GameCommandPacket packet;
    if (!toWirePacket(command, packet))
    {
        Logging::error("Unable to serialize game command {} for broadcast", static_cast<uint32_t>(command.command));
        return;
    }
    sendPacketToAll(packet);
}

void NetworkServer::queueGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags, client_id_t requestedBy)
{
    ServerQueuedGameCommand sqc;
    sqc.cmd.index = ++_gameCommandIndex;
    sqc.cmd.tick = 0;
    sqc.cmd.company = company;
    sqc.cmd.flags = flags;
    sqc.cmd.command = static_cast<GameCommands::GameCommand>(regs.esi);
    sqc.cmd.regs = regs;
    sqc.requestedBy = requestedBy;
    _gameCommands.push(sqc);
}

void NetworkServer::runGameCommands()
{
    auto& gameState = getGameState();
    auto tick = gameState.scenarioTicks;

    // Execute all following commands if previously received
    while (!_gameCommands.empty())
    {
        auto& sqc = _gameCommands.front();
        auto& gc = sqc.cmd;
        gc.tick = tick;

        auto result = GameCommands::doCommandForReal(gc.command, gc.company, gc.regs, gc.flags);

        // Failed commands are broadcast too: every peer must consume the same
        // command index sequence, and the deterministic simulation guarantees a
        // command that failed here fails identically on every client (with no
        // state mutation, and error UI shown only on the issuing player's
        // machine). Skipping failed commands would leave a hole in the index
        // sequence and stall all clients.
        sendGameCommand(gc);

        // Join flow: resolve the client's company assignment now that the
        // replicated createPlayerCompany command has run identically on
        // every peer (including this server).
        if (sqc.requestedBy != 0 && gc.command == GameCommands::GameCommand::createPlayerCompany)
        {
            auto* client = findClient(sqc.requestedBy);
            if (client != nullptr)
            {
                client->assignmentResolved = true;
                if (result != GameCommands::kFailure)
                {
                    auto assignedCompany = GameCommands::getLegacyReturnState().lastCreatedCompanyId;
                    client->company = assignedCompany;
                    Logging::info("Assigned company {} to client '{}'", static_cast<uint32_t>(assignedCompany), client->name);

                    CompanyAssignmentPacket packet;
                    packet.company = assignedCompany;
                    client->connection->sendPacket(packet);
                }
                else
                {
                    Logging::info("Could not create a company for client '{}' (no free company slot or no competitor available); leaving as spectator", client->name);

                    // Tell the client explicitly so it knows it is spectating
                    CompanyAssignmentPacket packet;
                    packet.company = CompanyId::null;
                    client->connection->sendPacket(packet);
                }

                broadcastRosterUpdate();
            }
        }

        _gameCommands.pop();
    }
}
