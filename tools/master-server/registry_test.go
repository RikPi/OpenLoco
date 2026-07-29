package main

import (
	"net"
	"testing"
	"time"
)

func announce(port uint16, name string) MasterAnnounce {
	return MasterAnnounce{
		Version:     7,
		GamePort:    port,
		PlayerCount: 1,
		MaxPlayers:  32,
		JoinPolicy:  1,
		Name:        name,
	}
}

func TestRegistryUpsertAndList(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	now := time.Now()

	ip := net.ParseIP("203.0.113.1")
	if result := r.Upsert(ip, announce(11754, "Server A"), now); result != UpsertOK {
		t.Fatalf("Upsert result = %v, want UpsertOK", result)
	}
	if r.Len() != 1 {
		t.Fatalf("Len() = %d, want 1", r.Len())
	}

	list := r.List()
	if len(list) != 1 || list[0].Name != "Server A" || list[0].Port != 11754 {
		t.Fatalf("unexpected list contents: %+v", list)
	}
}

func TestRegistryUpsertRefreshesExistingEntry(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	ip := net.ParseIP("203.0.113.1")
	t0 := time.Now()

	r.Upsert(ip, announce(11754, "Server A"), t0)
	t1 := t0.Add(30 * time.Second)
	r.Upsert(ip, announce(11754, "Server A renamed"), t1)

	if r.Len() != 1 {
		t.Fatalf("Len() = %d, want 1 (upsert of the same key must not create a second entry)", r.Len())
	}
	list := r.List()
	if list[0].Name != "Server A renamed" {
		t.Errorf("Name = %q, want refreshed name", list[0].Name)
	}
	if !list[0].LastSeen.Equal(t1) {
		t.Errorf("LastSeen = %v, want %v", list[0].LastSeen, t1)
	}
}

func TestRegistrySamePortDifferentIPAreDistinctEntries(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	now := time.Now()
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "A"), now)
	r.Upsert(net.ParseIP("203.0.113.2"), announce(11754, "B"), now)
	if r.Len() != 2 {
		t.Fatalf("Len() = %d, want 2", r.Len())
	}
}

func TestRegistrySameIPDifferentPortAreDistinctEntries(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	now := time.Now()
	ip := net.ParseIP("203.0.113.1")
	r.Upsert(ip, announce(11754, "A"), now)
	r.Upsert(ip, announce(11755, "B"), now)
	if r.Len() != 2 {
		t.Fatalf("Len() = %d, want 2 (one host, two game servers on different ports)", r.Len())
	}
}

func TestRegistryRejectsNonIPv4(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	result := r.Upsert(net.ParseIP("::1"), announce(11754, "A"), time.Now())
	if result != UpsertRejectedNotIPv4 {
		t.Fatalf("result = %v, want UpsertRejectedNotIPv4", result)
	}
	if r.Len() != 0 {
		t.Fatalf("Len() = %d, want 0", r.Len())
	}
}

func TestRegistryPerIPCap(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 2)
	ip := net.ParseIP("203.0.113.1")
	now := time.Now()

	if res := r.Upsert(ip, announce(11754, "A"), now); res != UpsertOK {
		t.Fatalf("first upsert result = %v", res)
	}
	if res := r.Upsert(ip, announce(11755, "B"), now); res != UpsertOK {
		t.Fatalf("second upsert result = %v", res)
	}
	// Third distinct port from the same IP should be rejected (cap=2).
	if res := r.Upsert(ip, announce(11756, "C"), now); res != UpsertRejectedPerIPCap {
		t.Fatalf("third upsert result = %v, want UpsertRejectedPerIPCap", res)
	}
	if r.Len() != 2 {
		t.Fatalf("Len() = %d, want 2", r.Len())
	}
	// Refreshing an existing entry must still succeed even at the cap.
	if res := r.Upsert(ip, announce(11754, "A refreshed"), now.Add(time.Second)); res != UpsertOK {
		t.Fatalf("refresh at cap result = %v, want UpsertOK", res)
	}
}

func TestRegistryMaxEntriesBound(t *testing.T) {
	r := NewRegistry(90*time.Second, 2, 100)
	now := time.Now()
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "A"), now)
	r.Upsert(net.ParseIP("203.0.113.2"), announce(11754, "B"), now)
	if res := r.Upsert(net.ParseIP("203.0.113.3"), announce(11754, "C"), now); res != UpsertRejectedRegistryFull {
		t.Fatalf("result = %v, want UpsertRejectedRegistryFull", res)
	}
	if r.Len() != 2 {
		t.Fatalf("Len() = %d, want 2 (bounded)", r.Len())
	}
}

func TestRegistryTTLExpiry(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	t0 := time.Now()
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "A"), t0)

	// Just under TTL: still present.
	if removed := r.Sweep(t0.Add(89 * time.Second)); removed != 0 {
		t.Fatalf("Sweep removed %d entries before TTL elapsed", removed)
	}
	if r.Len() != 1 {
		t.Fatalf("Len() = %d, want 1 before TTL", r.Len())
	}

	// Past TTL: expired.
	if removed := r.Sweep(t0.Add(91 * time.Second)); removed != 1 {
		t.Fatalf("Sweep removed %d entries, want 1", removed)
	}
	if r.Len() != 0 {
		t.Fatalf("Len() = %d, want 0 after TTL sweep", r.Len())
	}
}

func TestRegistryTTLExpiryRefreshResetsClock(t *testing.T) {
	r := NewRegistry(10*time.Second, 1024, 5)
	t0 := time.Now()
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "A"), t0)

	// Refresh right before it would expire.
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "A"), t0.Add(9*time.Second))

	// 15s after the original announce (>10s TTL from t0, but <10s from the
	// refresh) - must still be alive because of the refresh.
	if removed := r.Sweep(t0.Add(15 * time.Second)); removed != 0 {
		t.Fatalf("Sweep removed %d entries; refresh should have reset the TTL clock", removed)
	}
}

func TestRegistryListIsSortedDeterministically(t *testing.T) {
	r := NewRegistry(90*time.Second, 1024, 5)
	now := time.Now()
	r.Upsert(net.ParseIP("203.0.113.5"), announce(11754, "Z"), now)
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11760, "A"), now)
	r.Upsert(net.ParseIP("203.0.113.1"), announce(11754, "B"), now)

	list := r.List()
	if len(list) != 3 {
		t.Fatalf("len(list) = %d, want 3", len(list))
	}
	// 203.0.113.1:11754 < 203.0.113.1:11760 < 203.0.113.5:*
	if list[0].Name != "B" || list[1].Name != "A" || list[2].Name != "Z" {
		t.Fatalf("unexpected order: %+v", list)
	}
}
