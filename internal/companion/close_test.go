package companion

import (
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
)

func TestCloseFlushesStateAndReturnsPersistenceFailures(t *testing.T) {
	dir := stateDir(t)
	s, err := New(testIdentity(1), newTestRadio(), testConfig(dir))
	if err != nil {
		t.Fatal(err)
	}
	blockStateReplacement(t, dir)
	first := s.Close()
	if first == nil {
		t.Fatal("Close suppressed its final persistence failure")
	}
	if again := s.Close(); again == nil || again.Error() != first.Error() {
		t.Fatalf("repeated Close lost persistence error: %v", again)
	}
	select {
	case <-s.workerDone:
	default:
		t.Fatal("Close returned before its worker stopped")
	}
}

func blockStateReplacement(t *testing.T, dir string) func() {
	t.Helper()
	path := filepath.Join(dir, "companion.json")
	backup := filepath.Join(dir, "companion.before")
	if err := os.Rename(path, backup); err != nil {
		t.Fatal(err)
	}
	var once sync.Once
	restore := func() {
		once.Do(func() {
			if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
				t.Error(err)
				return
			}
			if err := os.Rename(backup, path); err != nil {
				t.Error(err)
			}
		})
	}
	t.Cleanup(restore)
	if err := os.Mkdir(path, 0700); err != nil {
		t.Fatal(err)
	}
	return restore
}
