package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"io"
	"mime"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"
)

const (
	maxRequestBytes  = 4096
	maxResponseBytes = 2048
	maxEchoBytes     = 512
	maxPlaceBytes    = 80
	maxRequestID     = 64
	maxConcurrent    = 16
	requestTimeout   = 5 * time.Second
)

type rpcRequest struct {
	Operation string          `json:"operation"`
	RequestID string          `json:"request_id"`
	Args      json.RawMessage `json:"args"`
}

type rpcError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

type rpcResponse struct {
	OK     bool      `json:"ok"`
	Result any       `json:"result,omitempty"`
	Error  *rpcError `json:"error,omitempty"`
}

type rpcServer struct {
	token   [32]byte
	hasAuth bool
	weather bool
	source  *weatherProvider
	slots   chan struct{}
	compute *computeService
}

func newRPCServer(token string, weather bool, source *weatherProvider) *rpcServer {
	return &rpcServer{
		token:   sha256.Sum256([]byte("Bearer " + token)),
		hasAuth: token != "",
		weather: weather,
		source:  source,
		slots:   make(chan struct{}, maxConcurrent),
	}
}

func (s *rpcServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	method := http.MethodPost
	switch r.URL.Path {
	case "/v1/rpc", "/v1/example/echo":
	case "/v1/example/status":
		method = http.MethodGet
	default:
		writeError(w, http.StatusNotFound, "not_found", "endpoint not found")
		return
	}
	if r.Method != method {
		w.Header().Set("Allow", method)
		writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", method+" required")
		return
	}
	if s.hasAuth {
		authorization := r.Header.Get("Authorization")
		hash := sha256.Sum256([]byte(authorization))
		if len(r.Header.Values("Authorization")) != 1 || subtle.ConstantTimeCompare(hash[:], s.token[:]) != 1 {
			w.Header().Set("WWW-Authenticate", "Bearer")
			writeError(w, http.StatusUnauthorized, "unauthorized", "invalid or missing bearer token")
			return
		}
	}
	ctx, cancel := context.WithTimeout(r.Context(), requestTimeout)
	defer cancel()
	r = r.WithContext(ctx)
	if method == http.MethodGet {
		if r.ContentLength != 0 {
			writeError(w, http.StatusBadRequest, "bad_request", "GET requires an empty body")
			return
		}
		select {
		case s.slots <- struct{}{}:
			defer func() { <-s.slots }()
		default:
			writeError(w, http.StatusServiceUnavailable, "busy", "service is busy")
			return
		}
		writePlainResult(w, map[string]string{"status": "ok", "service": "meshcore-bot-service"})
		return
	}
	mediaType, params, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" || (params["charset"] != "" && !strings.EqualFold(params["charset"], "utf-8")) {
		writeError(w, http.StatusUnsupportedMediaType, "unsupported_media_type", "application/json required")
		return
	}
	if r.ContentLength > maxRequestBytes {
		writeError(w, http.StatusRequestEntityTooLarge, "request_too_large", "request body exceeds 4096 bytes")
		return
	}
	body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxRequestBytes))
	if err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			writeError(w, http.StatusRequestEntityTooLarge, "request_too_large", "request body exceeds 4096 bytes")
		} else {
			writeError(w, http.StatusBadRequest, "bad_request", "could not read request body")
		}
		return
	}
	if !utf8.Valid(body) {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid UTF-8 JSON")
		return
	}
	select {
	case s.slots <- struct{}{}:
		defer func() { <-s.slots }()
	default:
		writeError(w, http.StatusServiceUnavailable, "busy", "service is busy")
		return
	}
	if r.URL.Path == "/v1/example/echo" {
		var args struct {
			Text string `json:"text"`
		}
		if decodeStrict(body, &args) != nil || !validText(args.Text, maxEchoBytes) {
			writeError(w, http.StatusBadRequest, "bad_request", "invalid example echo body")
			return
		}
		writePlainResult(w, map[string]string{"text": args.Text})
		return
	}
	var req rpcRequest
	if err := decodeStrict(body, &req); err != nil || req.Operation == "" || !validRequestID(req.RequestID) || len(req.Args) == 0 || bytes.TrimSpace(req.Args)[0] != '{' {
		writeError(w, http.StatusBadRequest, "bad_request", "invalid RPC request")
		return
	}
	switch req.Operation {
	case "digest":
		if s.compute == nil {
			writeError(w, http.StatusServiceUnavailable, "compute_disabled", "digest is not enabled")
			return
		}
		status, response := s.compute.digest(ctx, req)
		writeJSON(w, status, response)
	case "health":
		var args struct{}
		if decodeStrict(req.Args, &args) != nil {
			writeError(w, http.StatusBadRequest, "bad_request", "invalid health arguments")
			return
		}
		writeResult(w, map[string]any{"status": "ok"})
	case "echo":
		var args struct {
			Text string `json:"text"`
		}
		if decodeStrict(req.Args, &args) != nil || !validText(args.Text, maxEchoBytes) {
			writeError(w, http.StatusBadRequest, "bad_request", "invalid echo arguments")
			return
		}
		writeResult(w, map[string]any{"text": args.Text})
	case "sum":
		var args struct {
			A *int64 `json:"a"`
			B *int64 `json:"b"`
		}
		if decodeStrict(req.Args, &args) != nil || args.A == nil || args.B == nil ||
			*args.A < -1000000 || *args.A > 1000000 ||
			*args.B < -1000000 || *args.B > 1000000 {
			writeError(w, http.StatusBadRequest, "bad_request", "sum requires integer a and b from -1000000 to 1000000")
			return
		}
		writeResult(w, map[string]any{"sum": *args.A + *args.B})
	case "weather":
		var args struct {
			Place string `json:"place"`
		}
		if decodeStrict(req.Args, &args) != nil || !validText(args.Place, maxPlaceBytes) || strings.TrimSpace(args.Place) != args.Place {
			writeError(w, http.StatusBadRequest, "bad_request", "invalid weather arguments")
			return
		}
		if !s.weather {
			writeError(w, http.StatusServiceUnavailable, "weather_disabled", "weather is not enabled")
			return
		}
		result, status, failure := s.source.lookup(ctx, args.Place)
		if failure != nil {
			writeError(w, status, failure.Code, failure.Message)
			return
		}
		writeResult(w, result)
	default:
		writeError(w, http.StatusBadRequest, "unsupported_operation", "operation is not supported")
	}
}

func decodeStrict(data []byte, value any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(value); err != nil {
		return err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return errors.New("trailing JSON")
	}
	return nil
}

func validRequestID(id string) bool {
	if len(id) < 1 || len(id) > maxRequestID {
		return false
	}
	for _, c := range id {
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			return false
		}
	}
	return true
}

func validText(text string, limit int) bool {
	if len(text) < 1 || len(text) > limit || !utf8.ValidString(text) {
		return false
	}
	for _, c := range text {
		if unicode.IsControl(c) {
			return false
		}
	}
	return true
}

func writeResult(w http.ResponseWriter, result any) {
	writeJSON(w, http.StatusOK, rpcResponse{OK: true, Result: result})
}

func writePlainResult(w http.ResponseWriter, result any) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	err := encoder.Encode(result)
	data := bytes.TrimSuffix(buffer.Bytes(), []byte("\n"))
	if err != nil || len(data) > maxResponseBytes {
		writeError(w, http.StatusInternalServerError, "internal_error", "response could not be encoded")
		return
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, rpcResponse{OK: false, Error: &rpcError{Code: code, Message: message}})
}

func writeJSON(w http.ResponseWriter, status int, response rpcResponse) {
	var buffer bytes.Buffer
	encoder := json.NewEncoder(&buffer)
	encoder.SetEscapeHTML(false)
	err := encoder.Encode(response)
	data := bytes.TrimSuffix(buffer.Bytes(), []byte("\n"))
	if err != nil || len(data) > maxResponseBytes {
		status = http.StatusInternalServerError
		data = []byte(`{"ok":false,"error":{"code":"internal_error","message":"response could not be encoded"}}`)
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.WriteHeader(status)
	_, _ = w.Write(data)
}
