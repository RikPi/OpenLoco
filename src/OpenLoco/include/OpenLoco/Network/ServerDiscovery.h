#pragma once

#include "Network.h"
#include <vector>

namespace OpenLoco::Network::ServerDiscovery
{
    // Starts (if not already active) broadcasting discoveryRequest probes
    // roughly once a second to 255.255.255.255:kDefaultPort and
    // 127.0.0.1:kDefaultPort (see docs/multiplayer.md § LAN server
    // discovery), collecting discoveryResponse replies into a deduplicated
    // (by endpoint) list. A no-op if already active.
    void begin();

    // Stops discovery and forgets everything found so far. A no-op if not
    // active.
    void end();

    // Drives probing/expiry (~1s probe interval, ~5s entry expiry). Call
    // once per frame regardless of network mode/scene - see
    // Network::tick(), which already runs unconditionally. A no-op while
    // not active.
    void tick();

    // Snapshot copy of servers seen within the last ~5s. Empty while not
    // active.
    std::vector<DiscoveredServer> getServers();
}
