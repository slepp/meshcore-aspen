package app

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
	"meshcore.local/meshcore/internal/state"
)

func routingReply(t *testing.T, link *routingLink, peer, server meshcore.LocalIdentity) string {
	t.Helper()
	packet := routingNext(t, link)
	if packet.PayloadType() != meshcore.PayloadTypeTxtMsg || len(packet.Payload) < 4 {
		t.Fatal("expected encrypted CLI reply")
	}
	key, err := peer.SharedSecret(server.Identity)
	if err != nil {
		t.Fatal(err)
	}
	plain, err := meshcore.MACThenDecrypt(key, packet.Payload[2:])
	if err != nil || len(plain) < 5 || plain[4]>>2 != 1 {
		t.Fatalf("invalid encrypted CLI reply: %v", err)
	}
	return string(bytes.TrimRight(plain[5:], "\x00"))
}

func candidateIdentity(t *testing.T, dir string) (meshcore.LocalIdentity, []byte) {
	t.Helper()
	id, err := state.Identity(dir, "candidate")
	if err != nil {
		t.Fatal(err)
	}
	key, err := state.ExportIdentity(dir, "candidate")
	if err != nil {
		t.Fatal(err)
	}
	return id, key
}

func TestHostIdentityActivationSkipsDisabledRoles(t *testing.T) {
	cfg := DefaultConfig()
	cfg.StateDir = t.TempDir()
	cfg.EnabledRoles = []string{}
	if err := activatePendingRoleIdentities(cfg); err != nil {
		t.Fatal(err)
	}
	for _, role := range []string{"repeater", "room"} {
		if _, err := os.Stat(filepath.Join(cfg.StateDir, role)); !os.IsNotExist(err) {
			t.Fatalf("disabled %s state touched: %v", role, err)
		}
	}
}

func TestNativeRoleKeyStageActivatesOnlyAfterLogicalOrHostRestart(t *testing.T) {
	for _, role := range []string{"room", "repeater"} {
		for _, hostRestart := range []bool{false, true} {
			mode := map[bool]string{false: "logical", true: "host"}[hostRestart]
			t.Run(role+"/"+mode, func(t *testing.T) {
				cfg, ids := lifecycleConfig(t)
				cfg.RoleKeyImport = true
				first, second := newRoutingLink(), newRoutingLink()
				var opens atomic.Int32
				retiring, release := make(chan struct{}), make(chan struct{})
				var once sync.Once
				unblock := func() { once.Do(func() { close(release) }) }
				var started atomic.Bool
				first.beforeClose = func() {
					if started.Load() && !hostRestart {
						close(retiring)
						<-release
					}
				}
				worker := routingStart(t, context.Background(), cfg, role, func(context.Context) (sourceLink, error) {
					if opens.Add(1) == 1 {
						return first, nil
					}
					if !first.retired.Load() || !first.nodeRadio.closed.Load() {
						return nil, errors.New("activation preceded old source/service retirement")
					}
					return second, nil
				})
				t.Cleanup(unblock)
				started.Store(true)
				routingNext(t, first)
				start := worker.status().ApplicationStartedAt
				peer := meshcore.NewLocalIdentityFromSeed([32]byte{77})
				routingLogin(t, first, role, peer, ids[role])
				next, key := candidateIdentity(t, cfg.StateDir)
				routingCommand(t, first, peer, ids[role], 2, "set prv.key "+hex.EncodeToString(key))
				reply := routingReply(t, first, peer, ids[role])
				if reply != "OK, reboot to apply! New pubkey: "+strings.ToUpper(next.String()) {
					t.Fatal("key stage did not return reboot-to-apply reply")
				}
				if status := worker.status(); status.PublicKey != ids[role].String() || !status.ApplicationStartedAt.Equal(*start) {
					t.Fatal("staging changed running identity or lifetime")
				}
				routingCommand(t, first, peer, ids[role], 3, "set name After staging")
				routingReply(t, first, peer, ids[role])
				if active, err := state.Identity(cfg.StateDir, role); err != nil || active.PublicKey() != ids[role].PublicKey() {
					t.Fatalf("staging or normal save prematurely activated the key: %v", err)
				}
				if hostRestart {
					if err := worker.Close(); err != nil {
						t.Fatal(err)
					}
					if active, err := state.Identity(cfg.StateDir, role); err != nil || active.PublicKey() != ids[role].PublicKey() {
						t.Fatalf("Close alone activated pending key: %v", err)
					}
					cfg.EnabledRoles = []string{}
					if err := activatePendingRoleIdentities(cfg); err != nil {
						t.Fatal(err)
					}
					if active, err := state.Identity(cfg.StateDir, role); err != nil || active.PublicKey() != ids[role].PublicKey() {
						t.Fatalf("disabled role's pending identity was activated: %v", err)
					}
					cfg.EnabledRoles = nil
					// Exercise the exact pre-identity-selection startup helper,
					// never the actual host or any hardware constructor.
					if err := activatePendingRoleIdentities(cfg); err != nil {
						t.Fatal(err)
					}
					cfg.RoleKeyImport = false
					worker = routingStart(t, context.Background(), cfg, role, func(context.Context) (sourceLink, error) {
						return second, nil
					})
				} else {
					routingCommand(t, first, peer, ids[role], 4, "reboot")
					lifecycleWait(t, func() bool {
						select {
						case <-retiring:
							return true
						default:
							return false
						}
					})
					if active, err := state.Identity(cfg.StateDir, role); err != nil || active.PublicKey() != ids[role].PublicKey() {
						t.Fatalf("key activated before source retirement: %v", err)
					}
					unblock()
					lifecycleWait(t, func() bool { return worker.status().State == "running" && opens.Load() == 2 })
				}
				advert := routingNext(t, second)
				if len(advert.Payload) < 32 || !bytes.Equal(advert.Payload[:32], next.PublicKeyBytes()) {
					t.Fatal("new application did not advertise activated identity")
				}
				status := worker.status()
				if status.PublicKey != next.String() || status.ApplicationStartedAt == nil ||
					!status.ApplicationStartedAt.After(*start) || status.ApplicationError != "" {
					t.Fatalf("activation status incoherent: %+v", status)
				}
				routingCommand(t, second, peer, next, 5, "get name")
				if got := routingReply(t, second, peer, next); got != "> After staging" {
					t.Fatalf("post-rekey encrypted exchange lost saved state: %q", got)
				}
				raw, err := os.ReadFile(filepath.Join(cfg.StateDir, role, "identity-state.json"))
				if err != nil {
					t.Fatal(err)
				}
				var envelope struct {
					Pending json.RawMessage `json:"pending_identity"`
					State   struct {
						Identity string
						Name     string
					} `json:"state"`
				}
				if err := json.Unmarshal(raw, &envelope); err != nil || len(envelope.Pending) != 0 ||
					envelope.State.Identity != next.String() || envelope.State.Name != "After staging" {
					t.Fatal("activation failed to clear pending or rebind the authoritative document")
				}
				for sibling, want := range ids {
					if sibling == role {
						continue
					}
					got, err := state.Identity(cfg.StateDir, sibling)
					if err != nil || got.PublicKey() != want.PublicKey() {
						t.Fatalf("activation changed %s identity: %v", sibling, err)
					}
				}
			})
		}
	}
}

func TestCompanionImportChecksPendingAndCurrentCanonicalRoleKeys(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	first, second := newRoutingLink(), newRoutingLink()
	var opens atomic.Int32
	room := routingStart(t, context.Background(), cfg, "room", func(context.Context) (sourceLink, error) {
		if opens.Add(1) == 1 {
			return first, nil
		}
		return second, nil
	})
	companion := lifecycleStart(t, context.Background(), cfg, ids, func(context.Context) (sourceLink, error) {
		return &lifecycleLink{}, nil
	})
	conn := lifecycleDial(t, companion.status().Endpoint)
	oldRoomKey, err := state.ExportIdentity(cfg.StateDir, "room")
	if err != nil {
		t.Fatal(err)
	}
	next, key := candidateIdentity(t, cfg.StateDir)
	if err := state.StageRoleIdentity(context.Background(), cfg.StateDir, "room", key); err != nil {
		t.Fatal(err)
	}
	command := append([]byte{protocol.CmdImportPrivateKey}, key...)
	if got := lifecycleCommand(t, conn, command); len(got) != 2 || got[0] != protocol.RespErr {
		t.Fatal("companion import stole a pending room key")
	}
	if err := room.request(context.Background()); err != nil {
		t.Fatal(err)
	}
	lifecycleWait(t, func() bool { return room.status().State == "running" && room.status().PublicKey == next.String() })
	if got := lifecycleCommand(t, conn, command); len(got) != 2 || got[0] != protocol.RespErr {
		t.Fatal("companion import stole a newly activated room key")
	}
	if got := lifecycleCommand(t, conn, append([]byte{protocol.CmdImportPrivateKey}, oldRoomKey...)); !bytes.Equal(got, []byte{protocol.RespOk}) {
		t.Fatal("companion import still reserved room's stale startup key")
	}
	if companion.status().PublicKey != ids["room"].String() {
		t.Fatal("companion did not activate the now-unreserved old room key")
	}
}

func TestUncertainOldClosePreventsPendingKeyActivation(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	link := newRoutingLink()
	link.nodeRadio.closeErr = state.ErrCommitIndeterminate
	var opens atomic.Int32
	worker := routingStart(t, context.Background(), cfg, "room", func(context.Context) (sourceLink, error) {
		opens.Add(1)
		return link, nil
	})
	_, key := candidateIdentity(t, cfg.StateDir)
	if err := state.StageRoleIdentity(context.Background(), cfg.StateDir, "room", key); err != nil {
		t.Fatal(err)
	}
	if err := worker.request(context.Background()); err != nil {
		t.Fatal(err)
	}
	lifecycleWait(t, func() bool { return worker.status().ApplicationError != "" })
	if err := worker.Close(); !errors.Is(err, state.ErrCommitIndeterminate) {
		t.Fatalf("indeterminate Close classification lost: %v", err)
	}
	if active, err := state.Identity(cfg.StateDir, "room"); err != nil || active.PublicKey() != ids["room"].PublicKey() ||
		opens.Load() != 1 || !link.retired.Load() || worker.status().ApplicationStartedAt != nil {
		t.Fatalf("uncertain old Close still activated/reopened: %v", err)
	}
}

func TestRoleKeyImportIsIndependentAndDisabledByDefault(t *testing.T) {
	cfg, ids := lifecycleConfig(t)
	link := newRoutingLink()
	worker := routingStart(t, context.Background(), cfg, "room", func(context.Context) (sourceLink, error) {
		return link, nil
	})
	routingNext(t, link)
	peer := meshcore.NewLocalIdentityFromSeed([32]byte{77})
	routingLogin(t, link, "room", peer, ids["room"])
	_, key := candidateIdentity(t, cfg.StateDir)
	routingCommand(t, link, peer, ids["room"], 2, "set prv.key "+hex.EncodeToString(key))
	if got := routingReply(t, link, peer, ids["room"]); got != "Error, key import disabled" {
		t.Fatal("companion opt-in accidentally enabled role key import")
	}
	if worker.status().PublicKey != ids["room"].String() {
		t.Fatal("disabled staging changed role identity")
	}
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, []byte(`{"role_key_import":true}`), 0600); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadConfig(path)
	if err != nil || !loaded.RoleKeyImport || loaded.CompanionKeyImport {
		t.Fatalf("role-key opt-in was lost or enabled companion import: %v", err)
	}
}
