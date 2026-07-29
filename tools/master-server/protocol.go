// Package main: wire protocol for the OpenLoco master server.
//
// This file mirrors the game's packet framing exactly, as defined in
// src/OpenLoco/include/OpenLoco/Network/Packet.h. See docs/multiplayer.md
// § "Master server (phase 2 — design)" for the protocol description this
// implements.
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"net"
)

// ---------------------------------------------------------------------------
// Framing (Packet.h: PacketHeader, kMaxPacketSize)
// ---------------------------------------------------------------------------

const (
	// kMaxPacketSize mirrors Network.h's kMaxPacketSize (4096): the hard
	// ceiling on any single UDP packet the game will read or write.
	kMaxPacketSize = 4096

	// packetHeaderSize is sizeof(PacketHeader) under #pragma pack(1):
	// uint16 kind + uint16 sequence + uint16 dataSize = 6 bytes.
	packetHeaderSize = 6

	// kMaxPacketDataSize mirrors Packet.h's kMaxPacketDataSize
	// (kMaxPacketSize - sizeof(PacketHeader)).
	kMaxPacketDataSize = kMaxPacketSize - packetHeaderSize // 4090
)

// PacketKind mirrors Network::PacketKind (Packet.h). Only the tail of the
// enum matters here; the master server never speaks any of the earlier
// (session-oriented) kinds. Values are hard-coded to match the order the
// existing enum already has, plus three new kinds appended after
// resyncRequired's *following* two additions (discoveryRequest,
// discoveryResponse) — i.e. appended at the very end of the enum as it
// stands on this branch today:
//
//	unknown=0, ack=1, ping=2, connect=3, connectResponse=4,
//	requestState=5, requestStateResponse=6, requestStateResponseChunk=7,
//	sendChatMessage=8, receiveChatMessage=9, gameCommand=10,
//	desyncReport=11, companyAssignment=12, rosterUpdate=13,
//	serverClosing=14, resyncRequired=15, discoveryRequest=16,
//	discoveryResponse=17,
//	masterAnnounce=18, masterQuery=19, masterServerList=20  <- new, this file
//
// The game-side change must append these three in exactly this order
// (masterAnnounce, masterQuery, masterServerList) so the numeric values
// line up with what's hard-coded here.
type PacketKind uint16

const (
	KindMasterAnnounce   PacketKind = 18 // next after discoveryResponse=17
	KindMasterQuery      PacketKind = 19
	KindMasterServerList PacketKind = 20
)

// ---------------------------------------------------------------------------
// Payload sizes (fixed-layout, little-endian, matching Packet.h's
// #pragma pack(1) structs)
// ---------------------------------------------------------------------------

const (
	// kMaxNameLength mirrors Packet.h's kMaxRosterNameLength (31), reused
	// by masterAnnounce/masterServerList per the spec ("mirrors
	// RosterEntry").
	kMaxNameLength = 31

	// masterAnnouncePayloadSize: uint16 version + uint16 gamePort +
	// uint8 playerCount + uint8 maxPlayers + uint8 joinPolicy +
	// uint8 nameLength + char name[31].
	masterAnnouncePayloadSize = 2 + 2 + 1 + 1 + 1 + 1 + kMaxNameLength // 39

	// masterQueryPayloadSize: uint32 cookie.
	masterQueryPayloadSize = 4

	// serverListEntrySize: uint32 ipv4 (network byte order) + uint16 port +
	// uint16 version + uint8 playerCount + uint8 maxPlayers +
	// uint8 joinPolicy + uint8 nameLength + char name[31].
	serverListEntrySize = 4 + 2 + 2 + 1 + 1 + 1 + 1 + kMaxNameLength // 43

	// masterServerListHeaderSize: uint32 cookie + uint8 count.
	masterServerListHeaderSize = 4 + 1 // 5

	// kMaxServerListEntries is the cap on how many entries a single
	// masterServerList packet may carry, derived so the packet never
	// exceeds kMaxPacketDataSize (no amplification / no oversized UDP
	// writes):
	//
	//	available = kMaxPacketDataSize - masterServerListHeaderSize
	//	          = 4090 - 5 = 4085
	//	kMaxServerListEntries = floor(available / serverListEntrySize)
	//	                      = floor(4085 / 43) = 95  (95*43 = 4085, exact)
	kMaxServerListEntries = (kMaxPacketDataSize - masterServerListHeaderSize) / serverListEntrySize
)

// ---------------------------------------------------------------------------
// Framing helpers
// ---------------------------------------------------------------------------

var (
	errPayloadTooLarge = errors.New("master-server: payload exceeds kMaxPacketDataSize")
	errPacketTooShort  = errors.New("master-server: packet shorter than header")
	errPacketTruncated = errors.New("master-server: dataSize exceeds received bytes")
)

// EncodePacket frames a payload with a PacketHeader exactly as
// NetworkConnection::sendPacket / the discoveryResponse connectionless path
// do: kind, sequence, dataSize, then the raw payload bytes, all
// little-endian. sequence is 0 for connectionless kinds (mirrors
// discoveryResponse; the game ignores sequence for these kinds).
func EncodePacket(kind PacketKind, sequence uint16, payload []byte) ([]byte, error) {
	if len(payload) > kMaxPacketDataSize {
		return nil, errPayloadTooLarge
	}
	buf := make([]byte, packetHeaderSize+len(payload))
	binary.LittleEndian.PutUint16(buf[0:2], uint16(kind))
	binary.LittleEndian.PutUint16(buf[2:4], sequence)
	binary.LittleEndian.PutUint16(buf[4:6], uint16(len(payload)))
	copy(buf[6:], payload)
	return buf, nil
}

// DecodePacket parses a raw UDP datagram into its header fields and payload
// slice (a sub-slice of buf, not a copy). Returns false if the buffer is
// shorter than a header or the declared dataSize doesn't fit.
func DecodePacket(buf []byte) (kind PacketKind, sequence uint16, payload []byte, err error) {
	if len(buf) < packetHeaderSize {
		return 0, 0, nil, errPacketTooShort
	}
	kind = PacketKind(binary.LittleEndian.Uint16(buf[0:2]))
	sequence = binary.LittleEndian.Uint16(buf[2:4])
	dataSize := binary.LittleEndian.Uint16(buf[4:6])
	if int(dataSize) > len(buf)-packetHeaderSize {
		return kind, sequence, nil, errPacketTruncated
	}
	payload = buf[packetHeaderSize : packetHeaderSize+int(dataSize)]
	return kind, sequence, payload, nil
}

// ---------------------------------------------------------------------------
// masterAnnounce
// ---------------------------------------------------------------------------

// MasterAnnounce mirrors the masterAnnounce payload (game server -> master).
type MasterAnnounce struct {
	Version     uint16
	GamePort    uint16
	PlayerCount uint8
	MaxPlayers  uint8
	JoinPolicy  uint8
	Name        string // already trimmed to <= kMaxNameLength on decode
}

// DecodeMasterAnnounce parses a masterAnnounce payload. Returns false for a
// payload shorter than the fixed layout or a nameLength that would read past
// the fixed name[31] field (malformed/hostile input) — the caller should
// ignore the packet in that case.
func DecodeMasterAnnounce(payload []byte) (MasterAnnounce, bool) {
	if len(payload) < masterAnnouncePayloadSize {
		return MasterAnnounce{}, false
	}
	var a MasterAnnounce
	a.Version = binary.LittleEndian.Uint16(payload[0:2])
	a.GamePort = binary.LittleEndian.Uint16(payload[2:4])
	a.PlayerCount = payload[4]
	a.MaxPlayers = payload[5]
	a.JoinPolicy = payload[6]
	nameLength := payload[7]
	if int(nameLength) > kMaxNameLength {
		return MasterAnnounce{}, false
	}
	nameBytes := payload[8 : 8+kMaxNameLength]
	a.Name = string(nameBytes[:nameLength])
	return a, true
}

// EncodeMasterAnnounce serializes a MasterAnnounce (used by the integration
// test's client side to exercise the real wire format; the service itself
// only decodes this kind).
func EncodeMasterAnnounce(a MasterAnnounce) []byte {
	buf := make([]byte, masterAnnouncePayloadSize)
	binary.LittleEndian.PutUint16(buf[0:2], a.Version)
	binary.LittleEndian.PutUint16(buf[2:4], a.GamePort)
	buf[4] = a.PlayerCount
	buf[5] = a.MaxPlayers
	buf[6] = a.JoinPolicy
	name := a.Name
	if len(name) > kMaxNameLength {
		name = name[:kMaxNameLength]
	}
	buf[7] = uint8(len(name))
	copy(buf[8:8+kMaxNameLength], name)
	return buf
}

// ---------------------------------------------------------------------------
// masterQuery
// ---------------------------------------------------------------------------

// DecodeMasterQuery parses a masterQuery payload (browser -> master).
func DecodeMasterQuery(payload []byte) (cookie uint32, ok bool) {
	if len(payload) < masterQueryPayloadSize {
		return 0, false
	}
	return binary.LittleEndian.Uint32(payload[0:4]), true
}

// EncodeMasterQuery serializes a masterQuery payload (test client use).
func EncodeMasterQuery(cookie uint32) []byte {
	buf := make([]byte, masterQueryPayloadSize)
	binary.LittleEndian.PutUint32(buf[0:4], cookie)
	return buf
}

// ---------------------------------------------------------------------------
// masterServerList
// ---------------------------------------------------------------------------

// ServerListEntry mirrors one entry in the masterServerList payload.
type ServerListEntry struct {
	IPv4        [4]byte // network byte order, i.e. octet order (1.2.3.4 -> {1,2,3,4})
	Port        uint16
	Version     uint16
	PlayerCount uint8
	MaxPlayers  uint8
	JoinPolicy  uint8
	Name        string // truncated to <= kMaxNameLength on encode
}

// ServerListEntryFromIP builds a ServerListEntry's address field from a
// net.IP, rejecting non-IPv4 addresses.
func ServerListEntryFromIP(ip net.IP) ([4]byte, bool) {
	v4 := ip.To4()
	if v4 == nil {
		return [4]byte{}, false
	}
	var out [4]byte
	copy(out[:], v4)
	return out, true
}

// EncodeMasterServerList serializes a masterServerList payload (cookie echo
// + entries). entries beyond kMaxServerListEntries are silently dropped —
// callers should already have capped the slice, but this is a hard backstop
// against ever emitting an oversized packet (no amplification).
func EncodeMasterServerList(cookie uint32, entries []ServerListEntry) []byte {
	if len(entries) > kMaxServerListEntries {
		entries = entries[:kMaxServerListEntries]
	}
	buf := make([]byte, masterServerListHeaderSize+len(entries)*serverListEntrySize)
	binary.LittleEndian.PutUint32(buf[0:4], cookie)
	buf[4] = uint8(len(entries))
	off := masterServerListHeaderSize
	for _, e := range entries {
		copy(buf[off:off+4], e.IPv4[:])
		binary.LittleEndian.PutUint16(buf[off+4:off+6], e.Port)
		binary.LittleEndian.PutUint16(buf[off+6:off+8], e.Version)
		buf[off+8] = e.PlayerCount
		buf[off+9] = e.MaxPlayers
		buf[off+10] = e.JoinPolicy
		name := e.Name
		if len(name) > kMaxNameLength {
			name = name[:kMaxNameLength]
		}
		buf[off+11] = uint8(len(name))
		copy(buf[off+12:off+12+kMaxNameLength], name)
		off += serverListEntrySize
	}
	return buf
}

// DecodeMasterServerList parses a masterServerList payload (test client
// use / round-trip tests).
func DecodeMasterServerList(payload []byte) (cookie uint32, entries []ServerListEntry, err error) {
	if len(payload) < masterServerListHeaderSize {
		return 0, nil, errPacketTooShort
	}
	cookie = binary.LittleEndian.Uint32(payload[0:4])
	count := int(payload[4])
	need := masterServerListHeaderSize + count*serverListEntrySize
	if len(payload) < need {
		return 0, nil, fmt.Errorf("master-server: masterServerList declares %d entries but payload is too short", count)
	}
	entries = make([]ServerListEntry, 0, count)
	off := masterServerListHeaderSize
	for i := 0; i < count; i++ {
		var e ServerListEntry
		copy(e.IPv4[:], payload[off:off+4])
		e.Port = binary.LittleEndian.Uint16(payload[off+4 : off+6])
		e.Version = binary.LittleEndian.Uint16(payload[off+6 : off+8])
		e.PlayerCount = payload[off+8]
		e.MaxPlayers = payload[off+9]
		e.JoinPolicy = payload[off+10]
		nameLength := payload[off+11]
		if int(nameLength) > kMaxNameLength {
			return 0, nil, errors.New("master-server: entry nameLength exceeds kMaxNameLength")
		}
		nameBytes := payload[off+12 : off+12+kMaxNameLength]
		e.Name = string(nameBytes[:nameLength])
		entries = append(entries, e)
		off += serverListEntrySize
	}
	return cookie, entries, nil
}
