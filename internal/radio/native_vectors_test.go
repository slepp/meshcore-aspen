package radio

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"
	"time"

	"github.com/meshcore-go/meshcore-go/node"
)

func TestQueuedAdapterPreservesNativeOriginVectors(t *testing.T) {
	file, err := os.Open("../../testdata/parity/native-events.jsonl")
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	phy := newTestPHY(t)
	phy.parity, phy.jobs = true, make(chan []byte, 32)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	link, err := Open(ctx, queuedConfig(phy, true))
	if err != nil {
		t.Fatal(err)
	}
	defer link.Close()
	radio, stop := link.Radio()
	defer stop()
	tx := radio.(node.TxRadio)
	scanner := bufio.NewScanner(file)
	count := 0
	for scanner.Scan() {
		var event struct {
			Scenario string `json:"scenario"`
			Event    string `json:"event"`
			Priority uint8  `json:"priority"`
			Eligible int64  `json:"eligible_ms"`
			Bytes    string `json:"bytes"`
		}
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil {
			t.Fatal(err)
		}
		if event.Scenario != "originated-priorities" || event.Event != "enqueue" {
			continue
		}
		data, err := hex.DecodeString(event.Bytes)
		if err != nil {
			t.Fatal(err)
		}
		delay := time.Duration(event.Eligible-1000) * time.Millisecond
		start := time.Now()
		if !tx.Enqueue(data, event.Priority, delay) {
			t.Fatalf("native vector rejected: %+v", event)
		}
		select {
		case wire := <-phy.jobs:
			remaining := time.Duration(binary.LittleEndian.Uint32(wire[10:])) * time.Millisecond
			if wire[9] != event.Priority || remaining > delay || remaining < max(0, delay-time.Since(start)) ||
				string(wire[18:]) != string(data) {
				t.Fatalf("native origin scheduling changed in PHY submission: vector=%+v wire=%x", event, wire)
			}
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
		count++
	}
	if err := scanner.Err(); err != nil {
		t.Fatal(err)
	}
	if count == 0 {
		t.Fatal("native origin corpus selected no cases")
	}
	t.Logf("preserved %d actual-native originated packet/priority/eligibility vectors", count)
}
