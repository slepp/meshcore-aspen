package companion

import meshcore "github.com/meshcore-go/meshcore-go"

type advertPath struct {
	Path    []byte
	Encoded byte
	At      uint32
}

func (s *Server) rememberAdvertPath(key [32]byte, pkt *meshcore.Packet) {
	if !pkt.IsRouteFlood() {
		return
	}
	if _, exists := s.advertPaths[key]; !exists && len(s.advertPaths) >= 16 {
		var oldest [32]byte
		timestamp := ^uint32(0)
		for k, p := range s.advertPaths {
			if p.At <= timestamp {
				oldest, timestamp = k, p.At
			}
		}
		delete(s.advertPaths, oldest)
	}
	s.advertPaths[key] = advertPath{Path: append([]byte(nil), pkt.Path...), Encoded: pkt.PathLength, At: s.now()}
}
