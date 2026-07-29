#pragma once

#include "Types.hpp"
#include <cstdint>
#include <string>
#include <string_view>
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
    constexpr uint16_t kNetworkVersion = 4;

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
    };

    /**
     * Trims a possibly whitespace/NUL-padded display name (e.g. a fixed-size
     * wire buffer, or a machine-local config value) and falls back to
     * "Player #<id>" if the result is empty. Centralizes the display-name
     * rule so every place that logs or stores a name (client accept,
     * company assignment, roster) agrees.
     */
    std::string resolveDisplayName(std::string_view raw, client_id_t id);

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
