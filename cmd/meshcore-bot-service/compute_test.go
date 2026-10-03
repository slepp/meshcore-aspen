package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestComputeRequestIDContract(t *testing.T) {
	s := newComputeService()
	now := time.Now()
	s.now = func() time.Time { return now }
	req := rpcRequest{Operation: "digest", RequestID: "node-boot-job", Args: json.RawMessage(`{"text":"mesh","rounds":1000}`)}
	status, first := s.digest(context.Background(), req)
	if status != 200 || !first.OK || s.runs != 1 {
		t.Fatalf("first digest: %d %+v runs=%d", status, first, s.runs)
	}
	req.Args = json.RawMessage(`{"rounds":1000, "text":"mesh"}`)
	status, repeated := s.digest(context.Background(), req)
	if status != 200 || !repeated.OK || s.runs != 1 {
		t.Fatalf("replay recomputed: %d %+v runs=%d", status, repeated, s.runs)
	}
	req.Args = json.RawMessage(`{"text":"different","rounds":1000}`)
	if status, result := s.digest(context.Background(), req); status != 409 || result.Error.Code != "request_id_conflict" {
		t.Fatalf("conflicting ID accepted: %d %+v", status, result)
	}
	now = now.Add(computeRetention + time.Second)
	status, _ = s.digest(context.Background(), req)
	if status != 200 || s.runs != 2 {
		t.Fatal("completed ID did not expire")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	req.RequestID = "cancelled"
	status, result := s.digest(ctx, req)
	if status != 408 || result.Error.Code != "cancelled" {
		t.Fatalf("cancellation: %d %+v", status, result)
	}
	status, result = s.digest(context.Background(), req)
	if status != 408 || result.Error.Code != "cancelled" || s.runs != 3 {
		t.Fatal("cancelled request was replayed")
	}
}

func TestComputeConcurrentDuplicatesAndBudget(t *testing.T) {
	s := newComputeService()
	req := rpcRequest{RequestID: "shared", Args: json.RawMessage(`{"text":"mesh","rounds":1000000}`)}
	var calls sync.WaitGroup
	for i := 0; i < 8; i++ {
		calls.Add(1)
		go func() {
			defer calls.Done()
			status, result := s.digest(context.Background(), req)
			if status != 200 && (status != 409 || result.Error.Code != "request_in_progress") {
				t.Errorf("duplicate: %d %+v", status, result)
			}
		}()
	}
	calls.Wait()
	if s.runs != 1 {
		t.Fatalf("duplicate executions=%d", s.runs)
	}
	for i := len(s.entries); i < computeIDs; i++ {
		s.entries[strings.Repeat("x", i+1)] = &computeEntry{}
	}
	req.RequestID = "full"
	if status, result := s.digest(context.Background(), req); status != 503 || result.Error.Code != "busy" {
		t.Fatalf("unbounded ID store: %d %+v", status, result)
	}
}

func TestComputeOptInHTTP(t *testing.T) {
	server := newRPCServer("", false, nil)
	body := `{"operation":"digest","request_id":"example-1","args":{"text":"mesh","rounds":1000}}`
	call := func() *httptest.ResponseRecorder {
		request := httptest.NewRequest(http.MethodPost, "/v1/rpc", strings.NewReader(body))
		request.Header.Set("Content-Type", "application/json")
		result := httptest.NewRecorder()
		server.ServeHTTP(result, request)
		return result
	}
	if result := call(); result.Code != 503 || !strings.Contains(result.Body.String(), "compute_disabled") {
		t.Fatal("compute enabled by default")
	}
	server.compute = newComputeService()
	first, second := call(), call()
	if first.Code != 200 || first.Body.String() != second.Body.String() || server.compute.runs != 1 {
		t.Fatal("HTTP replay did not return saved digest")
	}
}
