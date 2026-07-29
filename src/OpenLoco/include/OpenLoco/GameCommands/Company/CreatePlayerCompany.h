#pragma once

#include "GameCommands/GameCommands.h"

namespace OpenLoco::GameCommands
{
    // Creates a company for a joining network player. Deterministic: the
    // competitor and colours are chosen inside the command via the synced
    // PRNG (see CompanyManager::createJoiningPlayerCompany), never from
    // machine-local Config, so it replicates identically on every peer. Takes
    // no arguments; on success the created id is exposed via
    // LegacyReturnState::lastCreatedCompanyId for the server to read after
    // doCommandForReal returns.
    struct CreatePlayerCompanyArgs
    {
        static constexpr auto command = GameCommand::createPlayerCompany;

        CreatePlayerCompanyArgs() = default;
        explicit CreatePlayerCompanyArgs(const registers&)
        {
        }

        explicit operator registers() const
        {
            return registers();
        }
    };

    void createPlayerCompany(registers& regs, const uint8_t flags);
}
