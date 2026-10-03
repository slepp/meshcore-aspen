package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
)

func (r *checker) threeByteRelay(ctx context.Context) (result error) {
	original := r.stock.device.PathHashMode
	if err := r.stock.call(ctx, "select three-byte flood paths", func(ctx context.Context) error {
		return r.stock.client.SetPathHashMode(ctx, 2)
	}); err != nil {
		return err
	}
	defer func() {
		if r.stock.lost.Load() {
			result = errors.Join(result, errors.New("companion disconnected before its path mode could be restored"))
			return
		}
		restoreCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		result = errors.Join(result, r.stock.client.SetPathHashMode(restoreCtx, original))
	}()
	info, err := r.stock.client.DeviceQuery(ctx)
	if err != nil {
		return err
	}
	if info.PathHashMode != 2 {
		return errors.New("companion did not select three-byte flood paths")
	}
	hostMark, stockMark := r.host.capture.mark(), r.stock.capture.mark()
	if err := r.stock.call(ctx, "send three-byte flood advert", func(ctx context.Context) error {
		return r.stock.client.SendSelfAdvert(ctx, 1)
	}); err != nil {
		return err
	}
	repeater := r.roles["repeater"].PublicKey()
	var origin, forwarded *reception
	if err := wait(ctx, func() (bool, error) {
		origin = advertRX(r.host.capture.since(hostMark), r.stock.self.Identity(), "CHAT", r.options.name, true)
		if origin == nil {
			return false, nil
		}
		if origin.packet.PathHashSize() != 3 || origin.packet.PathHashCount() != 0 {
			return false, errors.New("companion advert did not originate with an empty three-byte path")
		}
		forwarded = findRX(r.stock.capture.since(stockMark), func(rx *reception) bool {
			return rx.packet.PayloadType() == meshcore.PayloadTypeAdvert && rx.packet.IsRouteFlood() &&
				rx.packet.PathHashSize() == 3 && rx.packet.PathHashCount() == 1 &&
				bytes.Equal(rx.packet.Path, repeater[:3]) &&
				bytes.Equal(rx.packet.Payload, origin.packet.Payload)
		})
		return forwarded != nil, nil
	}); err != nil {
		return fmt.Errorf("three-byte repeater forwarding (ingress=%t forwarded=%t): %w", origin != nil, forwarded != nil, err)
	}
	if err := r.proof("three-byte-relay-ingress", "host", origin); err != nil {
		return err
	}
	if err := r.proof("three-byte-relay-egress", "stock", forwarded); err != nil {
		return err
	}
	return r.emit("three-byte-relay", "verified", map[string]any{
		"repeater_public_key": r.roles["repeater"].String(),
		"scope":               "MeshCore companion received the signed packet through one repeater with an exact three-byte path",
	})
}
