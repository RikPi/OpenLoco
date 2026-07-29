package main

import (
	"net"
	"sort"
	"sync"
	"time"
)

// entryKey identifies a registry entry: the observed source IP (the
// announce's public address, per docs/multiplayer.md § Master server) plus
// the advertised game port. Keying on the *advertised* port (not the
// announce's source port, which is ephemeral/irrelevant) lets one host
// behind one IP run multiple game servers on different ports.
type entryKey struct {
	ip   [4]byte
	port uint16
}

// Entry is one registered game server.
type Entry struct {
	IP          [4]byte
	Port        uint16
	Version     uint16
	PlayerCount uint8
	MaxPlayers  uint8
	JoinPolicy  uint8
	Name        string
	LastSeen    time.Time
}

// Registry is the in-memory, TTL-expiring server list. Stateless by design
// (docs/multiplayer.md § Master server): no database, no persistence across
// restarts. Safe for concurrent use.
type Registry struct {
	mu         sync.Mutex
	entries    map[entryKey]Entry
	ttl        time.Duration
	maxEntries int
	maxPerIP   int
}

// NewRegistry constructs an empty Registry.
func NewRegistry(ttl time.Duration, maxEntries, maxPerIP int) *Registry {
	return &Registry{
		entries:    make(map[entryKey]Entry),
		ttl:        ttl,
		maxEntries: maxEntries,
		maxPerIP:   maxPerIP,
	}
}

// UpsertResult explains what Upsert did, for logging.
type UpsertResult int

const (
	UpsertOK UpsertResult = iota
	UpsertRejectedNotIPv4
	UpsertRejectedPerIPCap
	UpsertRejectedRegistryFull
)

// Upsert records (or refreshes) an announce. Keyed by (observed source IP,
// advertised game port) — see entryKey. Enforces the per-IP cap and the
// overall registry bound; both are checked only when this is a *new* key
// (an existing entry refreshing its TTL never gets rejected by either cap,
// matching "upsert" semantics).
func (r *Registry) Upsert(sourceIP net.IP, ann MasterAnnounce, now time.Time) UpsertResult {
	v4 := sourceIP.To4()
	if v4 == nil {
		return UpsertRejectedNotIPv4
	}
	var ip [4]byte
	copy(ip[:], v4)
	key := entryKey{ip: ip, port: ann.GamePort}

	r.mu.Lock()
	defer r.mu.Unlock()

	if _, exists := r.entries[key]; !exists {
		if len(r.entries) >= r.maxEntries {
			return UpsertRejectedRegistryFull
		}
		if r.countByIP(ip) >= r.maxPerIP {
			return UpsertRejectedPerIPCap
		}
	}

	r.entries[key] = Entry{
		IP:          ip,
		Port:        ann.GamePort,
		Version:     ann.Version,
		PlayerCount: ann.PlayerCount,
		MaxPlayers:  ann.MaxPlayers,
		JoinPolicy:  ann.JoinPolicy,
		Name:        ann.Name,
		LastSeen:    now,
	}
	return UpsertOK
}

// countByIP counts current entries for the given IP. Caller must hold r.mu.
func (r *Registry) countByIP(ip [4]byte) int {
	n := 0
	for k := range r.entries {
		if k.ip == ip {
			n++
		}
	}
	return n
}

// Sweep removes entries whose LastSeen is older than the TTL relative to
// now. Returns the number of entries removed.
func (r *Registry) Sweep(now time.Time) int {
	r.mu.Lock()
	defer r.mu.Unlock()
	removed := 0
	for k, e := range r.entries {
		if now.Sub(e.LastSeen) > r.ttl {
			delete(r.entries, k)
			removed++
		}
	}
	return removed
}

// List returns a snapshot of all current entries, sorted by (IP, port) for
// deterministic output (stable JSON/wire output across calls with the same
// registry contents).
func (r *Registry) List() []Entry {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]Entry, 0, len(r.entries))
	for _, e := range r.entries {
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].IP != out[j].IP {
			return string(out[i].IP[:]) < string(out[j].IP[:])
		}
		return out[i].Port < out[j].Port
	})
	return out
}

// Len returns the current entry count.
func (r *Registry) Len() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.entries)
}
