package policy

type Route uint8

const (
	TransportFlood Route = iota
	Flood
	Direct
	TransportDirect
)

func (r Route) IsFlood() bool { return r == Flood || r == TransportFlood }

// ReceiveContext is copied with each received packet before asynchronous work.
// ScopeKnown is false when no permitted region resolved the incoming scope.
type ReceiveContext struct {
	Route      Route
	Path       Path
	ScopeKnown bool
	Scope      Scope
	RegionID   uint16
	Unscoped   bool
}

type ReplyRoute uint8

const (
	ReplyPathReturn ReplyRoute = iota
	ReplyDirectSupplied
	ReplyDirectLearned
	ReplyFlood
)

func ChooseReplyRoute(incomingFlood, suppliedPath, learnedPath bool) ReplyRoute {
	if incomingFlood {
		return ReplyPathReturn
	}
	if suppliedPath {
		return ReplyDirectSupplied
	}
	if learnedPath {
		return ReplyDirectLearned
	}
	return ReplyFlood
}

func ChooseReplyScope(ctx ReceiveContext, fallback Scope) Scope {
	if ctx.ScopeKnown {
		return ctx.Scope
	}
	if ctx.Unscoped {
		return Scope{}
	}
	return fallback
}

// SendScope is session-local; Preferences.DefaultScope is role-wide.
type SendScope struct {
	Override Scope
	Unscoped bool
}

func (s SendScope) Resolve(fallback Scope) Scope {
	if s.Unscoped {
		return Scope{}
	}
	if !s.Override.IsNull() {
		return s.Override
	}
	return fallback
}
