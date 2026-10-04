package policy

import (
	"strings"

	meshcore "github.com/meshcore-go/meshcore-go"
)

// AutoScope derives the public hashtag key; an implicit name receives '#'.
// Private '$' regions must receive explicit keys and are never auto-derived.
func AutoScope(name string) Scope {
	s := Scope{Name: name}
	if name == "" || strings.HasPrefix(name, "$") {
		return s
	}
	if !strings.HasPrefix(name, "#") {
		name = "#" + name
	}
	s.Key = [16]byte(meshcore.DeriveRegionKey(name))
	return s
}

// TransportCode authenticates the payload type and payload only. Packet route,
// path and the existing transport codes do not participate in the native HMAC.
func TransportCode(scope Scope, payloadType uint8, payload []byte) uint16 {
	return meshcore.RegionKey(scope.Key).CalcTransportCode(payloadType, payload)
}

func regionScopes(r Region) []Scope {
	if !strings.HasPrefix(r.Name, "$") {
		return []Scope{AutoScope(r.Name)}
	}
	scopes := make([]Scope, len(r.Keys))
	for i, key := range r.Keys {
		scopes[i] = Scope{Name: r.Name, Key: key}
	}
	return scopes
}

// ResolveReceiveContext captures the native region lookup before the packet is
// queued. Region ancestry is management/export metadata, not inherited admission.
// A denied unscoped packet has no wildcard match and therefore may receive a
// reply using the default scope, just like native sendFloodReply.
func ResolveReceiveContext(p Preferences, route Route, path Path, payloadType uint8, payload []byte, transportCode uint16) ReceiveContext {
	ctx := ReceiveContext{Route: route, Path: path}
	if route == Flood {
		ctx.Unscoped = p.WildcardFlags&RegionDenyFlood == 0
		return ctx
	}
	if route != TransportFlood {
		return ctx
	}
	for _, region := range p.Regions {
		if region.Flags&RegionDenyFlood != 0 {
			continue
		}
		scopes := regionScopes(region)
		for _, scope := range scopes {
			if TransportCode(scope, payloadType, payload) == transportCode {
				ctx.ScopeKnown, ctx.RegionID = true, region.ID
				// Native replies use the first configured key, not necessarily
				// the key that matched this request during a key rotation.
				ctx.Scope = scopes[0]
				return ctx
			}
		}
	}
	return ctx
}

// AllowForward includes the ordinary path capacity gate from Mesh::routeRecvPacket
// and the selected application's allowPacketForward. Local processing is separate:
// rejecting forwarding does not reject receipt by this logical role.
func AllowForward(profile Profile, p Preferences, ctx ReceiveContext, payloadType uint8, selfPublicKey [32]byte) bool {
	if !validProfile(profile) || !p.Repeat || ctx.Route > TransportDirect {
		return false
	}
	if !ctx.Route.IsFlood() {
		return true
	}
	if payloadType == payloadTrace || !ctx.Path.Known() || ctx.Path.Count() >= 63 ||
		(int(ctx.Path.Count())+1)*int(ctx.Path.Width()) > 64 {
		return false
	}
	if profile == Companion {
		return true
	}
	hops := ctx.Path.Count()
	if hops >= p.FloodMaxHops || (ctx.Route == Flood && hops >= p.UnscopedMaxHops) ||
		(payloadType == payloadAdvert && hops >= p.AdvertMaxHops) {
		return false
	}
	if profile == Room {
		return true
	}
	if !ctx.ScopeKnown && !ctx.Unscoped {
		return false
	}
	if p.Loop == LoopOff {
		return true
	}
	if p.Loop > LoopStrict {
		return false
	}
	thresholds := [3][3]int{{4, 2, 1}, {2, 1, 1}, {1, 1, 1}}
	width := int(ctx.Path.Width())
	path := ctx.Path.Bytes()
	matches := 0
	for start := 0; start < len(path); start += width {
		equal := true
		for i := 0; i < width; i++ {
			if path[start+i] != selfPublicKey[i] {
				equal = false
				break
			}
		}
		if equal {
			matches++
		}
	}
	return matches < thresholds[p.Loop-1][width-1]
}
