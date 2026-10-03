package companion

import (
	"bytes"
	"context"
	"crypto/sha512"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	hoststate "meshcore.local/meshcore/internal/state"
)

func TestIdentityExportUsesParentNativeRepresentation(t *testing.T) {
	root := stateDir(t)
	dir := filepath.Join(root, "companion")
	if err := os.Mkdir(dir, 0700); err != nil {
		t.Fatal(err)
	}
	id := testIdentity(1)
	seed := id.Seed()
	if err := os.WriteFile(filepath.Join(dir, "identity.seed"), seed[:], 0600); err != nil {
		t.Fatal(err)
	}
	cfg := testConfig(dir)
	cfg.ExportIdentity = func(context.Context) ([]byte, error) { return hoststate.ExportIdentity(root, "companion") }
	s, _, _ := startTestServer(t, id, cfg)
	frame := runCommand(t, s, []byte{protocol.CmdExportPrivateKey})
	expected := sha512.Sum512(seed[:])
	expected[0] &= 248
	expected[31] &= 63
	expected[31] |= 64
	if !bytes.Equal(frame, append([]byte{protocol.RespPrivateKey}, expected[:]...)) {
		t.Fatal("export did not preserve native scalar-prefix representation")
	}
	if _, err := protocol.ParseResponse(frame); err != nil {
		t.Fatal(err)
	}
}

func TestIdentityExportPolicyAndFailures(t *testing.T) {
	other := testIdentity(2).Seed()
	expanded := sha512.Sum512(other[:])
	expanded[0] &= 248
	expanded[31] &= 63
	expanded[31] |= 64
	for _, tc := range []struct {
		name   string
		export func(context.Context) ([]byte, error)
		want   []byte
	}{
		{"disabled", nil, []byte{protocol.RespDisabled}},
		{"provider failure", func(context.Context) ([]byte, error) { return nil, errors.New("identity read failed") }, []byte{protocol.RespErr, protocol.ErrCodeBadState}},
		{"wrong size", func(context.Context) ([]byte, error) { return make([]byte, 32), nil }, []byte{protocol.RespErr, protocol.ErrCodeBadState}},
		{"other role", func(context.Context) ([]byte, error) { return expanded[:], nil }, []byte{protocol.RespErr, protocol.ErrCodeBadState}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			cfg := testConfig("")
			cfg.ExportIdentity = tc.export
			s, _, _ := startTestServer(t, testIdentity(1), cfg)
			if got := runCommand(t, s, []byte{protocol.CmdExportPrivateKey}); !bytes.Equal(got, tc.want) {
				t.Fatalf("export response %x, want %x", got, tc.want)
			}
		})
	}
}

func expandedTestKey(id meshcore.LocalIdentity) [64]byte {
	seed := id.Seed()
	key := sha512.Sum512(seed[:])
	key[0] &= 248
	key[31] &= 63
	key[31] |= 64
	return key
}

func TestIdentityImportActivatesAfterCommitAndSurvivesRestart(t *testing.T) {
	root := stateDir(t)
	dir := filepath.Join(root, "companion")
	path := filepath.Join(dir, "companion.json")
	old, err := hoststate.Identity(root, "companion")
	if err != nil {
		t.Fatal(err)
	}
	sibling, err := hoststate.Identity(root, "room")
	if err != nil {
		t.Fatal(err)
	}
	next, peer := testIdentity(2), testIdentity(3)
	key := expandedTestKey(next)
	cfg := testConfig(dir)
	var s *Server
	cfg.ImportIdentity = func(ctx context.Context, key []byte, data json.RawMessage) (meshcore.LocalIdentity, error) {
		if err := ctx.Err(); err != nil {
			return meshcore.LocalIdentity{}, err
		}
		imported, err := hoststate.ImportRoleIdentity(path, key, data)
		if err == nil && s.Node.Identity().PublicKey() != old.PublicKey() {
			return meshcore.LocalIdentity{}, errors.New("runtime identity changed before storage callback completed")
		}
		return imported, err
	}
	cfg.ExportIdentity = func(context.Context) ([]byte, error) { return hoststate.ExportIdentity(root, "companion") }
	var r *testRadio
	var addr string
	s, r, addr = startTestServer(t, old, cfg)
	a, b := testClient(t, addr), testClient(t, addr)
	ctx := testContext(t)
	if err := a.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
		PublicKey: peer.PublicKey(), Type: meshcore.AdvertTypeChat, OutPathLen: 0, Name: "Peer",
	}); err != nil {
		t.Fatal(err)
	}
	channel, err := meshcore.NewChannelFromBase64("Retained", "izOH6cXN6mrJ5e26oRXNcg==")
	if err != nil {
		t.Fatal(err)
	}
	if err := b.SetChannel(ctx, 2, channel.Name, channel.PSK); err != nil {
		t.Fatal(err)
	}
	waiting := capturePush(a, protocol.PushMsgWaiting)
	plain := meshcore.BuildTextPlaintextWithAttempt(time.Unix(123, 0), 0, []byte("retained message"), 0)
	r.inject(datagram(t, peer, old, meshcore.PayloadTypeTxtMsg, plain))
	nextPush(t, waiting)
	r.packet(t)
	s.mu.Lock()
	s.state.Contacts[0].SyncSince = 123
	before := s.state
	before.PublicKey = next.PublicKey()
	wantState, err := json.Marshal(before)
	s.mu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	legacy, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	if err := a.ImportPrivateKey(ctx, key); err != nil {
		t.Fatal(err)
	}
	for _, c := range []*client.Client{a, b} {
		info, err := c.AppStart(ctx, 3, "after import")
		if err != nil || info.PublicKey != next.PublicKey() {
			t.Fatalf("client observed wrong post-import identity: %+v %v", info, err)
		}
	}
	s.mu.Lock()
	gotState, err := json.Marshal(s.state)
	s.mu.Unlock()
	if err != nil || !bytes.Equal(gotState, wantState) {
		t.Fatalf("import changed state beyond PublicKey: %v", err)
	}
	if got := runCommand(t, s, []byte{protocol.CmdExportPrivateKey}); !bytes.Equal(got, append([]byte{protocol.RespPrivateKey}, key[:]...)) {
		t.Fatal("export did not select the imported authority")
	}
	if _, err := b.SendTextMessage(ctx, peer.Identity, "new key", protocol.TxtTypePlain); err != nil {
		t.Fatal(err)
	}
	pkt := r.packet(t)
	msg, err := meshcore.TextMessageFromBytes(pkt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	secret, err := peer.SharedSecret(next.Identity)
	if err != nil {
		t.Fatal(err)
	}
	decrypted := msg.Decrypt(secret)
	if len(decrypted) < 5 || string(bytes.TrimRight(decrypted[5:], "\x00")) != "new key" || msg.Source != next.PublicKey()[0] {
		t.Fatal("post-import text used the old identity or cached shared secret")
	}
	if err := b.SetAdvertName(ctx, "After import"); err != nil {
		t.Fatal(err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if data, err := os.ReadFile(path); err != nil || !bytes.Equal(data, legacy) {
		t.Fatal("post-import saves overwrote the retained legacy document")
	}
	imported, err := hoststate.Identity(root, "companion")
	if err != nil || imported.PublicKey() != next.PublicKey() {
		t.Fatalf("restart selected the wrong identity: %v", err)
	}
	other, err := hoststate.Identity(root, "room")
	if err != nil || other.PublicKey() != sibling.PublicKey() {
		t.Fatal("companion import changed the room identity")
	}
	reloaded, _, _ := startTestServer(t, imported, cfg)
	reloaded.mu.Lock()
	defer reloaded.mu.Unlock()
	if reloaded.state.Name != "After import" || len(reloaded.state.Messages) != 1 ||
		len(reloaded.state.Contacts) != 1 || reloaded.state.Contacts[0].SyncSince != 123 ||
		reloaded.state.Channels[2].Name != "Retained" {
		t.Fatal("restart lost authoritative role state")
	}
}

func TestCommittedIdentityImportSurvivesDisconnectAndShutdown(t *testing.T) {
	for _, shutdown := range []bool{false, true} {
		t.Run(fmt.Sprintf("shutdown=%t", shutdown), func(t *testing.T) {
			root := stateDir(t)
			old, err := hoststate.Identity(root, "companion")
			if err != nil {
				t.Fatal(err)
			}
			cfg := testConfig(filepath.Join(root, "companion"))
			path := filepath.Join(cfg.StateDir, "companion.json")
			committed := make(chan struct{})
			release := make(chan struct{})
			callbackContext := make(chan error, 1)
			cfg.ImportIdentity = func(ctx context.Context, key []byte, data json.RawMessage) (meshcore.LocalIdentity, error) {
				id, err := hoststate.ImportRoleIdentity(path, key, data)
				if err != nil {
					return meshcore.LocalIdentity{}, err
				}
				close(committed)
				select {
				case <-release:
				case <-ctx.Done():
				}
				callbackContext <- ctx.Err()
				return id, nil
			}
			s, _, addr := startTestServer(t, old, cfg)
			conn, err := net.Dial("tcp", addr)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			next := testIdentity(2)
			key := expandedTestKey(next)
			frame, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, append([]byte{protocol.CmdImportPrivateKey}, key[:]...))
			if err != nil {
				t.Fatal(err)
			}
			if _, err := conn.Write(frame); err != nil {
				t.Fatal(err)
			}
			select {
			case <-committed:
			case <-time.After(3 * time.Second):
				t.Fatal("import did not reach durable commit")
			}
			if shutdown {
				if err := s.Close(); err != nil {
					t.Fatal(err)
				}
			} else {
				if err := conn.Close(); err != nil {
					t.Fatal(err)
				}
				close(release)
				if reply := runCommand(t, s, []byte{protocol.CmdGetDeviceTime}); reply[0] != protocol.RespCurrTime {
					t.Fatal("requester disconnect disabled the committed runtime")
				}
			}
			err = <-callbackContext
			if shutdown && !errors.Is(err, context.Canceled) {
				t.Fatalf("shutdown did not cancel the committed callback context: %v", err)
			}
			if !shutdown && err != nil {
				t.Fatalf("disconnected-client import relied on callback timeout: %v", err)
			}
			if s.Node.Identity().PublicKey() != next.PublicKey() {
				t.Fatal("successful commit was not activated")
			}
			if err := s.Close(); err != nil {
				t.Fatal(err)
			}
			recovered, err := hoststate.Identity(root, "companion")
			if err != nil || recovered.PublicKey() != next.PublicKey() {
				t.Fatalf("shutdown lost the imported key: %v", err)
			}
			reloaded, err := New(recovered, newTestRadio(), cfg)
			if err != nil {
				t.Fatalf("shutdown overwrote the committed identity/state pair: %v", err)
			}
			if err := reloaded.Close(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestIdentityImportFailureAndIndeterminateRecovery(t *testing.T) {
	for _, uncertain := range []bool{false, true} {
		t.Run(fmt.Sprintf("indeterminate=%t", uncertain), func(t *testing.T) {
			root := stateDir(t)
			old, err := hoststate.Identity(root, "companion")
			if err != nil {
				t.Fatal(err)
			}
			cfg := testConfig(filepath.Join(root, "companion"))
			path := filepath.Join(cfg.StateDir, "companion.json")
			key := expandedTestKey(testIdentity(2))
			cfg.ImportIdentity = func(_ context.Context, key []byte, data json.RawMessage) (meshcore.LocalIdentity, error) {
				if uncertain {
					if _, err := hoststate.ImportRoleIdentity(path, key, data); err != nil {
						return meshcore.LocalIdentity{}, err
					}
					return meshcore.LocalIdentity{}, fmt.Errorf("injected post-commit uncertainty: %w", hoststate.ErrCommitIndeterminate)
				}
				return meshcore.LocalIdentity{}, errors.New("injected aborted commit")
			}
			s, err := New(old, newTestRadio(), cfg)
			if err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = s.Close() })
			got := runCommand(t, s, append([]byte{protocol.CmdImportPrivateKey}, key[:]...))
			if !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeFileIoError}) {
				t.Fatalf("failed import response %x", got)
			}
			if s.Node.Identity().PublicKey() != old.PublicKey() || s.faulted.Load() != uncertain {
				t.Fatal("failed import activated an identity or used the wrong failure state")
			}
			if uncertain {
				if frame := runCommand(t, s, []byte{protocol.CmdGetDeviceTime}); !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeFileIoError}) {
					t.Fatal("indeterminate import allowed further commands")
				}
			} else if frame := runCommand(t, s, []byte{protocol.CmdGetDeviceTime}); frame[0] != protocol.RespCurrTime {
				t.Fatal("definitely aborted import disabled the old runtime")
			}
			err = s.Close()
			if uncertain != errors.Is(err, hoststate.ErrCommitIndeterminate) {
				t.Fatalf("unexpected close outcome: %v", err)
			}
			recovered, err := hoststate.Identity(root, "companion")
			if err != nil {
				t.Fatal(err)
			}
			want := old.PublicKey()
			if uncertain {
				want = testIdentity(2).PublicKey()
			}
			if recovered.PublicKey() != want {
				t.Fatal("recovery selected the wrong durable identity")
			}
			cfg.ImportIdentity = nil
			reloaded, err := New(recovered, newTestRadio(), cfg)
			if err != nil {
				t.Fatal(err)
			}
			if err := reloaded.Close(); err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestIdentityImportRejectsInvalidInputBeforeCommit(t *testing.T) {
	key := expandedTestKey(testIdentity(2))
	unclamped := key
	unclamped[0] |= 1
	var reserved [64]byte
	found := false
	for i := uint32(0); i < 4096; i++ {
		var seed [32]byte
		put32(seed[:], i)
		id := meshcore.NewLocalIdentityFromSeed(seed)
		if id.PublicKey()[0] == 0 || id.PublicKey()[0] == 255 {
			reserved, found = expandedTestKey(id), true
			break
		}
	}
	if !found {
		t.Fatal("no reserved-prefix test identity found")
	}
	cfg := testConfig(stateDir(t))
	cfg.ImportIdentity = func(context.Context, []byte, json.RawMessage) (meshcore.LocalIdentity, error) {
		t.Error("invalid network key reached persistence")
		return meshcore.LocalIdentity{}, errors.New("unexpected commit")
	}
	nonpersistent := cfg
	nonpersistent.StateDir = ""
	if s, err := New(testIdentity(1), newTestRadio(), nonpersistent); err == nil {
		_ = s.Close()
		t.Fatal("identity import enabled without persistent role state")
	}
	s, _, _ := startTestServer(t, testIdentity(1), cfg)
	for _, raw := range [][]byte{nil, key[:63], make([]byte, 64), unclamped[:], reserved[:]} {
		got := runCommand(t, s, append([]byte{protocol.CmdImportPrivateKey}, raw...))
		if !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}) {
			t.Fatalf("invalid key response %x", got)
		}
	}
	if s.Node.Identity().PublicKey() != testIdentity(1).PublicKey() {
		t.Fatal("invalid import changed the identity")
	}
}

func TestIdentityImportInvalidatesPendingWorkWithoutReplyBroadcast(t *testing.T) {
	cfg := testConfig(stateDir(t))
	cfg.ImportIdentity = func(_ context.Context, key []byte, data json.RawMessage) (meshcore.LocalIdentity, error) {
		return hoststate.ImportRoleIdentity(filepath.Join(cfg.StateDir, "companion.json"), key, data)
	}
	old, next := testIdentity(1), testIdentity(2)
	s, _, _ := startTestServer(t, old, cfg)
	a, b := commandSession(t, s), commandSession(t, s)
	for _, c := range []*session{a, b} {
		sessionCommand(t, c, []byte{protocol.CmdSignStart})
		requireOK(t, sessionCommand(t, c, append([]byte{protocol.CmdSignData}, "old session"...)))
	}
	s.mu.Lock()
	s.clients[a], s.clients[b] = struct{}{}, struct{}{}
	s.pending[1] = pending{Client: a, Expires: time.Now().Add(time.Hour)}
	s.loginFences[4] = loginFence{Key: old.PublicKey(), Expires: time.Now().Add(time.Hour)}
	s.acks[2] = time.Now()
	s.connections[old.PublicKey()] = &connection{NextPing: time.Now().Add(time.Hour)}
	s.traces[3] = traceRequest{}
	s.addTransient(&contact{ContactResponse: protocol.ContactResponse{PublicKey: old.PublicKey(), OutPathLen: 0}})
	s.mu.Unlock()
	key := expandedTestKey(next)
	command := append([]byte{protocol.CmdImportPrivateKey}, key[:]...)
	requireOK(t, sessionCommand(t, a, append(command, 0xaa, 0xbb)))
	select {
	case extra := <-b.out:
		t.Fatalf("import reply leaked to another client: %x", extra)
	default:
	}
	s.mu.Lock()
	if len(s.pending)+len(s.loginFences)+len(s.acks)+len(s.connections)+len(s.traces)+len(s.transient) != 0 {
		t.Error("identity-bound volatile state survived import")
	}
	if s.identity.Load().PublicKey() != next.PublicKey() || len(s.Node.Peers().Peers()) != 0 {
		t.Error("identity snapshot or transient peer cache retained the old identity")
	}
	s.mu.Unlock()
	for _, c := range []*session{a, b} {
		if got := sessionCommand(t, c, []byte{protocol.CmdSignFinish}); !bytes.Equal(got, []byte{protocol.RespErr, protocol.ErrCodeBadState}) {
			t.Fatalf("old signing session continued under the new identity: %x", got)
		}
	}
	sessionCommand(t, b, []byte{protocol.CmdSignStart})
	requireOK(t, sessionCommand(t, b, append([]byte{protocol.CmdSignData}, "fresh"...)))
	signature := sessionCommand(t, b, []byte{protocol.CmdSignFinish})
	if signature[0] != protocol.RespSignature || !next.Verify([]byte("fresh"), signature[1:]) || old.Verify([]byte("fresh"), signature[1:]) {
		t.Fatal("new signing session did not use the imported identity")
	}
}

func TestIdentityImportCancellationDoesNotDeadlockClose(t *testing.T) {
	entered := make(chan struct{})
	cancelled := make(chan error, 1)
	cfg := testConfig(stateDir(t))
	cfg.ImportIdentity = func(ctx context.Context, _ []byte, _ json.RawMessage) (meshcore.LocalIdentity, error) {
		close(entered)
		<-ctx.Done()
		cancelled <- ctx.Err()
		return meshcore.LocalIdentity{}, ctx.Err()
	}
	s, err := New(testIdentity(1), newTestRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	key := expandedTestKey(testIdentity(2))
	reply := make(chan []byte, 1)
	go func() { reply <- runCommand(t, s, append([]byte{protocol.CmdImportPrivateKey}, key[:]...)) }()
	select {
	case <-entered:
	case <-time.After(3 * time.Second):
		t.Fatal("import did not enter persistence callback")
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	if err := <-cancelled; !errors.Is(err, context.Canceled) {
		t.Fatalf("close waited for the deadline instead of cancelling import: %v", err)
	}
	select {
	case frame := <-reply:
		if !bytes.Equal(frame, []byte{protocol.RespErr, protocol.ErrCodeFileIoError}) {
			t.Fatalf("cancelled import response %x", frame)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("cancelled command did not finish")
	}
	if s.Node.Identity().PublicKey() != testIdentity(1).PublicKey() {
		t.Fatal("cancelled import changed the runtime identity")
	}
}

func TestRoleResetNullStateDoesNotRestoreLegacyDocument(t *testing.T) {
	root := stateDir(t)
	old, err := hoststate.Identity(root, "companion")
	if err != nil {
		t.Fatal(err)
	}
	cfg := testConfig(filepath.Join(root, "companion"))
	s, err := New(old, newTestRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	requireOK(t, runCommand(t, s, append([]byte{protocol.CmdSetAdvertName}, "Old name"...)))
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(cfg.StateDir, "companion.json")
	fresh, err := hoststate.ResetRole(path)
	if err != nil {
		t.Fatal(err)
	}
	reloaded, err := New(fresh, newTestRadio(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := reloaded.Close(); err != nil {
		t.Fatal(err)
	}
	data, err := hoststate.ReadRoleJSON(path)
	if err != nil {
		t.Fatal(err)
	}
	var saved state
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatal(err)
	}
	if saved.Name != cfg.Name || saved.PublicKey != fresh.PublicKey() || fresh.PublicKey() == old.PublicKey() {
		t.Fatal("reset restored the legacy identity or state instead of saving fresh defaults")
	}
}

func TestLifecycleCommandAdmission(t *testing.T) {
	rejected := errors.New("lifecycle worker is busy")
	for _, command := range []struct {
		name string
		wire []byte
	}{
		{"reboot", (protocol.RebootCommand{}).ToBytes()},
		{"reset", (protocol.FactoryResetCommand{}).ToBytes()},
	} {
		wrongMarker := bytes.Clone(command.wire)
		wrongMarker[1] = 'x'
		for _, tc := range []struct {
			name     string
			wire     []byte
			missing  bool
			err      error
			want     []byte
			wantCall bool
		}{
			{"accepted", command.wire, false, nil, nil, true},
			{"trailing bytes", append(bytes.Clone(command.wire), 0xaa), false, nil, nil, true},
			{"missing provider", command.wire, true, nil, []byte{protocol.RespErr, protocol.ErrCodeUnsupportedCmd}, false},
			{"rejected", command.wire, false, rejected, []byte{protocol.RespErr, protocol.ErrCodeBadState}, true},
			{"truncated", command.wire[:len(command.wire)-1], false, nil, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}, false},
			{"wrong marker", wrongMarker, false, nil, []byte{protocol.RespErr, protocol.ErrCodeIllegalArg}, false},
		} {
			t.Run(command.name+"/"+tc.name, func(t *testing.T) {
				called := false
				var reported error
				cfg := testConfig("")
				cfg.AdvertInterval = 0
				cfg.ErrorHandler = func(err error) { reported = err }
				other := func(context.Context) error { return errors.New("wrong lifecycle callback selected") }
				cfg.RequestRestart, cfg.RequestFactoryReset = other, other
				var request func(context.Context) error
				if !tc.missing {
					request = func(context.Context) error { called = true; return tc.err }
				}
				if command.wire[0] == protocol.CmdReboot {
					cfg.RequestRestart = request
				} else {
					cfg.RequestFactoryReset = request
				}
				s, err := New(testIdentity(1), newTestRadio(), cfg)
				if err != nil {
					t.Fatal(err)
				}
				t.Cleanup(func() { _ = s.Close() })
				c := commandSession(t, s)
				s.mu.Lock()
				s.command(c, tc.wire)
				s.mu.Unlock()
				if called != tc.wantCall {
					t.Fatal("request validation or callback routing was incorrect")
				}
				select {
				case got := <-c.out:
					if tc.want == nil || !bytes.Equal(got, tc.want) {
						t.Fatalf("admission response %x, want %x", got, tc.want)
					}
				default:
					if tc.want != nil {
						t.Fatal("admission failure did not produce a protocol error")
					}
				}
				if !errors.Is(reported, tc.err) {
					t.Fatalf("provider error was not reported: %v", reported)
				}
			})
		}
	}
}

func TestLifecycleAdmissionAllowsIndependentWorkerClose(t *testing.T) {
	for _, wire := range [][]byte{(protocol.RebootCommand{}).ToBytes(), (protocol.FactoryResetCommand{}).ToBytes()} {
		t.Run(fmt.Sprintf("command=%d", wire[0]), func(t *testing.T) {
			requests := make(chan struct{}, 1)
			cfg := testConfig("")
			request := func(ctx context.Context) error {
				select {
				case requests <- struct{}{}:
					return nil
				case <-ctx.Done():
					return ctx.Err()
				}
			}
			cfg.RequestRestart, cfg.RequestFactoryReset = request, request
			s, _, addr := startTestServer(t, testIdentity(1), cfg)
			conn, err := net.Dial("tcp", addr)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			workerDone := make(chan error, 1)
			ctx := testContext(t)
			go func() {
				select {
				case <-requests:
					workerDone <- s.Close()
				case <-ctx.Done():
					workerDone <- ctx.Err()
				}
			}()
			frame, err := protocol.FrameEncode(protocol.FrameTypeOutgoing, wire)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := conn.Write(frame); err != nil {
				t.Fatal(err)
			}
			select {
			case err := <-workerDone:
				if err != nil {
					t.Fatal(err)
				}
			case <-ctx.Done():
				t.Fatal("independent lifecycle worker could not close the server")
			}
			_ = conn.SetReadDeadline(time.Now().Add(time.Second))
			if frame, err := readFrame(conn); !errors.Is(err, io.EOF) {
				t.Fatalf("expected disconnect without completion response, got %x, %v", frame, err)
			}
		})
	}
}
