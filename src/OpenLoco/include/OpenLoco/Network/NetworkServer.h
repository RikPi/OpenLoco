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
        std::queue<ChatMessage> _chatMessageQueue;
        client_id_t _nextClientId = 1;
        uint32_t _lastPing{};
        uint32_t _gameCommandIndex{};
        std::queue<ServerQueuedGameCommand> _gameCommands;

        Client* findClient(const INetworkEndpoint& endpoint);
        Client* findClient(client_id_t id);
        void createNewClient(std::unique_ptr<NetworkConnection> conn, const ConnectPacket& packet);
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

        // Builds the authoritative roster from the live client list plus the
        // host itself (client_id_t 0). Used both to broadcast
        // RosterUpdatePacket to clients and to answer Network::getPlayerRoster()
        // locally on the host, so there is one source of truth.
        std::vector<PlayerRosterEntry> buildRoster() const;
    };
}
