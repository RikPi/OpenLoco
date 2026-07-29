#include "Network/ServerDiscovery.h"
#include "Network/NetworkBase.h"
#include "Network/Packet.h"
#include "Network/Socket.h"
#include <OpenLoco/Diagnostics/Logging.h>
#include <OpenLoco/Platform/Platform.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

using namespace OpenLoco::Diagnostics;

namespace OpenLoco::Network::ServerDiscovery
{
    namespace
    {
        constexpr uint32_t kProbeIntervalMs = 1000;
        constexpr uint32_t kServerExpiryMs = 5000;

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
                _cookie = static_cast<uint32_t>(Platform::getTime()) ^ 0xA5A5A5A5u;

                _sockets.push_back(Socket::createUdp());
                beginReceivePacketLoop();

                // Probe immediately rather than waiting a full interval.
                sendProbe();
                _lastProbeTime = Platform::getTime();
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
                auto response = packet.as<PacketKind::discoveryResponse, DiscoveryResponsePacket>();
                if (response == nullptr || response->cookie != _cookie)
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

                std::unique_lock<std::mutex> lk(_serversSync);
                auto it = std::find_if(_servers.begin(), _servers.end(), [&](const DiscoveredServer& s) {
                    return s.address == entry.address && s.port == entry.port;
                });
                if (it != _servers.end())
                {
                    *it = std::move(entry);
                }
                else
                {
                    _servers.push_back(std::move(entry));
                }
            }

        private:
            uint32_t _cookie{};
            uint32_t _lastProbeTime{};
            mutable std::mutex _serversSync;
            std::vector<DiscoveredServer> _servers;

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
