// Command master-server is the OpenLoco internet server list (phase 2 of
// the lobby design, docs/multiplayer.md § "Master server (phase 2 —
// design)"). It speaks the game's own UDP packet framing (Packet.h) instead
// of HTTP so a hosting game server and the in-game browser can reuse their
// existing, tested wire code. It is stateless: an in-memory registry with
// TTL expiry, no database, no persistence across restarts.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"sync"
	"syscall"
	"time"
)

func main() {
	udpPort := flag.Int("udp-port", 11756, "UDP port to listen on for masterAnnounce/masterQuery packets")
	httpPort := flag.Int("http-port", 8080, "HTTP port for GET /servers and /healthz (0 disables the HTTP server)")
	ttl := flag.Duration("ttl", 90*time.Second, "how long an entry survives without a fresh announce")
	maxEntries := flag.Int("max-entries", 1024, "maximum number of registry entries")
	maxPerIP := flag.Int("max-per-ip", 5, "maximum number of registry entries per source IP")
	verbose := flag.Bool("verbose", false, "log ignored/rejected packets and other low-level detail")
	flag.Parse()

	udpLog := log.New(os.Stdout, "[udp] ", log.LstdFlags)
	httpLog := log.New(os.Stdout, "[http] ", log.LstdFlags)
	mainLog := log.New(os.Stdout, "[main] ", log.LstdFlags)

	registry := NewRegistry(*ttl, *maxEntries, *maxPerIP)

	ctx, cancel := context.WithCancel(context.Background())

	server := &UDPServer{
		Registry: registry,
		Log:      udpLog,
		Verbose:  *verbose,
	}
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{Port: *udpPort})
	if err != nil {
		mainLog.Fatalf("failed to listen on UDP :%d: %v", *udpPort, err)
	}
	mainLog.Printf("listening for masterAnnounce/masterQuery on udp :%d", *udpPort)

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		server.Serve(ctx, conn)
	}()

	// TTL sweeper.
	wg.Add(1)
	go func() {
		defer wg.Done()
		sweepInterval := *ttl / 4
		if sweepInterval <= 0 {
			sweepInterval = time.Second
		}
		ticker := time.NewTicker(sweepInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case now := <-ticker.C:
				if n := registry.Sweep(now); n > 0 {
					mainLog.Printf("expired %d stale entr(y/ies)", n)
				}
			}
		}
	}()

	var httpSrv *http.Server
	if *httpPort != 0 {
		mux := http.NewServeMux()
		mux.HandleFunc("/servers", func(w http.ResponseWriter, r *http.Request) {
			handleServers(w, r, registry)
		})
		mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte("ok"))
		})
		httpSrv = &http.Server{
			Addr:    net.JoinHostPort("", strconv.Itoa(*httpPort)),
			Handler: mux,
		}
		wg.Add(1)
		go func() {
			defer wg.Done()
			httpLog.Printf("listening on http :%d (GET /servers, GET /healthz)", *httpPort)
			if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
				httpLog.Printf("http server error: %v", err)
			}
		}()
	} else {
		mainLog.Printf("http server disabled (-http-port=0)")
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	<-sigCh
	mainLog.Printf("shutting down...")

	cancel()
	_ = conn.Close()
	if httpSrv != nil {
		shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer shutdownCancel()
		_ = httpSrv.Shutdown(shutdownCtx)
	}
	wg.Wait()
	mainLog.Printf("shutdown complete")
}

// serverJSON is the JSON shape of a GET /servers entry.
type serverJSON struct {
	Name               string `json:"name"`
	Address            string `json:"address"`
	Port               uint16 `json:"port"`
	Version            uint16 `json:"version"`
	Players            uint8  `json:"players"`
	MaxPlayers         uint8  `json:"maxPlayers"`
	JoinPolicy         uint8  `json:"joinPolicy"`
	LastSeenSecondsAgo int64  `json:"lastSeenSecondsAgo"`
}

func handleServers(w http.ResponseWriter, r *http.Request, registry *Registry) {
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}
	now := time.Now()
	entries := registry.List()
	out := make([]serverJSON, 0, len(entries))
	for _, e := range entries {
		out = append(out, serverJSON{
			Name:               e.Name,
			Address:            net.IP(e.IP[:]).String(),
			Port:               e.Port,
			Version:            e.Version,
			Players:            e.PlayerCount,
			MaxPlayers:         e.MaxPlayers,
			JoinPolicy:         e.JoinPolicy,
			LastSeenSecondsAgo: int64(now.Sub(e.LastSeen).Seconds()),
		})
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(out)
}
