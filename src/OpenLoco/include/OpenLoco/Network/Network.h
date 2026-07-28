#pragma once

#include "Types.hpp"
#include <cstdint>
#include <string_view>

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
    constexpr uint16_t kNetworkVersion = 2;

    void openServer();
    bool joinServer(std::string_view host);
    bool joinServer(std::string_view host, port_t port);
    void close();
    void tick();

    void sendChatMessage(std::string_view message);
    void receiveChatMessage(client_id_t client, std::string_view message);

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
