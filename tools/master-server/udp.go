package main

import (
	"context"
	"log"
	"net"
	"time"
)

// UDPServer owns the connectionless UDP request loop: masterAnnounce
// upserts the registry, masterQuery replies with a bounded masterServerList.
// Everything else is ignored silently (per the spec: "Ignore everything
// else silently").
type UDPServer struct {
	Registry *Registry
	Log      *log.Logger
	Verbose  bool
}

// Serve reads datagrams from conn until ctx is cancelled or the socket
// errors out (which happens on Close during shutdown).
func (s *UDPServer) Serve(ctx context.Context, conn *net.UDPConn) {
	buf := make([]byte, kMaxPacketSize)
	for {
		// Bound the read so we notice ctx cancellation promptly even
		// though ReadFromUDP itself doesn't take a context.
		_ = conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			// Any other error (e.g. socket closed from under us during
			// shutdown) - stop.
			return
		}
		s.handlePacket(conn, addr, buf[:n])
	}
}

func (s *UDPServer) handlePacket(conn *net.UDPConn, addr *net.UDPAddr, raw []byte) {
	kind, _, payload, err := DecodePacket(raw)
	if err != nil {
		if s.Verbose {
			s.Log.Printf("malformed packet from %s: %v", addr, err)
		}
		return
	}

	// IPv4 only v1 (spec): reject/ignore IPv6 sources gracefully.
	if addr.IP.To4() == nil {
		if s.Verbose {
			s.Log.Printf("ignoring non-IPv4 source %s", addr)
		}
		return
	}

	switch kind {
	case KindMasterAnnounce:
		s.handleAnnounce(addr, payload)
	case KindMasterQuery:
		s.handleQuery(conn, addr, payload)
	default:
		if s.Verbose {
			s.Log.Printf("ignoring packet kind %d from %s", kind, addr)
		}
	}
}

func (s *UDPServer) handleAnnounce(addr *net.UDPAddr, payload []byte) {
	ann, ok := DecodeMasterAnnounce(payload)
	if !ok {
		if s.Verbose {
			s.Log.Printf("malformed masterAnnounce from %s", addr)
		}
		return
	}

	result := s.Registry.Upsert(addr.IP, ann, time.Now())
	switch result {
	case UpsertOK:
		if s.Verbose {
			s.Log.Printf("announce from %s:%d -> registered '%s' (v%d, %d/%d, policy %d)",
				addr.IP, ann.GamePort, ann.Name, ann.Version, ann.PlayerCount, ann.MaxPlayers, ann.JoinPolicy)
		}
	case UpsertRejectedNotIPv4:
		if s.Verbose {
			s.Log.Printf("rejected announce from %s: not IPv4", addr)
		}
	case UpsertRejectedPerIPCap:
		s.Log.Printf("rejected announce from %s: per-IP cap reached", addr.IP)
	case UpsertRejectedRegistryFull:
		s.Log.Printf("rejected announce from %s: registry full", addr.IP)
	}
}

func (s *UDPServer) handleQuery(conn *net.UDPConn, addr *net.UDPAddr, payload []byte) {
	cookie, ok := DecodeMasterQuery(payload)
	if !ok {
		if s.Verbose {
			s.Log.Printf("malformed masterQuery from %s", addr)
		}
		return
	}

	all := s.Registry.List()
	if len(all) > kMaxServerListEntries {
		all = all[:kMaxServerListEntries]
	}
	entries := make([]ServerListEntry, 0, len(all))
	for _, e := range all {
		entries = append(entries, ServerListEntry{
			IPv4:        e.IP,
			Port:        e.Port,
			Version:     e.Version,
			PlayerCount: e.PlayerCount,
			MaxPlayers:  e.MaxPlayers,
			JoinPolicy:  e.JoinPolicy,
			Name:        e.Name,
		})
	}

	respPayload := EncodeMasterServerList(cookie, entries)
	// Connectionless reply, sequence 0 - mirrors discoveryResponse
	// (NetworkServer::onReceiveDiscoveryRequestPacket): the game ignores
	// sequence for these kinds.
	framed, err := EncodePacket(KindMasterServerList, 0, respPayload)
	if err != nil {
		// Can only happen if entries were miscounted upstream; encodeMasterServerList
		// already caps to kMaxServerListEntries, so this should be unreachable.
		s.Log.Printf("failed to encode masterServerList reply: %v", err)
		return
	}

	if _, err := conn.WriteToUDP(framed, addr); err != nil {
		if s.Verbose {
			s.Log.Printf("failed to send masterServerList to %s: %v", addr, err)
		}
		return
	}
	if s.Verbose {
		s.Log.Printf("query from %s (cookie %d) -> %d entries", addr, cookie, len(entries))
	}
}
