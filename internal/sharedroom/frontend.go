package sharedroom

import (
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/meshcore-go/meshcore-go/node"
	"meshcore.local/meshcore/internal/radio"
)

type Submit func(context.Context, []byte, time.Duration) string

// Submitter uses the final Birch PHY receipt, rather than queue acceptance.
func Submitter(link *radio.Link) Submit {
	return func(ctx context.Context, wire []byte, delay time.Duration) string {
		receipt, err := link.SubmitWithReceipt(wire, node.PrioritySend, delay, 30*time.Second)
		if err != nil {
			if errors.Is(err, radio.ErrTXOutcomeUnknown) {
				return "unknown"
			}
			return "failed"
		}
		select {
		case result := <-receipt.Result:
			if result.State == radio.TXSucceeded {
				return "sent"
			}
			if result.State == radio.TXUnknown {
				return "unknown"
			}
			return "failed"
		case <-ctx.Done():
			return "unknown"
		}
	}
}

type job struct {
	alias string
	run   func() error
}
type Frontend struct {
	ctx     context.Context
	cancel  context.CancelFunc
	codec   Codec
	clients map[string]*Client
	submit  Submit
	report  func(error)
	jobs    chan job
	wg      sync.WaitGroup
	close   sync.Once
}

func Start(ctx context.Context, api, token string, identities []Identity, slot time.Duration, submit Submit, report func(error)) (*Frontend, error) {
	if len(identities) == 0 || submit == nil || slot < 10*time.Second {
		return nil, errors.New("shared room requires identities, radio submitter and advertisement slots >=10s")
	}
	aliases, keys := make(map[string]bool), make(map[string]bool)
	for _, identity := range identities {
		if identity.Alias == "" || identity.Name == "" || len(identity.Name) > 31 || identity.PathHashMode > 2 || aliases[identity.Alias] || keys[identity.Key.String()] {
			return nil, errors.New("configure distinct named shared-room identities with valid path width")
		}
		aliases[identity.Alias], keys[identity.Key.String()] = true, true
	}
	ctx, cancel := context.WithCancel(ctx)
	f := &Frontend{ctx: ctx, cancel: cancel, codec: Codec{Identities: identities}, clients: make(map[string]*Client),
		submit: submit, report: report, jobs: make(chan job, 128)}
	if f.report == nil {
		f.report = func(error) {}
	}
	// Clients can receive queued deliveries while the remaining aliases connect;
	// start the TX worker only after the immutable client map is complete.
	for _, identity := range identities {
		c, err := newClient(ctx, api, token, identity, f.delivery, f.report)
		if err != nil {
			cancel()
			for _, client := range f.clients {
				client.Close()
			}
			return nil, err
		}
		f.clients[identity.Alias] = c
	}
	f.wg.Add(2)
	go f.tx()
	go f.advertise(slot)
	return f, nil
}
func (f *Frontend) Close() {
	f.close.Do(func() {
		f.cancel()
		for _, client := range f.clients {
			client.Close()
		}
		f.wg.Wait()
	})
}
func (f *Frontend) lookup(ctx context.Context, alias, prefix string) ([]Member, error) {
	return f.clients[alias].Members(ctx, prefix)
}

// Receive accepts measured RF bytes only. The radio callback filters reflections.
func (f *Frontend) Receive(ctx context.Context, wire []byte) error {
	decoded, err := f.codec.Decode(ctx, wire, f.lookup)
	if err != nil || decoded == nil {
		return err
	}
	result, err := f.clients[decoded.Identity.Alias].Call(ctx, decoded.Operation)
	if err != nil {
		return err
	}
	if result.Respond {
		f.enqueue(job{decoded.Identity.Alias, func() error {
			packet, delay, err := f.codec.Response(decoded, result)
			if err != nil || packet == nil {
				return err
			}
			wire, err := packet.ToBytes()
			if err != nil {
				return err
			}
			if outcome := f.submit(f.ctx, wire, delay); outcome != "sent" {
				return fmt.Errorf("%s RF response %s; wait for client retry", decoded.Identity.Alias, outcome)
			}
			return nil
		}})
	}
	return nil
}
func (f *Frontend) delivery(delivery Delivery) {
	f.enqueue(job{delivery.Alias, func() error {
		var identity Identity
		for _, candidate := range f.codec.Identities {
			if candidate.Alias == delivery.Alias {
				identity = candidate
				break
			}
		}
		packet, proof, err := f.codec.Delivery(identity, delivery)
		if err != nil {
			return err
		}
		client := f.clients[delivery.Alias]
		prepared, err := client.Call(f.ctx, Operation{Op: "prepare", Client: delivery.Client, DeliveryID: delivery.DeliveryID, Proof: proof})
		if err != nil || !prepared.Transmit {
			return err
		}
		wire, err := packet.ToBytes()
		if err != nil {
			return err
		}
		outcome := f.submit(f.ctx, wire, 1200*time.Millisecond)
		_, err = client.Call(f.ctx, Operation{Op: "receipt", Client: delivery.Client, DeliveryID: delivery.DeliveryID, Outcome: outcome})
		return err
	}})
}
func (f *Frontend) enqueue(j job) {
	select {
	case f.jobs <- j:
	case <-f.ctx.Done():
	default:
		f.report(fmt.Errorf("%s physical TX queue full; refresh on next client request", j.alias))
	}
}
func (f *Frontend) tx() {
	defer f.wg.Done()
	queues := make(map[string][]job)
	aliases := make([]string, 0, len(f.codec.Identities))
	for _, identity := range f.codec.Identities {
		aliases = append(aliases, identity.Alias)
	}
	next := 0
	for {
		if f.ctx.Err() != nil {
			return
		}
		have := false
		for _, queue := range queues {
			have = have || len(queue) > 0
		}
		if !have {
			select {
			case <-f.ctx.Done():
				return
			case j := <-f.jobs:
				queues[j.alias] = append(queues[j.alias], j)
			}
		}
		// Bound intake so a busy alias cannot prevent selecting another identity.
		for i := 0; i < 128; i++ {
			select {
			case j := <-f.jobs:
				if len(queues[j.alias]) < 64 {
					queues[j.alias] = append(queues[j.alias], j)
				} else {
					f.report(fmt.Errorf("%s TX queue full; refresh on next client request", j.alias))
				}
			default:
				i = 128
			}
		}
		for i := 0; i < len(aliases); i++ {
			alias := aliases[next%len(aliases)]
			next++
			queue := queues[alias]
			if len(queue) == 0 {
				continue
			}
			queues[alias] = queue[1:]
			if err := queue[0].run(); err != nil && f.ctx.Err() == nil {
				f.report(err)
			}
			break
		}
	}
}
func (f *Frontend) advertise(slot time.Duration) {
	defer f.wg.Done()
	next := 0
	clocks := make(map[string]uint32)
	for {
		var random [4]byte
		rand.Read(random[:])
		jitter := time.Duration(binary.LittleEndian.Uint32(random[:])%1000) * slot / 10000
		timer := time.NewTimer(slot + jitter)
		select {
		case <-f.ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
		identity := f.codec.Identities[next%len(f.codec.Identities)]
		next++
		if !f.clients[identity.Alias].Connected() {
			continue
		}
		stamp := uint32(time.Now().Unix())
		if stamp <= clocks[identity.Alias] {
			stamp = clocks[identity.Alias] + 1
		}
		clocks[identity.Alias] = stamp
		f.enqueue(job{identity.Alias, func() error {
			packet, err := f.codec.Advertisement(identity, stamp)
			if err != nil {
				return err
			}
			wire, err := packet.ToBytes()
			if err != nil {
				return err
			}
			if outcome := f.submit(f.ctx, wire, 0); outcome != "sent" {
				return fmt.Errorf("%s advertisement %s", identity.Alias, outcome)
			}
			return nil
		}})
	}
}
