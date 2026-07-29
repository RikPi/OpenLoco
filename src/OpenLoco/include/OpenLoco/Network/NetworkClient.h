#pragma once

#include "Network.h"
#include "NetworkBase.h"
#include "Socket.h"
#include <cstdint>
#include <deque>
#include <list>
#include <span>
#include <vector>

namespace OpenLoco::Network
{
    class NetworkConnection;

    enum class NetworkClientStatus
    {
        none,
        connecting,
        connectedSuccessfully,
        waitingForState,
        connected,
        resyncing,
        closed,
    };

    class NetworkClient : public NetworkBase
    {
    private:
        std::unique_ptr<INetworkEndpoint> _serverEndpoint;
        std::unique_ptr<NetworkConnection> _serverConnection;
        NetworkClientStatus _status{};
        uint32_t _timeout{};
        uint32_t _localGameCommandIndex;
        uint32_t _serverGameCommandIndex;
        uint32_t _localTick;
        uint32_t _serverTick;
        std::list<QueuedGameCommand> _receivedGameCommands;

        struct ReceivedChunk
        {
            uint32_t offset{};
            std::vector<uint8_t> data;
        };

        // Headless test hook (--test_rename <name>, CommandLine.h): after the
        // client is assigned a real company, issues a company rename via the
        // normal doCommand path (outside tick execution, so it round-trips
        // through the network queue exactly like a UI-issued command) and
        // later verifies the name landed. See KNOWLEDGEBASE.md § Client
        // round-trip test hook.
        enum class TestRenameState
        {
            none,
            assignedWaitingConnected,
            pendingIssue,
            pendingVerify,
            done,
        };
        TestRenameState _testRenameState{ TestRenameState::none };
        uint32_t _testRenameDeadline{};
        void updateTestRenameHook();
        void issueTestRenameCommand();
        void verifyTestRenameCommand();

        uint32_t _requestStateCookie{};
        uint32_t _requestStateTotalSize{};
        uint16_t _requestStateNumChunks{};
        std::vector<ReceivedChunk> _requestStateChunksReceived;
        uint32_t _requestStateReceivedBytes{};
        uint32_t _requestStateReceivedChunks{};

        // PRNG state recorded after each fully simulated tick, used to verify
        // the server's advertised PRNG state for that tick (desync detection).
        struct TickRngState
        {
            uint32_t tick{};
            uint32_t srand0{};
            uint32_t srand1{};
        };
        std::deque<TickRngState> _tickRngHistory;
        std::deque<PingPacket> _pendingServerStates;

        void onCancel();
        void processReceivedPackets();
        bool hasTimedOut() const;
        void onReceivePacketFromServer(const Packet& packet);
        void processFullState(std::span<uint8_t const> data);
        void updateLocalTick();

        void initStatus(std::string_view text);
        void setStatus(std::string_view text);
        void clearStatus();
        void endStatus(std::string_view text);

        void sendConnectPacket();
        void sendRequestStatePacket();

        void receiveConnectionResponsePacket(const ConnectResponsePacket& response);
        void receiveRequestStateResponsePacket(const RequestStateResponse& response);
        void receiveRequestStateResponseChunkPacket(const RequestStateResponseChunk& responseChunk);
        void receiveChatMessagePacket(const ReceiveChatMessage& packet);
        void receivePingPacket(const PingPacket& packet);
        void receiveGameCommandPacket(const GameCommandPacket& packet);
        void receiveCompanyAssignmentPacket(const CompanyAssignmentPacket& packet);

        void checkForDesync();
        void onDesyncDetected(const PingPacket& serverState, const TickRngState& localState);
        void beginResync();

    protected:
        void onClose() override;
        void onUpdate() override;
        void onReceivePacket(IUdpSocket& socket, std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet) override;

    public:
        ~NetworkClient() override;

        NetworkClientStatus getStatus() const;
        uint32_t getLocalTick() const;

        void connect(std::string_view host, port_t port);
        void sendChatMessage(std::string_view message) override;
        void sendGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags);

        bool shouldProcessTick(uint32_t tick) const;
        void runGameCommandsForTick(uint32_t tick);
        void onTickProcessed(uint32_t tick);
    };
}
