package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"

	meshcore "github.com/meshcore-go/meshcore-go"
	protocol "github.com/meshcore-go/meshcore-go/companion"
)

func repairContact(ctx context.Context, options options, out io.Writer) (result error) {
	identity, err := meshcore.NewIdentityFromHex(options.repairContact)
	if err != nil || identity.IsZero() {
		return fmt.Errorf("repair-contact requires a valid full public key")
	}
	ctx, cancel := context.WithCancelCause(ctx)
	defer cancel(nil)
	peer := newEndpoint("contact recovery", newStockSerialTransport(options.serial, out), cancel)
	defer func() { result = errors.Join(result, peer.close()) }()
	if err := peer.handshake(ctx); err != nil {
		return err
	}
	return peer.call(ctx, "reset saved advert timestamp", func(ctx context.Context) error {
		saved, err := peer.client.GetContactByKey(ctx, identity)
		if err != nil {
			return err
		}
		clock, err := peer.client.GetDeviceTime(ctx)
		if err != nil {
			return err
		}
		if err := peer.client.AddUpdateContactFull(ctx, protocol.AddUpdateContactCommand{
			PublicKey: saved.PublicKey, Type: saved.Type, Flags: saved.Flags,
			OutPathLen: saved.OutPathLen, OutPath: saved.OutPath[:],
			Name: saved.AdvertName, Latitude: saved.AdvertLatitude, Longitude: saved.AdvertLongitude,
			LastAdvert: 0, LastModified: clock.Timestamp,
		}); err != nil {
			return err
		}
		actual, err := peer.client.GetContactByKey(ctx, identity)
		if err != nil {
			return err
		}
		if actual.LastAdvert != 0 || actual.PublicKey != saved.PublicKey ||
			actual.AdvertName != saved.AdvertName || actual.Flags != saved.Flags ||
			actual.Type != saved.Type || actual.OutPath != saved.OutPath || actual.OutPathLen != saved.OutPathLen {
			return fmt.Errorf("contact recovery readback did not preserve settings")
		}
		return json.NewEncoder(out).Encode(map[string]any{
			"contact": identity.String(), "previous_advert_epoch": saved.LastAdvert,
			"advert_epoch": actual.LastAdvert, "result": "ready for a fresh signed advert",
		})
	})
}
