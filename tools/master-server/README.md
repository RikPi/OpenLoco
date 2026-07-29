# OpenLoco master server

An internet-wide OpenLoco server list ("phase 2" of the lobby design — see
`docs/multiplayer.md` § "Master server (phase 2 — design)"). Unlike a
typical master server, this one speaks the game's own UDP packet framing
(`Packet.h`) rather than HTTP, so both a hosting game server and the
in-game server browser reuse their existing, tested wire code instead of
gaining an HTTP client dependency.

The service is **stateless**: an in-memory registry with TTL expiry, no
database, no persistence across restarts. A single static Go binary, stdlib
only (see `go.mod` — no `require` entries).

## Protocol

Three connectionless packet kinds, framed exactly like the game's
`PacketHeader { uint16 kind; uint16 sequence; uint16 dataSize }`
(little-endian), appended after the game's existing `discoveryResponse`
(`PacketKind` enum value 17):

| Kind               | Value | Direction              |
|--------------------|-------|-------------------------|
| `masterAnnounce`   | 18    | game server → master    |
| `masterQuery`      | 19    | browser → master        |
| `masterServerList` | 20    | master → browser        |

**These numeric values are load-bearing for the game-side change** — the
game must append `masterAnnounce`, `masterQuery`, `masterServerList` to
`PacketKind` in exactly that order (i.e. immediately after
`discoveryResponse`) so the enum values line up with what this service
hard-codes in `protocol.go`.

Like `discoveryRequest`/`discoveryResponse`, all three kinds bypass
`NetworkConnection` sequencing entirely: no acks, no resend-on-loss, `sequence`
is always sent as `0` and ignored on receipt (mirrors
`NetworkServer::onReceiveDiscoveryRequestPacket`).

### `masterAnnounce` (39-byte payload)

```
uint16 version
uint16 gamePort
uint8  playerCount
uint8  maxPlayers
uint8  joinPolicy
uint8  nameLength
char   name[31]   // not null-terminated; nameLength gives the real length
```

Sent by a hosting game server roughly every 30s and on roster change. The
master records the announce's **observed source IP** (not anything in the
payload) plus the advertised `gamePort` as the registered address — i.e.
the public endpoint as seen from the internet, which is what makes this
useful behind NAT/routers that don't just forward one specific port back to
one specific internal port pair. Entries are keyed by (source IP,
`gamePort`), so one host can register multiple game servers on different
ports. A repeat announce for the same key refreshes its TTL instead of
creating a duplicate entry.

### `masterQuery` (4-byte payload)

```
uint32 cookie
```

Sent by a browsing client. The master replies only to well-formed queries,
directly to the querying address, with a `masterServerList` echoing the
cookie.

### `masterServerList` (variable payload, cookie + capped entry array)

```
uint32 cookie      // echoed from the request
uint8  count
count × {
    uint32 ipv4        // network byte order (octet order, e.g. 1.2.3.4 -> bytes {1,2,3,4})
    uint16 port
    uint16 version
    uint8  playerCount
    uint8  maxPlayers
    uint8  joinPolicy
    uint8  nameLength
    char   name[31]
}
```

Every other field in the packet is little-endian per the game's framing
convention; `ipv4` is the one deliberate exception (network/octet byte
order, matching how an IPv4 address is naturally laid out, e.g. as raw
`sin_addr` bytes) — called out explicitly in the design doc.

**Entry cap (no amplification):** the packet must never exceed
`kMaxPacketSize` (4096) once framed. Per-entry size is 43 bytes
(`4+2+2+1+1+1+1+31`); the `masterServerList` payload's own header (cookie +
count) is 5 bytes; `kMaxPacketDataSize` (`kMaxPacketSize` minus the 6-byte
`PacketHeader`) is 4090. So:

```
available  = kMaxPacketDataSize - 5           = 4090 - 5 = 4085
max entries = floor(available / 43)           = floor(4085 / 43) = 95   (95*43 = 4085, exact)
```

`kMaxServerListEntries = 95` is enforced both where the list is built (the
registry is truncated before encoding) and inside `EncodeMasterServerList`
itself as a hard backstop, so this service can never emit a UDP reply
larger than the request that prompted it by more than a small, fixed,
bounded factor (95 entries vs. one 4-byte query) — and never anywhere close
to an unbounded reflection amplifier.

## Flags

| Flag            | Default | Meaning                                                   |
|-----------------|---------|------------------------------------------------------------|
| `-udp-port`     | `11756` | UDP port for `masterAnnounce`/`masterQuery`                |
| `-http-port`    | `8080`  | HTTP port for `GET /servers` and `GET /healthz` (`0` disables) |
| `-ttl`          | `90s`   | how long an entry survives without a fresh announce         |
| `-max-entries`  | `1024`  | maximum total registry entries                              |
| `-max-per-ip`   | `5`     | maximum registry entries per source IP                      |
| `-verbose`      | `false` | log ignored/rejected packets and other low-level detail     |

IPv4 only in v1: announces/queries from an IPv6 source are ignored (logged
only under `-verbose`).

## HTTP endpoints

- `GET /servers` — JSON array, for humans/monitoring/dashboards, **not**
  consumed by the game itself:

  ```json
  [
    {
      "name": "My Server",
      "address": "203.0.113.10",
      "port": 11754,
      "version": 7,
      "players": 3,
      "maxPlayers": 32,
      "joinPolicy": 1,
      "lastSeenSecondsAgo": 12
    }
  ]
  ```

- `GET /healthz` — `200 ok`, for uptime checks / container orchestrators.

## Building and testing

Go 1.26+, stdlib only.

```sh
go vet ./...
go test ./...
go build -o master-server .          # master-server.exe on Windows
```

Run it:

```sh
./master-server -udp-port 11756 -http-port 8080 -ttl 90s
```

## Deployment

### systemd (personal VPS)

```ini
# /etc/systemd/system/openloco-master-server.service
[Unit]
Description=OpenLoco master server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/master-server -udp-port 11756 -http-port 8080 -ttl 90s
Restart=on-failure
RestartSec=5
User=nobody
Group=nogroup
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now openloco-master-server
```

The process handles `SIGINT`/`SIGTERM` gracefully (stops accepting new
work, closes the UDP socket, shuts the HTTP server down with a bounded
grace period, then exits) — no special systemd `KillSignal`/`TimeoutStopSec`
tuning is required beyond the defaults.

### Docker

```sh
docker build -t openloco-master-server .
docker run --rm \
  -p 11756:11756/udp \
  -p 8080:8080/tcp \
  openloco-master-server -ttl 90s
```

The image is a multi-stage build: `CGO_ENABLED=0` static compile in a
`golang` build stage, copied into `gcr.io/distroless/static-debian12:nonroot`
(no shell, no package manager, runs as a non-root user).

### Fly.io (or any UDP-capable free/low-cost tier)

Fly.io supports raw UDP services via `fly.toml`'s `[[services]]` block with
`protocol = "udp"`. Rough shape (adjust app name/region):

```toml
app = "openloco-master-server"

[build]
  dockerfile = "Dockerfile"

[[services]]
  protocol = "udp"
  internal_port = 11756
  [[services.ports]]
    port = 11756

[[services]]
  protocol = "tcp"
  internal_port = 8080
  [[services.ports]]
    port = 8080
    handlers = ["http"]
```

Since the registry is purely in-memory, a restart/redeploy simply drops all
current entries — every hosting server re-announces within one TTL-length
window (default 90s) and the list repopulates itself; no migration or
backup step is needed.

## Known v1 limitation

No NAT traversal: an internet-reachable host must forward its game port
itself. This service only makes a server *discoverable* (via its observed
public IP + advertised port), not *reachable*.
