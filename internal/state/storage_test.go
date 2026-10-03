package state

import (
	"math"
	"testing"
)

func TestStorageProtocolUnitsAndBounds(t *testing.T) {
	got, err := storageStats(100, 75, 4096)
	if err != nil || got.UsedKB != 100 || got.TotalKB != 400 {
		t.Fatalf("used/total KiB: got %+v, %v", got, err)
	}
	got, err = storageStats(math.MaxUint32, 0, 1024)
	if err != nil || got.UsedKB != math.MaxUint32 || got.TotalKB != math.MaxUint32 {
		t.Fatalf("largest representable capacity: %+v, %v", got, err)
	}
	for _, tc := range []struct {
		blocks, free uint64
		size         int64
	}{
		{math.MaxUint32 + 1, 0, 1024},
		{math.MaxUint64, 0, 4096},
		{100, 101, 4096},
		{100, 75, 0},
		{100, 75, -1},
	} {
		if _, err := storageStats(tc.blocks, tc.free, tc.size); err == nil {
			t.Fatalf("unrepresentable capacity silently accepted: %+v", tc)
		}
	}
}
