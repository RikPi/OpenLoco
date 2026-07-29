package main

import (
	"context"
	"io"
	"log"
	"net"
	"testing"
	"time"
)

// startTestServer spins up a real UDPServer on an ephemeral loopback port,
// backed by the given Registry, and returns the listening address plus a
// cleanup func. Logs are discarded (tests assert on protocol behavior, not
// log lines).
func startTestServer(t *testing.T, registry *Registry) (*net.UDPAddr, func()) {
	t.Helper()

	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.ParseIP("127.0.0.1"), Port: 0})
	if err != nil {
		t.Fatalf("ListenUDP: %v", err)
	}

	server := &UDPServer{
		Registry: registry,
		Log:      log.New(io.Discard, "", 0),
		Verbose:  true,
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		defer close(done)
		server.Serve(ctx, conn)
	}()

	cleanup := func() {
		cancel()
		_ = conn.Close()
		<-done
	}
	return conn.LocalAddr().(*net.UDPAddr), cleanup
}

// TestIntegrationAnnounceThenQuery drives the full announce -> announce
// (refresh) -> query -> masterServerList round trip over a real UDP socket,
// end to end through the wire encoding.
func TestIntegrationAnnounceThenQuery(t *testing.T) {
	registry := NewRegistry(90*time.Second, 1024, 5)
	serverAddr, cleanup := startTestServer(t, registry)
	defer cleanup()

	client, err := net.DialUDP("udp4", nil, serverAddr)
	if err != nil {
		t.Fatalf("DialUDP: %v", err)
	}
	defer client.Close()
	client.SetDeadline(time.Now().Add(5 * time.Second))

	sendAnnounce := func(name string) {
		payload := EncodeMasterAnnounce(MasterAnnounce{
			Version:     7,
			GamePort:    11754,
			PlayerCount: 2,
			MaxPlayers:  32,
			JoinPolicy:  1,
			Name:        name,
		})
		framed, err := EncodePacket(KindMasterAnnounce, 0, payload)
		if err != nil {
			t.Fatalf("EncodePacket(masterAnnounce): %v", err)
		}
		if _, err := client.Write(framed); err != nil {
			t.Fatalf("write announce: %v", err)
		}
	}

	// Announce twice from the same (client) IP - the second is a refresh,
	// not a second entry, since both share the same source IP and
	// advertised game port.
	sendAnnounce("Integration Server")
	sendAnnounce("Integration Server")

	// Announces are one-way (no reply) - give the server loop a moment to
	// process both before querying, polling instead of a fixed sleep.
	deadline := time.Now().Add(2 * time.Second)
	for registry.Len() != 1 && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	if got := registry.Len(); got != 1 {
		t.Fatalf("registry.Len() = %d, want 1 (second announce should refresh, not duplicate)", got)
	}

	// Now query and expect a masterServerList with exactly that one entry.
	queryPayload := EncodeMasterQuery(0xCAFEF00D)
	queryFramed, err := EncodePacket(KindMasterQuery, 0, queryPayload)
	if err != nil {
		t.Fatalf("EncodePacket(masterQuery): %v", err)
	}
	if _, err := client.Write(queryFramed); err != nil {
		t.Fatalf("write query: %v", err)
	}

	respBuf := make([]byte, kMaxPacketSize)
	n, err := client.Read(respBuf)
	if err != nil {
		t.Fatalf("read masterServerList reply: %v", err)
	}

	kind, seq, payload, err := DecodePacket(respBuf[:n])
	if err != nil {
		t.Fatalf("DecodePacket(reply): %v", err)
	}
	if kind != KindMasterServerList {
		t.Fatalf("reply kind = %d, want %d", kind, KindMasterServerList)
	}
	if seq != 0 {
		t.Fatalf("reply sequence = %d, want 0 (connectionless, mirrors discoveryResponse)", seq)
	}

	cookie, entries, err := DecodeMasterServerList(payload)
	if err != nil {
		t.Fatalf("DecodeMasterServerList: %v", err)
	}
	if cookie != 0xCAFEF00D {
		t.Fatalf("cookie = %#x, want %#x", cookie, 0xCAFEF00D)
	}
	if len(entries) != 1 {
		t.Fatalf("got %d entries, want 1", len(entries))
	}
	got := entries[0]
	if got.Name != "Integration Server" {
		t.Errorf("Name = %q, want %q", got.Name, "Integration Server")
	}
	if got.Port != 11754 {
		t.Errorf("Port = %d, want 11754", got.Port)
	}
	if got.Version != 7 {
		t.Errorf("Version = %d, want 7", got.Version)
	}
	loopback, _ := ServerListEntryFromIP(net.ParseIP("127.0.0.1"))
	if got.IPv4 != loopback {
		t.Errorf("IPv4 = %v, want %v (observed source address)", got.IPv4, loopback)
	}
}

// TestIntegrationMalformedAndUnknownPacketsAreIgnored checks that garbage
// and unrelated packet kinds neither crash the server nor produce a reply,
// and that a subsequent well-formed query still works afterwards.
func TestIntegrationMalformedAndUnknownPacketsAreIgnored(t *testing.T) {
	registry := NewRegistry(90*time.Second, 1024, 5)
	serverAddr, cleanup := startTestServer(t, registry)
	defer cleanup()

	client, err := net.DialUDP("udp4", nil, serverAddr)
	if err != nil {
		t.Fatalf("DialUDP: %v", err)
	}
	defer client.Close()

	// Too-short garbage.
	if _, err := client.Write([]byte{1, 2}); err != nil {
		t.Fatalf("write garbage: %v", err)
	}
	// A well-framed but unrelated kind (ping=2).
	framed, _ := EncodePacket(PacketKind(2), 0, []byte{0, 0, 0, 0})
	if _, err := client.Write(framed); err != nil {
		t.Fatalf("write ping: %v", err)
	}

	// A well-formed query should still get an (empty) list reply -
	// proves the server loop survived the garbage above.
	queryFramed, _ := EncodePacket(KindMasterQuery, 0, EncodeMasterQuery(42))
	client.SetDeadline(time.Now().Add(3 * time.Second))
	if _, err := client.Write(queryFramed); err != nil {
		t.Fatalf("write query: %v", err)
	}

	respBuf := make([]byte, kMaxPacketSize)
	n, err := client.Read(respBuf)
	if err != nil {
		t.Fatalf("server did not reply to a well-formed query after malformed input: %v", err)
	}
	kind, _, payload, err := DecodePacket(respBuf[:n])
	if err != nil {
		t.Fatalf("DecodePacket: %v", err)
	}
	if kind != KindMasterServerList {
		t.Fatalf("kind = %d, want %d", kind, KindMasterServerList)
	}
	cookie, entries, err := DecodeMasterServerList(payload)
	if err != nil {
		t.Fatalf("DecodeMasterServerList: %v", err)
	}
	if cookie != 42 {
		t.Errorf("cookie = %d, want 42", cookie)
	}
	if len(entries) != 0 {
		t.Errorf("got %d entries, want 0 (nothing was ever announced)", len(entries))
	}
}
