package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"sync"
	"time"
)

const computeIDs = 128
const computeRetention = 10 * time.Minute

type computeEntry struct {
	fingerprint [32]byte
	expires     time.Time
	done        bool
	status      int
	response    rpcResponse
}

type computeService struct {
	mu      sync.Mutex
	entries map[string]*computeEntry
	runs    uint64
	now     func() time.Time
}

func newComputeService() *computeService {
	return &computeService{entries: make(map[string]*computeEntry), now: time.Now}
}

func (s *computeService) digest(ctx context.Context, req rpcRequest) (int, rpcResponse) {
	var args struct {
		Text   string `json:"text"`
		Rounds int    `json:"rounds"`
	}
	failure := func(code, message string) rpcResponse {
		return rpcResponse{Error: &rpcError{Code: code, Message: message}}
	}
	if decodeStrict(req.Args, &args) != nil || !validText(args.Text, 512) ||
		args.Rounds < 1 || args.Rounds > 1000000 {
		return http.StatusBadRequest, failure("bad_request", "digest requires text (1..512 bytes) and rounds (1..1000000)")
	}
	canonical, _ := json.Marshal(args)
	fingerprint := sha256.Sum256(canonical)
	s.mu.Lock()
	now := s.now()
	for id, entry := range s.entries {
		if entry.done && !entry.expires.After(now) {
			delete(s.entries, id)
		}
	}
	if entry := s.entries[req.RequestID]; entry != nil {
		defer s.mu.Unlock()
		if entry.fingerprint != fingerprint {
			return http.StatusConflict, failure("request_id_conflict", "request ID already names different digest arguments")
		}
		if !entry.done {
			return http.StatusConflict, failure("request_in_progress", "request ID is still computing; query the same ID later")
		}
		return entry.status, entry.response
	}
	if len(s.entries) >= computeIDs {
		s.mu.Unlock()
		return http.StatusServiceUnavailable, failure("busy", "digest request-ID budget is full; wait for expiry")
	}
	entry := &computeEntry{fingerprint: fingerprint}
	s.entries[req.RequestID] = entry
	s.runs++
	s.mu.Unlock()
	status := http.StatusOK
	response := rpcResponse{}
	digest := sha256.Sum256([]byte(args.Text))
	for n := 1; n < args.Rounds; n++ {
		if n%1024 == 0 && ctx.Err() != nil {
			break
		}
		digest = sha256.Sum256(digest[:])
	}
	if ctx.Err() != nil {
		status = http.StatusRequestTimeout
		response = failure("cancelled", "digest cancelled; this request ID retains the cancelled result")
	} else {
		response = rpcResponse{OK: true, Result: map[string]any{
			"sha256": hex.EncodeToString(digest[:]), "rounds": args.Rounds,
		}}
	}
	s.mu.Lock()
	entry.status, entry.response, entry.done = status, response, true
	entry.expires = s.now().Add(computeRetention)
	s.mu.Unlock()
	return status, response
}
