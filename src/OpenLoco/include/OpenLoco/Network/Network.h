#pragma once

#include "Types.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenLoco::GameCommands
{
    struct registers;
}

namespace OpenLoco::Network
{
    using client_id_t = uint32_t;
    using port_t = uint16_t;

    constexpr port_t kDefaultPort = 11754;
    // Default UDP port of the master server (tools/master-server's own
    // -udp-port default), used when network.masterServer / --master_server
    // names a host with no explicit port.
    constexpr port_t kDefaultMasterServerPort = 11756;
    constexpr uint16_t kMaxPacketSize = 4096;

    // Version 2: game command arguments are serialized field-by-field
    // (see GameCommands::encodeCommandArgs) instead of as a registers blob,
    // desync reporting added, and the server validates the client version.
    // Version 3: session model Phase B - ExtraState carries the deterministic
    // human-company set (ExtraState::humanCompanyMask) and a new
    // CompanyAssignmentPacket tells a joining client which company (if any)
    // it was assigned.
    // Version 4: player roster (RosterUpdatePacket, presentation data only -
    // never touches GameState/game commands) and graceful server shutdown
    // notification (ServerClosingPacket).
    // Version 5: host-driven mid-session load - ResyncRequiredPacket makes
    // every client discard its state and re-request the snapshot; join
    // assignments are re-resolved against the newly loaded world (while
    // desync resyncs now deliberately keep the existing assignment).
    // Version 6: reconnect (docs/multiplayer.md § Reconnect) - ConnectPacket
    // carries a session token (0 = fresh join) and CompanyAssignmentPacket
    // echoes the server-issued token back; a client that times out has its
    // seat (token/id/name/company/assignmentResolved) reserved rather than
    // forgotten, and a later ConnectPacket with a matching token reclaims it
    // (skipping join policy - the existing assignmentResolved snapshot/
    // resend path does the rest); RosterEntry gained a `reserved` flag so
    // the roster can show reserved seats as "<name> (disconnected)".
    // Version 7: LAN server discovery (docs/multiplayer.md § LAN server
    // discovery) - two new connectionless packet kinds (discoveryRequest/
    // discoveryResponse, Packet.h) that bypass NetworkConnection sequencing
    // entirely (no acks; handled directly in NetworkServer::onReceivePacket
    // for endpoints that are not an established client). The server answers
    // ANY discoveryRequest regardless of the requester's version (the
    // request carries none) and self-describes its own version in the
    // response, so a browser can grey out incompatible servers rather than
    // this version bump itself gating whether a server can be found at all.
    // Version 8: master server (phase 2, docs/multiplayer.md § "Master
    // server (phase 2 - design)") - three more connectionless packet kinds
    // (masterAnnounce, masterQuery, masterServerList, Packet.h), appended
    // immediately after discoveryResponse, mirroring the standalone Go
    // service in tools/master-server/ (protocol.go) field-for-field. A
    // hosting server periodically announces itself to a configured master
    // (network.masterServer / --master_server) and the in-game browser
    // queries the master alongside its existing LAN probes, merging both
    // into one list (deduped by endpoint, LAN wins - see
    // Network/ServerDiscovery.cpp). Presentation-only, exactly like LAN
    // discovery - never touches GameState or the game command stream, so
    // this bump exists only for consistency with the versioning scheme (no
    // wire-incompatible change to the session protocol itself).
    constexpr uint16_t kNetworkVersion = 8;

    /**
     * Machine-local snapshot of one player/spectator, used to drive the
     * player list UI and to resolve chat sender names. Never part of
     * GameState and never fed into a game command - purely presentation
     * data, mirrored from the server's authoritative view via
     * RosterUpdatePacket.
     */
    struct PlayerRosterEntry
    {
        client_id_t id{};
        CompanyId company{ CompanyId::null };
        std::string name;
        // True for a reserved (disconnected, reconnect-pending) seat rather
        // than an actively connected client - see docs/multiplayer.md §
        // Reconnect.
        bool reserved{};
    };

    /**
     * Trims a possibly whitespace/NUL-padded display name (e.g. a fixed-size
     * wire buffer, or a machine-local config value) and falls back to
     * "Player #<id>" if the result is empty. Centralizes the display-name
     * rule so every place that logs or stores a name (client accept,
     * company assignment, roster) agrees.
     */
    std::string resolveDisplayName(std::string_view raw, client_id_t id);

    /**
     * Parses a free-form server address as typed by a player (or read from a
     * config/CLI value) into a (host, port) pair, defaulting to `defaultPort`
     * when no port is given (kDefaultPort unless the caller overrides it -
     * e.g. network.masterServer/--master_server default to
     * kDefaultMasterServerPort instead). Accepts "host", "host:port" and
     * "[ipv6]:port" (a bare IPv6 address with no port contains several
     * colons and is passed through unchanged). Shared by every entry point
     * that accepts such an address (the ServerBrowser "Join by address"
     * prompt, and the master server host/query endpoints).
     */
    std::pair<std::string, port_t> parseServerAddress(std::string_view input, port_t defaultPort = kDefaultPort);

    void openServer();
    bool joinServer(std::string_view host);
    bool joinServer(std::string_view host, port_t port);
    void close();
    void tick();

    void sendChatMessage(std::string_view message);
    void receiveChatMessage(client_id_t client, std::string_view message);

    /**
     * The current player roster: one entry per connected client plus the
     * host itself (client_id_t 0). Works identically whether called on the
     * server or a client - the single call UI code should use. Returns a
     * snapshot copy; on the server it is rebuilt on every call from the
     * live client list, on a client it is the latest RosterUpdatePacket
     * received from the server.
     */
    std::vector<PlayerRosterEntry> getPlayerRoster();

    // Where a DiscoveredServer entry came from - a LAN discoveryResponse or
    // a master server's masterServerList entry (docs/multiplayer.md §
    // "Master server (phase 2 - design)"). Used to dedupe entries that
    // appear via both paths (the same endpoint answering a LAN broadcast/
    // loopback probe and also being listed by the master): LAN always wins,
    // since it is a direct, freshly-verified reply from the server itself.
    enum class DiscoveredServerSource : uint8_t
    {
        lan,
        master,
    };

    /**
     * A server discovered either on the LAN or via the configured master
     * server (see Network/ServerDiscovery.h), deduplicated by endpoint (LAN
     * wins on conflict - see `source`). Presentation data for the server
     * browser UI only - never touches GameState or the game command stream.
     */
    struct DiscoveredServer
    {
        std::string address;
        port_t port{};
        std::string name;
        uint16_t version{};
        uint8_t playerCount{};
        uint8_t maxPlayers{};
        uint8_t joinPolicy{};
        uint32_t lastSeen{}; // Platform::getTime() of the last response/list entry received
        DiscoveredServerSource source{ DiscoveredServerSource::lan };
    };

    /**
     * Starts broadcasting discoveryRequest probes on the LAN roughly once a
     * second and collecting discoveryResponse replies, and - when
     * network.masterServer/--master_server is configured - also querying the
     * master server roughly every 2s and merging its results in (see
     * Network/ServerDiscovery.h). Safe to call repeatedly - a no-op while
     * already active.
     */
    void beginServerDiscovery();

    /**
     * Stops discovery and forgets everything found so far. Safe to call
     * when not active.
     */
    void endServerDiscovery();

    /**
     * Snapshot copy of servers seen within the last ~5s. Empty when
     * discovery is not active.
     */
    std::vector<DiscoveredServer> getDiscoveredServers();

    /**
     * Host only: after the local game state has been replaced (a different
     * save was loaded mid-session), resets every client's join assignment
     * and tells them to discard their state and resync against the new
     * world.
     */
    void requestAllClientsResync();

    void queueGameCommand(CompanyId company, const OpenLoco::GameCommands::registers& regs, const uint8_t flags);
    bool shouldProcessTick(uint32_t tick);
    void processGameCommands(uint32_t tick);

    /**
     * Called at the end of every fully simulated game tick. On a client this
     * records the PRNG state for the tick so it can be verified against the
     * state the server advertises in its pings (desync detection).
     */
    void onTickProcessed(uint32_t tick);

    /**
     * Exports the current game state to the desync dump directory
     * (save/desync). Used by both ends when a desync is detected so the two
     * states can be compared offline with the `compare` CLI command.
     */
    void saveDesyncDump(std::string_view role, uint32_t mismatchTick, uint32_t currentTick);

    /**
     * Whether the game state is networked.
     * This will return false if the client is still receiving the map from the server.
     */
    bool isConnected();

    /**
     * Gets the current tick the server is on.
     */
    uint32_t getServerTick();
}
