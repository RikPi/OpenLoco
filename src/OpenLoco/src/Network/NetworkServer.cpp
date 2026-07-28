#include "Network/NetworkServer.h"
#include "GameCommands/GameCommands.h"
#include "GameState.h"
#include "Logging.h"
#include "Network/NetworkConnection.h"
#include "S5/S5.h"
#include "Scenario/ScenarioManager.h"
#include "SceneManager.h"
#include <OpenLoco/Core/Exception.hpp>
#include <OpenLoco/Core/MemoryStream.h>
#include <OpenLoco/Platform/Platform.h>
#include <OpenLoco/Utility/String.hpp>
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

void NetworkServer::createNewClient(std::unique_ptr<NetworkConnection> conn, const ConnectPacket& packet)
{
    if (packet.version != kNetworkVersion)
    {
        Logging::info(
            "Rejecting client '{}': network version mismatch (client {}, server {})",
            Utility::nullTerminatedView(packet.name),
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
    newClient->name = Utility::nullTerminatedView(packet.name);
    _clients.push_back(std::move(newClient));

    auto& newClientPtr = *_clients.back();

    ConnectResponsePacket response;
    response.result = ConnectionResult::success;
    newClientPtr.connection->sendPacket(response);

    Logging::info("Accepted new client: {}", newClientPtr.name);
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
    queueGameCommand(command.company, command.regs, command.flags);
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
    for (auto it = _clients.begin(); it != _clients.end();)
    {
        auto& client = *it;
        if (client->connection->hasTimedOut())
        {
            Logging::info("Client timed out: %s", client->name);
            it = _clients.erase(it);
        }
        else
        {
            it++;
        }
    }

    _clients.erase(
        std::remove_if(_clients.begin(), _clients.end(), [](const std::unique_ptr<Client>& client) { return client->connection->hasTimedOut(); }), _clients.end());
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

void NetworkServer::queueGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags)
{
    QueuedGameCommand command;
    command.index = ++_gameCommandIndex;
    command.tick = 0;
    command.company = company;
    command.flags = flags;
    command.command = static_cast<GameCommands::GameCommand>(regs.esi);
    command.regs = regs;
    _gameCommands.push(command);
}

void NetworkServer::runGameCommands()
{
    auto& gameState = getGameState();
    auto tick = gameState.scenarioTicks;

    // Execute all following commands if previously received
    while (!_gameCommands.empty())
    {
        auto& gc = _gameCommands.front();
        gc.tick = tick;

        [[maybe_unused]] auto result = GameCommands::doCommandForReal(gc.command, gc.company, gc.regs, gc.flags);

        // Failed commands are broadcast too: every peer must consume the same
        // command index sequence, and the deterministic simulation guarantees a
        // command that failed here fails identically on every client (with no
        // state mutation, and error UI shown only on the issuing player's
        // machine). Skipping failed commands would leave a hole in the index
        // sequence and stall all clients.
        sendGameCommand(gc);

        _gameCommands.pop();
    }
}
