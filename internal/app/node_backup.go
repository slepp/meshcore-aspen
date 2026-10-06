// SPDX-License-Identifier: Apache-2.0
package app

import (
	"context"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"

	"meshcore.local/meshcore/internal/buildinfo"
	"meshcore.local/meshcore/internal/nodebackup"
)

func hostBackupSnapshot(ctx context.Context, cfg Config) (map[string][]byte, error) {
	config, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return nil, err
	}
	environment := make(map[string]string)
	for _, name := range []string{cfg.AdminPasswordEnv, cfg.RoomPasswordEnv, cfg.AdminHTTPTokenEnv,
		cfg.MQTT.UsernameEnv, cfg.MQTT.PasswordEnv} {
		if name == "" {
			continue
		}
		value, present := os.LookupEnv(name)
		if !present {
			return nil, fmt.Errorf("backup credential environment variable %s is missing", name)
		}
		environment[name] = value
	}
	credentials, err := json.Marshal(environment)
	if err != nil {
		return nil, err
	}
	manifest, err := json.Marshal(map[string]any{
		"schema_version": 1, "format": "meshcore-node-backup", "product": "birch",
		"firmware": buildinfo.HostVersion, "contents": "configuration,identities,source,data",
	})
	if err != nil {
		return nil, err
	}
	records := map[string][]byte{"manifest.json": manifest, "config/host.json": config, "config/environment.json": credentials}
	type saved struct {
		path   string
		digest [32]byte
	}
	var inventory []saved
	total := len(config) + len(credentials) + len(manifest)
	err = filepath.WalkDir(cfg.StateDir, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if err := ctx.Err(); err != nil {
			return err
		}
		if path == cfg.StateDir {
			return nil
		}
		name := entry.Name()
		if name == "node-backup" || name == "logs" || name == ".cache" {
			if entry.IsDir() {
				return filepath.SkipDir
			}
			return nil
		}
		if entry.IsDir() {
			return nil
		}
		if name == ".lock" || strings.HasSuffix(name, ".log") || strings.HasSuffix(name, ".tmp") ||
			strings.HasSuffix(name, ".stage") || strings.HasPrefix(name, ".backup-") {
			return nil
		}
		if entry.Type()&os.ModeSocket != 0 {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("backup state symlink is unsupported: %s", name)
		}
		relative, err := filepath.Rel(cfg.StateDir, path)
		if err != nil {
			return err
		}
		if len(relative) > 93 || len(records) >= 512 {
			return errors.New("host backup filename or record count exceeds format limits")
		}
		raw, err := readHostBackupRecord(path)
		if err != nil {
			return fmt.Errorf("backup state %s: %w", relative, err)
		}
		total += len(raw) + 1024
		if total > nodebackup.RawLimit-4096 {
			clear(raw)
			return errors.New("host useful state exceeds the 2 MiB backup limit")
		}
		records["files/"+filepath.ToSlash(relative)] = raw
		inventory = append(inventory, saved{path, sha256.Sum256(raw)})
		return nil
	})
	if err == nil {
		for _, record := range inventory {
			if err = ctx.Err(); err != nil {
				break
			}
			var raw []byte
			raw, err = readHostBackupRecord(record.path)
			if err == nil && sha256.Sum256(raw) != record.digest {
				err = errors.New("host settings or data changed during backup; request a new snapshot")
			}
			clear(raw)
			if err != nil {
				break
			}
		}
	}
	if err != nil {
		for _, raw := range records {
			clear(raw)
		}
		return nil, err
	}
	return records, nil
}

func readHostBackupRecord(path string) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 || info.Size() > nodebackup.RawLimit {
		return nil, errors.New("state record is not a private bounded regular file")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	actual, err := file.Stat()
	if err != nil || !os.SameFile(info, actual) {
		return nil, errors.New("state record changed while opening")
	}
	raw, err := io.ReadAll(io.LimitReader(file, nodebackup.RawLimit+1))
	if len(raw) > nodebackup.RawLimit {
		clear(raw)
		return nil, errors.New("state record grew beyond the backup limit")
	}
	return raw, err
}
