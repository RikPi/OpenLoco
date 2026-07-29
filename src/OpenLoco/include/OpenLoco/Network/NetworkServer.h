#pragma once

#include "Network.h"
#include "NetworkBase.h"
#include "NetworkConnection.h"
#include "Socket.h"
#include <mutex>

namespace OpenLoco::Network
{
    class NetworkConnection;

    struct Client
    {
        client_id_t id{};
        std::unique_ptr<NetworkConnection> connection;
        std::string name;

        // Company this client is allowed to act as. null = spectator (no
        // commands accepted). Assigned by the server during the join flow;
        // never trusted from the wire.
        CompanyId company{ CompanyId::null };

        // Whether the join assignment has been resolved for this client.
        // A state re-request (desync resync) must NOT run the assignment
        // again - it would create another company - only re-send the
        // existing one so the client can restore its controlling id after
        // applying the fresh snapshot.
        bool assignmentResolved{};

        // Session token (docs/multiplayer.md § Reconnect), generated with a
        // non-deterministic source (see generateSessionToken in
        // NetworkServer.cpp) - this is session bookkeeping, not game state,
        // so the usual determinism rules do not apply to it. Sent to the
        // client alongside every CompanyAssignmentPacket; a later
        // ConnectPacket presenting the same token reclaims this seat.
        uint64_t token{};
    };

    // A timed-out client's identity/company, kept so it can reclaim its seat
    // later instead of being forgotten (docs/multiplayer.md § Reconnect). V1
    // keeps these for the whole session's lifetime - no expiry policy yet.
    struct ReservedSeat
    {
        uint64_t token{};
        client_id_t id{};
        std::string name;
        CompanyId company{ CompanyId::null };
        bool assignmentResolved{};
    };

    struct ChatMessage
    {
        client_id_t sender;
        std::string message;
    };

    /**
     * Server-side-only wrapper around a queued command. requestedBy is never
     * put on the wire (QueuedGameCommand/GameCommandPacket stay unchanged);
     * it exists purely so runGameCommands() can, after executing a
     * createPlayerCompany command issued by the join flow, find its way back
     * to the client that should be assigned the resulting company.
     * requestedBy == 0 means "not join-tagged" (client ids start at 1).
     */
    struct ServerQueuedGameCommand
    {
        QueuedGameCommand cmd;
        client_id_t requestedBy{};
    };

    class NetworkServer : public NetworkBase
    {
    private:
        std::mutex _incomingConnectionsSync;
        std::mutex _chatMessageQueueSync;

        std::vector<std::unique_ptr<NetworkConnection>> _incomingConnections;
        std::vector<std::unique_ptr<Client>> _clients;
        // Reconnect (docs/multiplayer.md § Reconnect): seats belonging to
        // timed-out clients, kept for the session's lifetime pending a
        // reconnect with a matching token. See removedTimedOutClients() and
        // createNewClient()'s reclaim branch.
        std::vector<ReservedSeat> _reservedSeats;
        std::queue<ChatMessage> _chatMessageQueue;
        client_id_t _nextClientId = 1;
        // The port passed to listen() - the server's own game port, echoed
        // back in DiscoveryResponsePacket (docs/multiplayer.md § LAN server
        // discovery) since a browsing client's probes are only ever sent to
        // kDefaultPort, which may differ from this.
        port_t _listenPort{};
        uint32_t _lastPing{};
        uint32_t _gameCommandIndex{};
        std::queue<ServerQueuedGameCommand> _gameCommands;

        // Timestamp (Platform::getTime(), ms) the server started listening.
        // Used by the headless test hooks below to measure uptime.
        uint32_t _startTime{};

        // Master server (docs/multiplayer.md § "Master server (phase 2 -
        // design)"): resolved from network.masterServer / --master_server at
        // listen() time. _masterServerHost empty means the feature is
        // disabled - nothing is ever sent. See updateMasterAnnounce() and
        // sendMasterAnnounce().
        std::string _masterServerHost;
        port_t _masterServerPort{};
        uint32_t _lastMasterAnnounce{};
        void updateMasterAnnounce();
        void sendMasterAnnounce();

        // Headless test hook (--test_host_load <seconds>, CommandLine.h):
        // once the server has been up for <seconds> and at least one client
        // has a resolved join assignment, reloads the same save the host was
        // started with and resyncs every client -- the programmatic
        // equivalent of the file-browse-driven mid-session Load flow in
        // Game::loadGame. Driven from onUpdate() (main-thread loop, outside
        // GameScene::tick()), never from inside the deterministic tick
        // section. See KNOWLEDGEBASE.md § Host-driven mid-session load test
        // hook.
        bool _testHostLoadDone{};
        void updateTestHostLoadHook();

        // Headless test hook (--test_shutdown_after <seconds>,
        // CommandLine.h): once the server has been up for <seconds>,
        // gracefully closes it (ServerClosingPacket to every client, sockets
        // torn down) and keeps running as single-player -- exercises a
        // client's reaction to a clean shutdown under --headless, which has
        // no UI quit flow to trigger it otherwise. Also driven from
        // onUpdate(). See KNOWLEDGEBASE.md § Graceful shutdown test hook.
        bool _testShutdownTriggered{};
        void updateTestShutdownHook();

        Client* findClient(const INetworkEndpoint& endpoint);
        Client* findClient(client_id_t id);
        void createNewClient(std::unique_ptr<NetworkConnection> conn, const ConnectPacket& packet);
        // LAN server discovery (docs/multiplayer.md § LAN server discovery):
        // answered connectionless, directly via the socket - never creates a
        // Client or NetworkConnection. Answers ANY discoveryRequest
        // regardless of the requester's version (the request carries none).
        void onReceiveDiscoveryRequestPacket(IUdpSocket& socket, const INetworkEndpoint& endpoint, const DiscoveryRequestPacket& request);
        void onReceivePacketFromClient(Client& client, const Packet& packet);
        void onReceiveStateRequestPacket(Client& client, const RequestStatePacket& packet);
        void onReceiveSendChatMessagePacket(Client& client, const SendChatMessage& packet);
        void onReceiveGameCommandPacket(Client& client, const GameCommandPacket& packet);
        void onReceiveDesyncReportPacket(Client& client, const DesyncReportPacket& packet);
        void broadcastRosterUpdate();
        void removedTimedOutClients();
        void sendPings();
        void sendChatMessages();
        void processIncomingConnections();
        void processPackets();
        void updateClients();

        template<typename T>
        void sendPacketToAll(const T& packet)
        {
            for (auto& client : _clients)
            {
                client->connection->sendPacket(packet);
            }
        }

    protected:
        void onClose() override;
        void onReceivePacket(IUdpSocket& socket, std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet) override;
        void onUpdate() override;

    public:
        ~NetworkServer() override;

        void listen(const std::string& bind, port_t port);
        void sendChatMessage(std::string_view message) override;
        void sendGameCommand(const QueuedGameCommand& command);

        void queueGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags, client_id_t requestedBy = 0);
        void runGameCommands();

        // Host loaded a different save mid-session: reset every client's
        // join assignment and tell them to resync against the new world
        void requestAllClientsResync();

        // Builds the authoritative roster from the live client list plus the
        // host itself (client_id_t 0). Used both to broadcast
        // RosterUpdatePacket to clients and to answer Network::getPlayerRoster()
        // locally on the host, so there is one source of truth.
        std::vector<PlayerRosterEntry> buildRoster() const;
    };
}
