#include "GameCommands/Company/CreatePlayerCompany.h"
#include "GameCommands/GameCommands.h"
#include "World/CompanyManager.h"

namespace OpenLoco::GameCommands
{
    // Creates a company for a joining network player. Mirrors the
    // updateOwnerStatus/zero-cost pattern: the non-apply pass (cost
    // estimation) does nothing and always reports success so the real
    // (PRNG-consuming, state-mutating) allocation only ever runs once, on the
    // apply pass.
    static uint32_t createPlayerCompany(const uint8_t flags)
    {
        if (flags & Flags::apply)
        {
            auto newCompanyId = CompanyManager::createJoiningPlayerCompany();
            if (newCompanyId == CompanyId::null)
            {
                return kFailure;
            }

            CompanyManager::markCompanyAsHuman(newCompanyId);
            getLegacyReturnState().lastCreatedCompanyId = newCompanyId;
        }
        return 0;
    }

    void createPlayerCompany(registers& regs, const uint8_t flags)
    {
        // No arguments to unpack: the competitor/colours are chosen
        // deterministically inside the command.
        regs.ebx = createPlayerCompany(flags);
    }
}
