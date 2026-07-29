#pragma once

#include "GameCommands/GameCommands.h"
#include "Network.h"
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace OpenLoco::Network
{
    using sequence_t = uint16_t;

#pragma pack(push, 1)
    enum class PacketKind : uint16_t
    {
        unknown,
        ack,
        ping,
        connect,
        connectResponse,
        requestState,
        requestStateResponse,
        requestStateResponseChunk,
        sendChatMessage,
        receiveChatMessage,
        gameCommand,
        desyncReport,
        companyAssignment,
        rosterUpdate,
        serverClosing,
        resyncRequired,
    };

    struct PacketHeader
    {
        PacketKind kind{};
        sequence_t sequence{};
        uint16_t dataSize{};
    };

    constexpr uint16_t kMaxPacketDataSize = kMaxPacketSize - sizeof(PacketHeader);

    struct Packet
    {
        PacketHeader header;
        uint8_t data[kMaxPacketDataSize]{};

        template<typename T>
        const T* cast() const
        {
            return reinterpret_cast<const T*>(data);
        }

        template<PacketKind TKind, typename T>
        const T* as() const
        {
            if (header.kind == TKind && header.dataSize >= sizeof(T))
            {
                return cast<T>();
            }
            return nullptr;
        }
    };

    struct PingPacket
    {
        static constexpr PacketKind kind = PacketKind::ping;
        size_t size() const { return sizeof(PingPacket); }

        uint32_t gameCommandIndex{};
        uint32_t tick{};
        uint32_t srand0{};
        uint32_t srand1{};
    };

    struct ConnectPacket
    {
        static constexpr PacketKind kind = PacketKind::connect;
        size_t size() const { return sizeof(ConnectPacket); }

        uint16_t version{};
        char name[32]{};
        // Session token from a previous CompanyAssignmentPacket (see
        // docs/multiplayer.md § Reconnect). 0 means "fresh join" - the
        // server never issues 0 as a real token (see
        // NetworkServer::generateSessionToken). A non-zero value matching a
        // reserved seat reclaims that seat's identity/company instead of
        // running the normal join policy.
        uint64_t token{};
    };

    enum class ConnectionResult
    {
        success,
        error,
    };

    struct ConnectResponsePacket
    {
        static constexpr PacketKind kind = PacketKind::connectResponse;
        size_t size() const { return sizeof(ConnectResponsePacket); }

        ConnectionResult result;
        char message[256]{};
    };

    struct RequestStatePacket
    {
        static constexpr PacketKind kind = PacketKind::requestState;
        size_t size() const { return sizeof(RequestStatePacket); }

        uint32_t cookie{};
    };

    struct RequestStateResponse
    {
        static constexpr PacketKind kind = PacketKind::requestStateResponse;
        size_t size() const { return sizeof(RequestStateResponse); }

        uint32_t cookie{};
        uint32_t totalSize{};
        uint16_t numChunks{};
    };

    struct RequestStateResponseChunk
    {
        static constexpr PacketKind kind = PacketKind::requestStateResponseChunk;
        size_t size() const { return reinterpret_cast<size_t>(this->data + dataSize) - reinterpret_cast<size_t>(this); }

        uint32_t cookie{};
        uint16_t index{};
        uint32_t offset{};
        uint32_t dataSize{};
        uint8_t data[kMaxPacketDataSize - 14]{};
    };
    static_assert(sizeof(RequestStateResponseChunk) == kMaxPacketDataSize);

    /**
     * Extra state on top of S5 that we want to send over network
     */
    struct ExtraState
    {
        uint32_t gameCommandIndex{};
        uint32_t tick;
        // Deterministic human-company set (session model), mirrored so a
        // late joiner's isPlayerCompany() gating matches every other peer
        // immediately, before it has replayed the game commands that built
        // the set up. See CompanyManager::isHumanCompany.
        uint16_t humanCompanyMask{};
    };

    struct SendChatMessage
    {
        static constexpr PacketKind kind = PacketKind::sendChatMessage;
        size_t size() const { return reinterpret_cast<size_t>(this->text + length) - reinterpret_cast<size_t>(this); }

        uint16_t length{};
        char text[2048]{};

        std::string_view getText() const
        {
            return std::string_view(text, length);
        }
    };
    static_assert(sizeof(SendChatMessage) <= kMaxPacketDataSize);

    struct ReceiveChatMessage
    {
        static constexpr PacketKind kind = PacketKind::receiveChatMessage;
        size_t size() const { return reinterpret_cast<size_t>(this->text + length) - reinterpret_cast<size_t>(this); }

        client_id_t sender{};
        uint16_t length{};
        char text[2048]{};

        std::string_view getText() const
        {
            return std::string_view(text, length);
        }
    };
    static_assert(sizeof(SendChatMessage) <= kMaxPacketDataSize);

    /**
     * Sent by a client to the server when the client's recorded PRNG state for a
     * tick does not match the state the server advertised for that tick in a ping.
     * The server responds by dumping its own game state for offline comparison.
     */
    struct DesyncReportPacket
    {
        static constexpr PacketKind kind = PacketKind::desyncReport;
        size_t size() const { return sizeof(DesyncReportPacket); }

        uint32_t tick{};      // tick at which the mismatch was detected
        uint32_t localTick{}; // tick the client had reached when it noticed
        uint32_t srand0{};    // client's PRNG state at the mismatching tick
        uint32_t srand1{};
    };

    /**
     * Sent by the server to a specific client once it has resolved that
     * client's join-time createPlayerCompany command, telling it which
     * company (if any) it now controls. Only sent to the client in question,
     * never broadcast.
     */
    struct CompanyAssignmentPacket
    {
        static constexpr PacketKind kind = PacketKind::companyAssignment;
        size_t size() const { return sizeof(CompanyAssignmentPacket); }

        CompanyId company{ CompanyId::null };
        // Session token the client should remember and present (in a future
        // ConnectPacket) to reclaim this seat if its connection is lost. Sent
        // on every assignment (including coop/spectator/resend paths), not
        // just the first one - see docs/multiplayer.md § Reconnect.
        uint64_t token{};
    };

    // Cap on a roster entry's display name, and on the number of entries a
    // single RosterUpdatePacket can carry. Chosen generously (both keep the
    // packet well inside kMaxPacketDataSize) - see the static_assert below.
    constexpr size_t kMaxRosterNameLength = 31;
    constexpr size_t kMaxRosterEntries = 32;

    /**
     * One player/spectator entry in a roster snapshot. name is not
     * null-terminated on the wire; nameLength gives its length (already
     * trimmed and capped to kMaxRosterNameLength - see
     * Network::resolveDisplayName).
     */
    struct RosterEntry
    {
        client_id_t id{};
        CompanyId company{ CompanyId::null };
        uint8_t nameLength{};
        char name[kMaxRosterNameLength]{};
        // Set when this entry is a reserved seat (a client that timed out
        // but kept its identity/company reserved pending reconnect) rather
        // than an actively connected client - see docs/multiplayer.md §
        // Reconnect. Rendered as "<name> (disconnected)".
        uint8_t reserved{};
    };

    /**
     * Broadcast by the server to all clients whenever the player roster
     * changes (client accepted, client removed/timed out, company
     * assigned). Presentation data only - never touches GameState or the
     * game command stream. Follows the same "reserve the max, send only
     * what's used" pattern as SendChatMessage: entries beyond `count` are
     * never put on the wire.
     */
    struct RosterUpdatePacket
    {
        static constexpr PacketKind kind = PacketKind::rosterUpdate;
        size_t size() const { return reinterpret_cast<size_t>(this->entries + count) - reinterpret_cast<size_t>(this); }

        uint8_t count{};
        RosterEntry entries[kMaxRosterEntries]{};
    };
    static_assert(sizeof(RosterUpdatePacket) <= kMaxPacketDataSize);

    /**
     * Sent by the server to every client immediately before it tears down
     * its sockets (host quit / process exit), so clients can disconnect
     * gracefully and return to the title scene instead of waiting out a
     * connection timeout. No payload.
     */
    struct ServerClosingPacket
    {
        static constexpr PacketKind kind = PacketKind::serverClosing;
        size_t size() const { return sizeof(ServerClosingPacket); }
    };

    /**
     * Sent by the server when the whole session state has been replaced
     * (host loaded a different save mid-session). Clients must discard
     * their state and re-request the snapshot; join assignments have been
     * reset and will be re-resolved during the resync. No payload.
     */
    struct ResyncRequiredPacket
    {
        static constexpr PacketKind kind = PacketKind::resyncRequired;
        size_t size() const { return sizeof(ResyncRequiredPacket); }
    };

    /**
     * Wire form of a game command. The argument payload is the portable
     * field-by-field serialization produced by GameCommands::encodeCommandArgs
     * (little-endian, layout-independent), not a raw registers blob.
     */
    struct GameCommandPacket
    {
        static constexpr PacketKind kind = PacketKind::gameCommand;
        size_t size() const { return sizeof(GameCommandPacket) - sizeof(data) + dataSize; }

        uint32_t index{};
        uint32_t tick{};
        CompanyId company{};
        uint8_t flags{};
        uint8_t commandId{};
        uint16_t dataSize{};
        uint8_t data[kMaxPacketDataSize - 13]{};
    };
    static_assert(sizeof(GameCommandPacket) == kMaxPacketDataSize);
#pragma pack(pop)

    /**
     * In-memory form of a (received or locally queued) game command, with the
     * arguments unpacked back into the dispatcher's registers representation.
     */
    struct QueuedGameCommand
    {
        uint32_t index{};
        uint32_t tick{};
        CompanyId company{};
        uint8_t flags{};
        GameCommands::GameCommand command{};
        GameCommands::registers regs;
    };

    /**
     * Serializes a queued command into a wire packet.
     * Returns false if the serialized arguments do not fit in a packet.
     */
    bool toWirePacket(const QueuedGameCommand& command, GameCommandPacket& packet);

    /**
     * Parses a wire packet back into a queued command, reconstructing the
     * registers representation (including esi/bl).
     * Returns false if the payload is malformed.
     */
    bool fromWirePacket(const GameCommandPacket& packet, QueuedGameCommand& command);

    /**
     * Serializes a roster snapshot into a wire packet. Returns false (and
     * logs) if there are more entries than a RosterUpdatePacket can carry;
     * the packet is filled with as many entries as fit either way.
     */
    bool toWirePacket(const std::vector<PlayerRosterEntry>& roster, RosterUpdatePacket& packet);

    /**
     * Parses a roster wire packet back into a snapshot.
     */
    std::vector<PlayerRosterEntry> fromWirePacket(const RosterUpdatePacket& packet);
}
