#include "Network/ServerDiscovery.h"
#include "CommandLine.h"
#include "Config.h"
#include "Network/NetworkBase.h"
#include "Network/Packet.h"
#include "Network/Socket.h"
#include <OpenLoco/Diagnostics/Logging.h>
#include <OpenLoco/Platform/Platform.h>
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <tuple>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco::Network::ServerDiscovery
{
    namespace
    {
        constexpr uint32_t kProbeIntervalMs = 1000;
        constexpr uint32_t kServerExpiryMs = 5000;
        // Master server query cadence (docs/multiplayer.md § "Master server
        // (phase 2 - design)"), independent of the LAN probe interval above.
        constexpr uint32_t kMasterQueryIntervalMs = 2000;

        // Reuses NetworkBase purely for its background receive-thread
        // plumbing (beginReceivePacketLoop/onReceivePacket/close). A
        // browsing client is not a NetworkClient: it never sends a
        // ConnectPacket, never gets an ordered/acked NetworkConnection, and
        // has no session at all - it just fires connectionless
        // DiscoveryRequestPackets and collects whatever
        // DiscoveryResponsePackets come back (see Packet.h).
        class DiscoveryClient : public NetworkBase
        {
        public:
            // sendChatMessage is pure virtual on NetworkBase and irrelevant
            // to a discovery-only client.
            void sendChatMessage(std::string_view) override
            {
            }

            void start()
            {
                // Not security-relevant (LAN discovery, not a session token)
                // - just enough to ignore a response left over from a
                // previous discovery episode in a very unlucky timing case.
                // Reused as-is for masterQuery too (independent PacketKind,
                // no collision risk).
                _cookie = static_cast<uint32_t>(Platform::getTime()) ^ 0xA5A5A5A5u;

                _sockets.push_back(Socket::createUdp());
                beginReceivePacketLoop();

                // Master server (docs/multiplayer.md § "Master server (phase
                // 2 - design)"): --master_server overrides
                // network.masterServer, same precedence as --bind/--port.
                // Empty means disabled - no queries are ever sent.
                const auto& cmdlineOptions = getCommandLineOptions();
                const auto& masterServerConfig = !cmdlineOptions.masterServer.empty() ? cmdlineOptions.masterServer : Config::get().network.masterServer;
                if (!masterServerConfig.empty())
                {
                    std::tie(_masterHost, _masterPort) = parseServerAddress(masterServerConfig, kDefaultMasterServerPort);
                }

                // Probe immediately rather than waiting a full interval.
                sendProbe();
                _lastProbeTime = Platform::getTime();

                if (!_masterHost.empty())
                {
                    sendMasterQuery();
                    _lastMasterQueryTime = Platform::getTime();
                }
            }

            std::vector<DiscoveredServer> snapshot() const
            {
                std::unique_lock<std::mutex> lk(_serversSync);
                return _servers;
            }

        protected:
            void onUpdate() override
            {
                auto now = Platform::getTime();
                if (now - _lastProbeTime >= kProbeIntervalMs)
                {
                    _lastProbeTime = now;
                    sendProbe();
                }

                if (!_masterHost.empty() && now - _lastMasterQueryTime >= kMasterQueryIntervalMs)
                {
                    _lastMasterQueryTime = now;
                    sendMasterQuery();
                }

                std::unique_lock<std::mutex> lk(_serversSync);
                _servers.erase(
                    std::remove_if(
                        _servers.begin(),
                        _servers.end(),
                        [&](const DiscoveredServer& s) { return now - s.lastSeen > kServerExpiryMs; }),
                    _servers.end());
            }

            void onReceivePacket([[maybe_unused]] IUdpSocket& socket, std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet) override
            {
                if (auto response = packet.as<PacketKind::discoveryResponse, DiscoveryResponsePacket>())
                {
                    if (response->cookie != _cookie)
                    {
                        return;
                    }

                    DiscoveredServer entry;
                    entry.address = endpoint->getIpAddress();
                    entry.port = response->port;
                    auto nameLength = std::min<size_t>(response->nameLength, sizeof(response->name));
                    entry.name.assign(response->name, nameLength);
                    entry.version = response->version;
                    entry.playerCount = response->playerCount;
                    entry.maxPlayers = response->maxPlayers;
                    entry.joinPolicy = response->joinPolicy;
                    entry.lastSeen = Platform::getTime();
                    entry.source = DiscoveredServerSource::lan;

                    mergeServer(std::move(entry));
                    return;
                }

                // Master server (docs/multiplayer.md § "Master server (phase
                // 2 - design)"): a masterServerList is variable-length (only
                // `count` entries are meaningful - see Packet.h), so it can't
                // go through Packet::as<>() (which requires dataSize >=
                // sizeof(T), and sizeof(MasterServerListPacket) is the
                // fixed 95-entry maximum). Read the header fields directly
                // and validate dataSize covers the declared entry count
                // before trusting any of them - mirrors
                // tools/master-server/protocol.go's DecodeMasterServerList.
                if (packet.header.kind == PacketKind::masterServerList)
                {
                    const auto* list = reinterpret_cast<const MasterServerListPacket*>(packet.data);
                    if (list->cookie != _cookie)
                    {
                        return;
                    }

                    auto count = std::min<size_t>(list->count, kMaxMasterServerListEntries);
                    // Same pointer-difference idiom as
                    // MasterServerListPacket::size()/RosterUpdatePacket::size()
                    // (Packet.h), computed against the already-clamped local
                    // `count` rather than the possibly-oversized wire field.
                    auto need = reinterpret_cast<size_t>(list->entries + count) - reinterpret_cast<size_t>(list);
                    if (packet.header.dataSize < need)
                    {
                        return;
                    }

                    for (size_t i = 0; i < count; i++)
                    {
                        const auto& src = list->entries[i];

                        DiscoveredServer entry;
                        entry.address = fmt::format("{}.{}.{}.{}", src.ipv4[0], src.ipv4[1], src.ipv4[2], src.ipv4[3]);
                        entry.port = src.port;
                        auto nameLength = std::min<size_t>(src.nameLength, sizeof(src.name));
                        entry.name.assign(src.name, nameLength);
                        entry.version = src.version;
                        entry.playerCount = src.playerCount;
                        entry.maxPlayers = src.maxPlayers;
                        entry.joinPolicy = src.joinPolicy;
                        entry.lastSeen = Platform::getTime();
                        entry.source = DiscoveredServerSource::master;

                        mergeServer(std::move(entry));
                    }
                }
            }

        private:
            uint32_t _cookie{};
            uint32_t _lastProbeTime{};
            std::string _masterHost;
            port_t _masterPort{};
            uint32_t _lastMasterQueryTime{};
            mutable std::mutex _serversSync;
            std::vector<DiscoveredServer> _servers;

            // Merges a freshly-seen entry into _servers, deduplicated by
            // endpoint (address, port). A LAN-sourced entry always wins over
            // a master-sourced duplicate for the same endpoint (it is a
            // direct, freshly-verified reply from the server itself) - a
            // master entry never overwrites an existing LAN one, but a LAN
            // entry does overwrite/upgrade an existing master one.
            void mergeServer(DiscoveredServer entry)
            {
                std::unique_lock<std::mutex> lk(_serversSync);
                auto it = std::find_if(_servers.begin(), _servers.end(), [&](const DiscoveredServer& s) {
                    return s.address == entry.address && s.port == entry.port;
                });
                if (it != _servers.end())
                {
                    if (it->source == DiscoveredServerSource::lan && entry.source == DiscoveredServerSource::master)
                    {
                        return;
                    }
                    *it = std::move(entry);
                }
                else
                {
                    _servers.push_back(std::move(entry));
                }
            }

            void sendProbe()
            {
                DiscoveryRequestPacket request;
                request.cookie = _cookie;

                Packet packet;
                packet.header.kind = PacketKind::discoveryRequest;
                packet.header.sequence = 0;
                packet.header.dataSize = static_cast<uint16_t>(sizeof(request));
                std::memcpy(packet.data, &request, sizeof(request));

                auto size = sizeof(PacketHeader) + packet.header.dataSize;
                auto& socket = _sockets.front();

                // Broadcast reaches other machines on the LAN; loopback is
                // needed separately since broadcast usually does not
                // traverse the loopback interface (needed for same-machine
                // testing). Both target kDefaultPort - a server configured
                // to listen on a non-default port will not be discovered
                // this way (still reachable via "Join by address").
                socket->sendData(Protocol::ipv4, "255.255.255.255", kDefaultPort, &packet, size);
                socket->sendData(Protocol::ipv4, "127.0.0.1", kDefaultPort, &packet, size);
            }

            // Master server (docs/multiplayer.md § "Master server (phase 2 -
            // design)"): only ever called when _masterHost is non-empty (see
            // start()/onUpdate()). Connectionless, same shape as sendProbe().
            void sendMasterQuery()
            {
                MasterQueryPacket request;
                request.cookie = _cookie;

                Packet packet;
                packet.header.kind = PacketKind::masterQuery;
                packet.header.sequence = 0;
                packet.header.dataSize = static_cast<uint16_t>(sizeof(request));
                std::memcpy(packet.data, &request, sizeof(request));

                auto size = sizeof(PacketHeader) + packet.header.dataSize;
                auto& socket = _sockets.front();
                socket->sendData(Protocol::ipv4, _masterHost, _masterPort, &packet, size);
            }
        };

        std::unique_ptr<DiscoveryClient> _discovery;
    }

    void begin()
    {
        if (_discovery != nullptr)
        {
            return;
        }

        auto discovery = std::make_unique<DiscoveryClient>();
        try
        {
            discovery->start();
            _discovery = std::move(discovery);
        }
        catch (const std::exception& e)
        {
            Logging::error("Unable to start server discovery: {}", e.what());
        }
    }

    void end()
    {
        _discovery = nullptr;
    }

    void tick()
    {
        if (_discovery != nullptr)
        {
            _discovery->update();
        }
    }

    std::vector<DiscoveredServer> getServers()
    {
        if (_discovery == nullptr)
        {
            return {};
        }
        return _discovery->snapshot();
    }
}
