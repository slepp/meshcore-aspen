package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"github.com/meshcore-go/meshcore-go/companion"
	"github.com/meshcore-go/meshcore-go/companion/client"
	"github.com/meshcore-go/meshcore-go/hardware"
	"meshcore.local/meshcore/internal/app"
)

func loginRole(ctx context.Context, c *client.Client, peer meshcore.Identity, password string) error {
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	results := make(chan error, 2)
	success := c.OnPush(companion.PushLoginSuccess, func(response companion.Response) {
		if login, ok := response.Data.(companion.PushLoginSuccessResponse); ok && login.PubKeyPrefix == peer.Prefix() {
			select {
			case results <- nil:
			default:
			}
		}
	})
	defer success()
	failure := c.OnPush(companion.PushLoginFail, func(response companion.Response) {
		if login, ok := response.Data.(companion.PushLoginFailResponse); ok && login.PubKeyPrefix == peer.Prefix() {
			select {
			case results <- errors.New("role rejected administrator login"):
			default:
			}
		}
	})
	defer failure()
	if err := c.SendLogin(ctx, peer, password); err != nil {
		return err
	}
	select {
	case err := <-results:
		return err
	case <-ctx.Done():
		return fmt.Errorf("administrator login: %w", ctx.Err())
	}
}

func roleCommand(ctx context.Context, c *client.Client, peer meshcore.Identity, command string) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	if _, err := c.GetWaitingMessages(ctx); err != nil {
		return "", err
	}
	if _, err := c.SendTextMessage(ctx, peer, command, companion.TxtTypeCLIData); err != nil {
		return "", err
	}
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		messages, err := c.GetWaitingMessages(ctx)
		if err != nil {
			return "", err
		}
		for _, message := range messages {
			reply := message.Contact
			if reply == nil || reply.PubKeyPrefix != peer.Prefix() || reply.TxtType != companion.TxtTypeCLIData {
				continue
			}
			if strings.HasPrefix(reply.Text, "ERR") || strings.HasPrefix(reply.Text, "Error,") {
				return "", fmt.Errorf("role CLI request failed: %s", reply.Text)
			}
			return reply.Text, nil
		}
		select {
		case <-ticker.C:
		case <-ctx.Done():
			return "", fmt.Errorf("role CLI reply: %w", ctx.Err())
		}
	}
}

func checkRoleRestarts(ctx context.Context, cfg app.Config, clients []*client.Client, modem *hardware.KissModem, rekey bool, selected checks) error {
	password := os.Getenv(cfg.AdminPasswordEnv)
	if cfg.AdminPasswordEnv == "" || password == "" {
		return errors.New("role reboot check requires a configured administrator password environment variable")
	}
	if rekey && !cfg.RoleKeyImport {
		return errors.New("role rekey check requires role_key_import=true and disposable staging identities")
	}
	for _, role := range []string{"repeater", "room"} {
		if !enabled(cfg, selected, role) {
			continue
		}
		before, err := readStatus(ctx, cfg, selected.onchip)
		if err != nil {
			return err
		}
		if before[role].ApplicationStartedAt.IsZero() && before[role].SourceGeneration == 0 {
			return fmt.Errorf("%s does not report its application start time", role)
		}
		peer, err := meshcore.NewIdentityFromHex(before[role].PublicKey)
		if err != nil {
			return err
		}
		if err := loginRole(ctx, clients[0], peer, password); err != nil {
			return fmt.Errorf("%s: %w", role, err)
		}
		name, err := roleCommand(ctx, clients[0], peer, "get name")
		if err != nil {
			return fmt.Errorf("%s: %w", role, err)
		}
		expectedKey := before[role].PublicKey
		if rekey {
			key, next, err := freshNativeKey(before)
			if err != nil {
				return err
			}
			reply, err := roleCommand(ctx, clients[0], peer, "set prv.key "+hex.EncodeToString(key))
			if err != nil {
				return fmt.Errorf("%s key staging: %w", role, err)
			}
			expectedKey = next.String()
			if reply != "OK, reboot to apply! New pubkey: "+strings.ToUpper(expectedKey) {
				return fmt.Errorf("%s key staging did not return the native reboot-to-apply response", role)
			}
			staged, err := readStatus(ctx, cfg, selected.onchip)
			if err != nil {
				return err
			}
			for sibling, old := range before {
				current := staged[sibling]
				if current.PublicKey != old.PublicKey || !sameRoleLifetime(current, old) {
					return fmt.Errorf("key staging prematurely changed or restarted %s", sibling)
				}
			}
			activeName, err := roleCommand(ctx, clients[0], peer, "get name")
			if err != nil || activeName != name {
				return fmt.Errorf("%s stopped serving its original identity after staging: %v", role, err)
			}
		}
		if _, err := clients[0].SendTextMessage(ctx, peer, "reboot", companion.TxtTypeCLIData); err != nil {
			return err
		}
		if err := waitRoleRestart(ctx, cfg, role, before, expectedKey, selected.onchip); err != nil {
			return err
		}
		if _, err := modem.Request(ctx, hardware.HW_CMD_PING, nil); err != nil {
			return fmt.Errorf("%s reboot interrupted the existing bot/modem connection: %w", role, err)
		}
		for _, c := range clients {
			info, err := c.AppStart(ctx, 3, "meshcore-host-check")
			if err != nil {
				return err
			}
			if hex.EncodeToString(info.PublicKey[:]) != before["companion"].PublicKey {
				return fmt.Errorf("%s reboot changed the companion identity", role)
			}
		}
		if rekey {
			peer, err = meshcore.NewIdentityFromHex(expectedKey)
			if err != nil {
				return err
			}
			if err := waitRoleContact(ctx, clients[0], peer); err != nil {
				return fmt.Errorf("%s new identity: %w", role, err)
			}
		}
		if err := loginRole(ctx, clients[0], peer, password); err != nil {
			return fmt.Errorf("%s after reboot: %w", role, err)
		}
		afterName, err := roleCommand(ctx, clients[0], peer, "get name")
		if err != nil {
			return fmt.Errorf("%s after reboot: %w", role, err)
		}
		if afterName != name {
			return fmt.Errorf("%s reboot changed its configured name", role)
		}
		if rekey {
			fmt.Printf("%s rekey passed: durable staging without early activation, reboot-to-apply, encrypted new-key exchange, retained name, unchanged siblings and existing bot/modem connection\n", role)
		} else {
			fmt.Printf("%s reboot passed: encrypted admin command, fresh application lifetime, retained identity/name, unchanged siblings and existing bot/modem connection\n", role)
		}
	}
	return nil
}

func sameRoleLifetime(current, previous roleStatus) bool {
	return current.SourceGeneration == previous.SourceGeneration &&
		current.ApplicationStartedAt.Equal(previous.ApplicationStartedAt) &&
		current.DeviceUptime >= previous.DeviceUptime
}

func waitRoleRestart(ctx context.Context, cfg app.Config, role string, before map[string]roleStatus, expectedKey string, onchip bool) error {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	interval := 100 * time.Millisecond
	if onchip {
		interval = time.Second
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		current, err := readStatus(ctx, cfg, onchip)
		if err != nil {
			return err
		}
		for name, old := range before {
			if old.State == "disabled" {
				continue
			}
			entry, exists := current[name]
			keyValid := entry.PublicKey == old.PublicKey || (name == role && entry.PublicKey == expectedKey)
			if !exists || !keyValid || entry.ApplicationError != "" {
				return fmt.Errorf("%s reboot changed or failed %s: %s", role, name, entry.ApplicationError)
			}
			if entry.DeviceUptime < old.DeviceUptime {
				return errors.New("role restart rebooted the physical modem")
			}
			if name != role && (!sameRoleLifetime(entry, old) ||
				(name != "bot" && (entry.Connected == nil || !*entry.Connected))) {
				return fmt.Errorf("%s reboot interrupted sibling %s", role, name)
			}
		}
		entry := current[role]
		if entry.State == "running" && !sameRoleLifetime(entry, before[role]) && entry.Connected != nil && *entry.Connected {
			if entry.PublicKey != expectedKey {
				return fmt.Errorf("%s reboot did not activate its expected identity", role)
			}
			return nil
		}
		select {
		case <-ticker.C:
		case <-ctx.Done():
			return fmt.Errorf("%s application did not restart: %w", role, ctx.Err())
		}
	}
}

func freshNativeKey(existing map[string]roleStatus) ([]byte, meshcore.Identity, error) {
	for {
		key := make([]byte, 64)
		if _, err := rand.Read(key); err != nil {
			return nil, meshcore.Identity{}, err
		}
		key[0] &= 248
		key[31] = key[31]&63 | 64
		local, err := meshcore.NewLocalIdentityFromExpandedKey(key)
		if err != nil {
			return nil, meshcore.Identity{}, err
		}
		public := local.PublicKey()
		if public[0] == 0 || public[0] == 255 {
			continue
		}
		collision := false
		for _, entry := range existing {
			collision = collision || entry.PublicKey == local.String()
		}
		if !collision {
			return key, meshcore.NewIdentity(public), nil
		}
	}
}

func waitRoleContact(ctx context.Context, c *client.Client, peer meshcore.Identity) error {
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		contacts, err := c.GetContacts(ctx)
		if err != nil {
			return err
		}
		for _, contact := range contacts {
			if contact.PublicKey == peer.PublicKey() {
				return nil
			}
		}
		select {
		case <-ticker.C:
		case <-ctx.Done():
			return fmt.Errorf("new role advert was not discovered: %w", ctx.Err())
		}
	}
}
