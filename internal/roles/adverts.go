package roles

import meshcore "github.com/meshcore-go/meshcore-go"

func (s *Service) advertisement(flood bool) (*meshcore.Packet, error) {
	kind := "REPEATER"
	if s.room {
		kind = "ROOM"
	}
	data := &meshcore.AdvertAppData{Type: kind, Name: s.state.Name}
	if s.state.AdvertLocation != 0 {
		data.Lat, data.Lon = int32(s.state.Latitude*1e6), int32(s.state.Longitude*1e6)
		data.HasLocation = true
	}
	app, err := data.ToBytes()
	if err != nil {
		return nil, err
	}
	advert := &meshcore.Advert{PublicKey: s.id.Identity, Timestamp: s.uniqueTime(), RawAppData: app}
	advert.SignWith(s.id)
	payload, err := advert.ToBytes()
	if err != nil {
		return nil, err
	}
	p := &meshcore.Packet{Header: meshcore.MakeHeader(meshcore.RouteTypeDirect, meshcore.PayloadTypeAdvert, 0), Payload: payload}
	if flood {
		p.PathLength = byte(s.state.Preferences.PathHashMode) << 6
		applyScope(p, s.state.Preferences.DefaultScope)
	}
	return p, nil
}
