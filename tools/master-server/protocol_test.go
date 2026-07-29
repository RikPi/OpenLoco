package main

import (
	"net"
	"strings"
	"testing"
)

func TestPacketRoundTrip(t *testing.T) {
	payload := []byte{1, 2, 3, 4, 5}
	framed, err := EncodePacket(KindMasterQuery, 0, payload)
	if err != nil {
		t.Fatalf("EncodePacket: %v", err)
	}
	if len(framed) != packetHeaderSize+len(payload) {
		t.Fatalf("unexpected framed length %d", len(framed))
	}

	kind, seq, gotPayload, err := DecodePacket(framed)
	if err != nil {
		t.Fatalf("DecodePacket: %v", err)
	}
	if kind != KindMasterQuery {
		t.Errorf("kind = %d, want %d", kind, KindMasterQuery)
	}
	if seq != 0 {
		t.Errorf("sequence = %d, want 0", seq)
	}
	if string(gotPayload) != string(payload) {
		t.Errorf("payload = %v, want %v", gotPayload, payload)
	}
}

func TestDecodePacketTooShort(t *testing.T) {
	if _, _, _, err := DecodePacket([]byte{1, 2, 3}); err == nil {
		t.Fatal("expected error for undersized buffer")
	}
}

func TestDecodePacketTruncatedDataSize(t *testing.T) {
	// Claims dataSize=100 but the buffer doesn't have that many bytes.
	buf, _ := EncodePacket(KindMasterQuery, 0, []byte{1, 2})
	buf[4] = 100
	buf[5] = 0
	if _, _, _, err := DecodePacket(buf); err == nil {
		t.Fatal("expected error for truncated dataSize")
	}
}

func TestEncodePacketRejectsOversizedPayload(t *testing.T) {
	oversized := make([]byte, kMaxPacketDataSize+1)
	if _, err := EncodePacket(KindMasterAnnounce, 0, oversized); err == nil {
		t.Fatal("expected error for oversized payload")
	}
}

func TestMasterAnnounceRoundTrip(t *testing.T) {
	want := MasterAnnounce{
		Version:     7,
		GamePort:    11754,
		PlayerCount: 3,
		MaxPlayers:  32,
		JoinPolicy:  1,
		Name:        "Test Server",
	}
	encoded := EncodeMasterAnnounce(want)
	if len(encoded) != masterAnnouncePayloadSize {
		t.Fatalf("encoded length = %d, want %d", len(encoded), masterAnnouncePayloadSize)
	}
	got, ok := DecodeMasterAnnounce(encoded)
	if !ok {
		t.Fatal("DecodeMasterAnnounce returned false")
	}
	if got != want {
		t.Errorf("got %+v, want %+v", got, want)
	}
}

func TestMasterAnnounceNameTrimming(t *testing.T) {
	longName := strings.Repeat("x", 100)
	encoded := EncodeMasterAnnounce(MasterAnnounce{Name: longName})
	got, ok := DecodeMasterAnnounce(encoded)
	if !ok {
		t.Fatal("DecodeMasterAnnounce returned false")
	}
	if len(got.Name) != kMaxNameLength {
		t.Errorf("name length = %d, want %d", len(got.Name), kMaxNameLength)
	}
	if got.Name != strings.Repeat("x", kMaxNameLength) {
		t.Errorf("unexpected trimmed name: %q", got.Name)
	}
}

func TestDecodeMasterAnnounceRejectsBadNameLength(t *testing.T) {
	encoded := EncodeMasterAnnounce(MasterAnnounce{Name: "ok"})
	encoded[7] = kMaxNameLength + 10 // corrupt nameLength beyond the fixed field
	if _, ok := DecodeMasterAnnounce(encoded); ok {
		t.Fatal("expected DecodeMasterAnnounce to reject an out-of-range nameLength")
	}
}

func TestDecodeMasterAnnounceRejectsShortPayload(t *testing.T) {
	if _, ok := DecodeMasterAnnounce([]byte{1, 2, 3}); ok {
		t.Fatal("expected DecodeMasterAnnounce to reject a short payload")
	}
}

func TestMasterQueryRoundTrip(t *testing.T) {
	encoded := EncodeMasterQuery(0xDEADBEEF)
	cookie, ok := DecodeMasterQuery(encoded)
	if !ok {
		t.Fatal("DecodeMasterQuery returned false")
	}
	if cookie != 0xDEADBEEF {
		t.Errorf("cookie = %#x, want %#x", cookie, 0xDEADBEEF)
	}
}

func TestMasterServerListRoundTrip(t *testing.T) {
	ip, _ := ServerListEntryFromIP(net.ParseIP("192.0.2.1"))
	entries := []ServerListEntry{
		{IPv4: ip, Port: 11754, Version: 7, PlayerCount: 2, MaxPlayers: 32, JoinPolicy: 1, Name: "Server One"},
		{IPv4: ip, Port: 11755, Version: 7, PlayerCount: 0, MaxPlayers: 8, JoinPolicy: 0, Name: "Server Two"},
	}
	encoded := EncodeMasterServerList(12345, entries)

	cookie, got, err := DecodeMasterServerList(encoded)
	if err != nil {
		t.Fatalf("DecodeMasterServerList: %v", err)
	}
	if cookie != 12345 {
		t.Errorf("cookie = %d, want 12345", cookie)
	}
	if len(got) != len(entries) {
		t.Fatalf("got %d entries, want %d", len(got), len(entries))
	}
	for i := range entries {
		if got[i] != entries[i] {
			t.Errorf("entry %d = %+v, want %+v", i, got[i], entries[i])
		}
	}
}

func TestMasterServerListIPv4NetworkByteOrder(t *testing.T) {
	ip, ok := ServerListEntryFromIP(net.ParseIP("1.2.3.4"))
	if !ok {
		t.Fatal("ServerListEntryFromIP returned false for a valid IPv4")
	}
	want := [4]byte{1, 2, 3, 4}
	if ip != want {
		t.Errorf("ipv4 bytes = %v, want %v (network/octet byte order)", ip, want)
	}
}

func TestServerListEntryFromIPRejectsIPv6(t *testing.T) {
	if _, ok := ServerListEntryFromIP(net.ParseIP("::1")); ok {
		t.Fatal("expected ServerListEntryFromIP to reject an IPv6 address")
	}
}

func TestEncodeMasterServerListCapsEntries(t *testing.T) {
	ip, _ := ServerListEntryFromIP(net.ParseIP("10.0.0.1"))
	entries := make([]ServerListEntry, kMaxServerListEntries+10)
	for i := range entries {
		entries[i] = ServerListEntry{IPv4: ip, Port: uint16(i)}
	}
	encoded := EncodeMasterServerList(1, entries)
	if len(encoded) > kMaxPacketDataSize {
		t.Fatalf("encoded masterServerList payload is %d bytes, exceeds kMaxPacketDataSize %d", len(encoded), kMaxPacketDataSize)
	}
	_, got, err := DecodeMasterServerList(encoded)
	if err != nil {
		t.Fatalf("DecodeMasterServerList: %v", err)
	}
	if len(got) != kMaxServerListEntries {
		t.Errorf("got %d entries, want the cap %d", len(got), kMaxServerListEntries)
	}
}

func TestMaxServerListEntriesStaysWithinPacketBudget(t *testing.T) {
	// Exercise the boundary directly: the cap plus its framing must fit,
	// and one more entry than the cap must not.
	full, _ := EncodePacket(KindMasterServerList, 0, make([]byte, masterServerListHeaderSize+kMaxServerListEntries*serverListEntrySize))
	if len(full) > kMaxPacketSize {
		t.Fatalf("a full masterServerList packet is %d bytes, exceeds kMaxPacketSize %d", len(full), kMaxPacketSize)
	}
	oversizedPayloadLen := masterServerListHeaderSize + (kMaxServerListEntries+1)*serverListEntrySize
	if oversizedPayloadLen <= kMaxPacketDataSize {
		t.Fatalf("kMaxServerListEntries=%d is not actually the tightest cap for kMaxPacketDataSize=%d", kMaxServerListEntries, kMaxPacketDataSize)
	}
}
