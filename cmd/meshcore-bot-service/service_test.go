package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func rpcCall(t *testing.T, client *http.Client, endpoint, body, token string) (int, rpcResponse, []byte) {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, endpoint+"/v1/rpc", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Content-Type", "application/json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatal(err)
	}
	if len(raw) > maxResponseBytes {
		t.Fatalf("response length %d exceeds %d", len(raw), maxResponseBytes)
	}
	var result rpcResponse
	if err := json.Unmarshal(raw, &result); err != nil {
		t.Fatalf("invalid JSON response: %v: %s", err, raw)
	}
	if result.OK == (result.Error != nil) {
		t.Fatalf("invalid response envelope: %s", raw)
	}
	return resp.StatusCode, result, raw
}

func TestHealthEchoAuthorization(t *testing.T) {
	const secret = "fixture-only-token-not-a-real-credential-012345"
	host := httptest.NewServer(newRPCServer(secret, false, nil))
	defer host.Close()
	body := `{"operation":"health","request_id":"health-1","args":{}}`
	for _, credential := range []string{"", "incorrect"} {
		status, result, raw := rpcCall(t, host.Client(), host.URL, body, credential)
		if status != http.StatusUnauthorized || result.Error.Code != "unauthorized" || bytes.Contains(raw, []byte(secret)) {
			t.Fatalf("unauthorized response: %d %s", status, raw)
		}
	}
	status, result, raw := rpcCall(t, host.Client(), host.URL, body, secret)
	if status != http.StatusOK || !result.OK || string(raw) != `{"ok":true,"result":{"status":"ok"}}` {
		t.Fatalf("health response: %d %s", status, raw)
	}
	status, _, raw = rpcCall(t, host.Client(), host.URL,
		`{"operation":"echo","request_id":"echo.1","args":{"text":"hello, 世界"}}`, secret)
	if status != http.StatusOK || string(raw) != `{"ok":true,"result":{"text":"hello, 世界"}}` {
		t.Fatalf("echo response: %d %s", status, raw)
	}
	status, _, raw = rpcCall(t, host.Client(), host.URL,
		`{"OPERATION":"echo","REQUEST_ID":"case-1","ARGS":{"TEXT":"case-insensitive"}}`, secret)
	if status != http.StatusOK || string(raw) != `{"ok":true,"result":{"text":"case-insensitive"}}` {
		t.Fatalf("documented JSON field matching: %d %s", status, raw)
	}
	status, result, _ = rpcCall(t, host.Client(), host.URL,
		`{"operation":"weather","request_id":"w","args":{"place":"Fixture City"}}`, secret)
	if status != http.StatusServiceUnavailable || result.Error.Code != "weather_disabled" {
		t.Fatalf("disabled weather: %d %+v", status, result)
	}
}

func TestNamedNetworkExamples(t *testing.T) {
	const secret = "fixture-network-only-token-0123456789abcdef"
	host := httptest.NewServer(newRPCServer(secret, false, nil))
	defer host.Close()
	call := func(method, path, body, credential string) (int, string) {
		t.Helper()
		req, err := http.NewRequest(method, host.URL+path, strings.NewReader(body))
		if err != nil {
			t.Fatal(err)
		}
		if body != "" {
			req.Header.Set("Content-Type", "application/json")
		}
		if credential != "" {
			req.Header.Set("Authorization", "Bearer "+credential)
		}
		response, err := host.Client().Do(req)
		if err != nil {
			t.Fatal(err)
		}
		defer response.Body.Close()
		data, err := io.ReadAll(response.Body)
		if err != nil {
			t.Fatal(err)
		}
		return response.StatusCode, string(data)
	}
	for _, path := range []string{"/v1/example/status", "/v1/example/echo"} {
		method := http.MethodGet
		if strings.HasSuffix(path, "echo") {
			method = http.MethodPost
		}
		status, body := call(method, path, "", "")
		if status != http.StatusUnauthorized || strings.Contains(body, secret) {
			t.Fatalf("example endpoint authentication: %d %s", status, body)
		}
	}
	if status, body := call(http.MethodGet, "/v1/example/status", "", secret); status != http.StatusOK || body != `{"service":"meshcore-bot-service","status":"ok"}` {
		t.Fatalf("GET example: %d %s", status, body)
	}
	if status, body := call(http.MethodPost, "/v1/example/echo", `{"text":"node"}`, secret); status != http.StatusOK || body != `{"text":"node"}` {
		t.Fatalf("POST example: %d %s", status, body)
	}
	if status, body := call(http.MethodPost, "/v1/example/echo",
		`{"text":"`+strings.Repeat("<", maxEchoBytes)+`"}`, secret); status != http.StatusOK || body != `{"text":"`+strings.Repeat("<", maxEchoBytes)+`"}` {
		t.Fatalf("maximum escaped POST example: %d %s", status, body)
	}
	for _, tc := range []struct{ method, path, body string }{
		{http.MethodGet, "/v1/example/echo", ""},
		{http.MethodPost, "/v1/example/status", `{}`},
		{http.MethodGet, "/v1/example/status", `{}`},
		{http.MethodPost, "/v1/example/echo", `{"text":1}`},
		{http.MethodPost, "/v1/example/echo", `{"text":"node","secret":"x"}`},
	} {
		status, _ := call(tc.method, tc.path, tc.body, secret)
		if status < 400 {
			t.Fatalf("accepted malformed example %s %s: %d", tc.method, tc.path, status)
		}
	}
	status, response, _ := rpcCall(t, host.Client(), host.URL,
		`{"operation":"sum","request_id":"sum-1","args":{"a":13,"b":29}}`, secret)
	if status != http.StatusOK || !response.OK {
		t.Fatalf("sum RPC: %d %+v", status, response)
	}
	for _, body := range []string{
		`{"operation":"sum","request_id":"sum-2","args":{"a":1}}`,
		`{"operation":"sum","request_id":"sum-3","args":{"a":1.5,"b":2}}`,
		`{"operation":"sum","request_id":"sum-4","args":{"a":1000001,"b":2}}`,
	} {
		status, response, _ := rpcCall(t, host.Client(), host.URL, body, secret)
		if status != http.StatusBadRequest || response.Error.Code != "bad_request" {
			t.Fatalf("invalid sum RPC: %d %+v", status, response)
		}
	}
}

func TestMalformedAndBoundedRequests(t *testing.T) {
	host := httptest.NewServer(newRPCServer("", false, nil))
	defer host.Close()
	cases := []struct {
		body string
		code string
	}{
		{`{`, "bad_request"},
		{`{"operation":"health","request_id":"1","args":{}} {}`, "bad_request"},
		{`{"operation":"health","request_id":"1","args":null}`, "bad_request"},
		{`{"operation":"health","request_id":"1","args":{"x":1}}`, "bad_request"},
		{`{"operation":"health","request_id":"1","args":{},"extra":1}`, "bad_request"},
		{`{"request_id":"1","args":{}}`, "bad_request"},
		{`{"operation":"health","request_id":"","args":{}}`, "bad_request"},
		{`{"operation":"health","request_id":"a b","args":{}}`, "bad_request"},
		{`{"operation":"health","request_id":"` + strings.Repeat("a", 65) + `","args":{}}`, "bad_request"},
		{`{"operation":"missing","request_id":"1","args":{}}`, "unsupported_operation"},
		{`{"operation":"echo","request_id":"1","args":{}}`, "bad_request"},
		{`{"operation":"echo","request_id":"1","args":{"text":"one\nline"}}`, "bad_request"},
		{`{"operation":"echo","request_id":"1","args":{"text":"` + strings.Repeat("a", 513) + `"}}`, "bad_request"},
		{`{"operation":"weather","request_id":"1","args":{"place":" "}}`, "bad_request"},
		{`{"operation":"weather","request_id":"1","args":{"place":"` + strings.Repeat("a", 81) + `"}}`, "bad_request"},
		{`{"operation":"health","request_id":"1","args":{}}` + strings.Repeat(" ", maxRequestBytes+1), "request_too_large"},
	}
	for _, tc := range cases {
		status, result, _ := rpcCall(t, host.Client(), host.URL, tc.body, "")
		if result.Error == nil || result.Error.Code != tc.code || status < 400 {
			t.Errorf("body %.80q: status %d, result %+v; want %s", tc.body, status, result, tc.code)
		}
	}
	limitBody := `{"operation":"health","request_id":"1","args":{}}`
	status, result, _ := rpcCall(t, host.Client(), host.URL,
		limitBody+strings.Repeat(" ", maxRequestBytes-len(limitBody)), "")
	if status != http.StatusOK || !result.OK {
		t.Fatalf("exact request limit: %d %+v", status, result)
	}
	status, result, raw := rpcCall(t, host.Client(), host.URL,
		`{"operation":"echo","request_id":"`+strings.Repeat("a", maxRequestID)+`","args":{"text":"`+
			strings.Repeat("x", maxEchoBytes)+`"}}`, "")
	if status != http.StatusOK || !result.OK || len(raw) > maxResponseBytes {
		t.Fatalf("exact string limits: %d %s", status, raw)
	}
	req, _ := http.NewRequest(http.MethodPost, host.URL+"/v1/rpc", strings.NewReader(limitBody+strings.Repeat(" ", maxRequestBytes)))
	req.ContentLength = -1
	req.Header.Set("Content-Type", "application/json")
	resp, err := host.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var chunked rpcResponse
	if err := json.NewDecoder(resp.Body).Decode(&chunked); err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusRequestEntityTooLarge || chunked.Error.Code != "request_too_large" {
		t.Fatalf("chunked oversized request: %d %+v", resp.StatusCode, chunked)
	}
}

func TestMethodContentTypeAndUTF8(t *testing.T) {
	handler := newRPCServer("", false, nil)
	for _, tc := range []struct {
		method, contentType string
		body                []byte
		status              int
		code                string
	}{
		{http.MethodGet, "", nil, 405, "method_not_allowed"},
		{http.MethodPost, "text/plain", []byte("{}"), 415, "unsupported_media_type"},
		{http.MethodPost, "application/json", []byte{0xff}, 400, "bad_request"},
	} {
		req := httptest.NewRequest(tc.method, "/v1/rpc", bytes.NewReader(tc.body))
		req.Header.Set("Content-Type", tc.contentType)
		rec := httptest.NewRecorder()
		handler.ServeHTTP(rec, req)
		var result rpcResponse
		if err := json.Unmarshal(rec.Body.Bytes(), &result); err != nil {
			t.Fatal(err)
		}
		if rec.Code != tc.status || result.Error.Code != tc.code {
			t.Errorf("%s %s: %d %s", tc.method, tc.contentType, rec.Code, rec.Body.String())
		}
	}
}

func TestNonLoopbackRequiresTLSAndToken(t *testing.T) {
	t.Setenv(tokenEnv, "")
	if err := run([]string{"-listen", "0.0.0.0:0"}); err == nil || !strings.Contains(err.Error(), "requires a token and TLS") {
		t.Fatalf("missing protection: %v", err)
	}
	t.Setenv(tokenEnv, strings.Repeat("x", 32))
	if err := run([]string{"-listen", "[::]:0"}); err == nil || !strings.Contains(err.Error(), "requires a token and TLS") {
		t.Fatalf("missing TLS: %v", err)
	}
	for _, args := range [][]string{
		{"-listen", "example.com:0"},
		{"-tls-cert", "cert.pem"},
		{"-tls-key", "key.pem"},
	} {
		if err := run(args); err == nil {
			t.Errorf("accepted invalid configuration %q", args)
		}
	}
	t.Setenv(tokenEnv, "short")
	if err := run(nil); err == nil || !strings.Contains(err.Error(), tokenEnv) || strings.Contains(err.Error(), "short") {
		t.Fatalf("invalid token error leaks credential or missing error: %v", err)
	}
	if err := validateToken(strings.Repeat("x", 256)); err != nil {
		t.Fatal(err)
	}
}

func TestBusyDoesNotAdmitMoreRequests(t *testing.T) {
	s := newRPCServer("", false, nil)
	for i := 0; i < maxConcurrent; i++ {
		s.slots <- struct{}{}
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/rpc", strings.NewReader(`{"operation":"health","request_id":"1","args":{}}`))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	s.ServeHTTP(rec, req)
	if rec.Code != http.StatusServiceUnavailable || !strings.Contains(rec.Body.String(), `"code":"busy"`) {
		t.Fatalf("unbounded admission: %d %s", rec.Code, rec.Body.String())
	}
}

func TestOversizeResponseIsAnError(t *testing.T) {
	rec := httptest.NewRecorder()
	writeResult(rec, map[string]any{"text": strings.Repeat("x", maxResponseBytes)})
	var result rpcResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if rec.Code != http.StatusInternalServerError || result.Error == nil ||
		result.Error.Code != "internal_error" || len(rec.Body.Bytes()) > maxResponseBytes {
		t.Fatalf("response exceeded limit or failed open: %d %s", rec.Code, rec.Body.String())
	}
}

func TestAllValidEchoStringsFitResponseLimit(t *testing.T) {
	host := httptest.NewServer(newRPCServer("", false, nil))
	defer host.Close()
	for _, text := range []string{
		strings.Repeat("<", maxEchoBytes),
		strings.Repeat("&", maxEchoBytes),
		strings.Repeat(">", maxEchoBytes),
		strings.Repeat(`\`, maxEchoBytes),
		strings.Repeat(`"`, maxEchoBytes),
		strings.Repeat("\u2028", maxEchoBytes/3),
	} {
		input, err := json.Marshal(map[string]any{
			"operation": "echo", "request_id": "limit", "args": map[string]string{"text": text},
		})
		if err != nil {
			t.Fatal(err)
		}
		status, result, raw := rpcCall(t, host.Client(), host.URL, string(input), "")
		payload, ok := result.Result.(map[string]any)
		if status != http.StatusOK || !ok || payload["text"] != text {
			t.Fatalf("valid echo (%q) returned %d %s", text[:min(8, len(text))], status, raw)
		}
		if text[0] == '<' && !bytes.Contains(raw, []byte(strings.Repeat("<", maxEchoBytes))) {
			t.Fatal("HTML escaping unexpectedly expanded the echo response")
		}
	}
}

type waitingBody struct {
	started  chan<- struct{}
	released <-chan struct{}
}

func (b *waitingBody) Read([]byte) (int, error) {
	b.started <- struct{}{}
	<-b.released
	return 0, io.EOF
}

func (*waitingBody) Close() error { return nil }

func TestSlowReadsDoNotConsumeProcessingSlots(t *testing.T) {
	handler := newRPCServer("", false, nil)
	started := make(chan struct{}, maxConcurrent)
	released := make(chan struct{})
	var calls sync.WaitGroup
	for range maxConcurrent {
		calls.Add(1)
		go func() {
			defer calls.Done()
			req := httptest.NewRequest(http.MethodPost, "/v1/rpc", &waitingBody{started: started, released: released})
			req.Header.Set("Content-Type", "application/json")
			handler.ServeHTTP(httptest.NewRecorder(), req)
		}()
	}
	for range maxConcurrent {
		select {
		case <-started:
		case <-time.After(2 * time.Second):
			close(released)
			calls.Wait()
			t.Fatal("slow request did not reach body read")
		}
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/rpc",
		strings.NewReader(`{"operation":"health","request_id":"fast","args":{}}`))
	req.Header.Set("Content-Type", "application/json")
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, req)
	close(released)
	calls.Wait()
	if rec.Code != http.StatusOK || rec.Body.String() != `{"ok":true,"result":{"status":"ok"}}` {
		t.Fatalf("slow readers exhausted workers: %d %s", rec.Code, rec.Body.String())
	}
}

func TestNoConfiguredCredentialsInErrors(t *testing.T) {
	const secret = "never-print-this-test-only-credential-0000"
	host := httptest.NewServer(newRPCServer(secret, false, nil))
	defer host.Close()
	_, _, raw := rpcCall(t, host.Client(), host.URL, `{"secret":"`+secret+`"}`, secret)
	if bytes.Contains(raw, []byte(secret)) {
		t.Fatalf("credential leaked in error: %s", raw)
	}
}
