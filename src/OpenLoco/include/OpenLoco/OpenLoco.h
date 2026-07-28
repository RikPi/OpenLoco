#pragma once

#include <OpenLoco/Core/FileSystem.hpp>
#include <cstdint>
#include <functional>
#include <string>

namespace OpenLoco
{
    using StringId = uint16_t;

    namespace Engine
    {
        constexpr uint32_t MaxTimeDeltaMs = 500;
        constexpr uint32_t UpdateRateHz = 40;
        constexpr uint32_t UpdateRateInMs = 1000 / UpdateRateHz;
        constexpr uint32_t MaxUpdates = 3;
    }

    void* hInstance();
    void resetSubsystems();
    void simulateGame(const fs::path& path, int32_t ticks);

    // Procedurally generates a small, tickable game state (headless, without requiring
    // vanilla Locomotion assets) and exports it as an S5 save. Used to create fixture
    // saves for testing. The same seed always produces a byte-identical save.
    void generateSaveGame(const fs::path& path, uint32_t seed);

    void initialise();
    void update();
    void sub_431695(uint16_t var_F253A0);
    uint16_t getTimeSinceLastTick();
    uint16_t getNumFrameUpdates();
    bool promptTickLoop(std::function<bool()> tickAction);
    [[noreturn]] void exitCleanly();
    [[noreturn]] void exitWithError(StringId titleStringId, StringId messageStringId);
}
