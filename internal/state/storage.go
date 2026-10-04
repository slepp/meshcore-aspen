package state

import (
	"errors"
	"fmt"
	"math"
	"syscall"
)

type StorageStats struct {
	UsedKB  uint32
	TotalKB uint32
}

// Storage reports the shared filesystem, not a fabricated per-role capacity.
func Storage(dir string) (StorageStats, error) {
	var info syscall.Statfs_t
	if err := syscall.Statfs(dir, &info); err != nil {
		return StorageStats{}, fmt.Errorf("reading state filesystem capacity: %w", err)
	}
	return storageStats(info.Blocks, info.Bfree, int64(info.Bsize))
}

func storageStats(blocks, free uint64, blockSize int64) (StorageStats, error) {
	if blockSize <= 0 || free > blocks {
		return StorageStats{}, errors.New("invalid state filesystem capacity")
	}
	const maxBytes = uint64(math.MaxUint32)*1024 + 1023
	if blocks > maxBytes/uint64(blockSize) {
		return StorageStats{}, errors.New("state filesystem capacity exceeds the protocol's uint32 KiB fields")
	}
	return StorageStats{
		UsedKB:  uint32((blocks - free) * uint64(blockSize) / 1024),
		TotalKB: uint32(blocks * uint64(blockSize) / 1024),
	}, nil
}
