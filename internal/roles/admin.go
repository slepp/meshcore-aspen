package roles

import (
	"encoding/hex"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	meshcore "github.com/meshcore-go/meshcore-go"
	"meshcore.local/meshcore/internal/buildinfo"
)

func (s *Service) command(command string) (string, []transmission, error) {
	return s.commandAt(0, command)
}
func (s *Service) commandAt(timestamp uint32, command string) (string, []transmission, error) {
	return s.commandWithTelemetry(timestamp, command, Telemetry{}, nil)
}

func splitCommand(command string) (string, string) {
	command = strings.TrimLeft(command, " \t")
	if len(command) > 3 && command[2] == '|' {
		return command[:3], strings.TrimLeft(command[3:], " \t")
	}
	return "", command
}

func (s *Service) commandWithTelemetry(timestamp uint32, command string, telemetry Telemetry, telemetryErr error) (string, []transmission, error) {
	if s.regionLoad != nil {
		if s.regionLoad.owner != s.commandOwner {
			return "Err - region load owned by another administrator", nil, nil
		}
		return s.regionLoadLine(command), nil, nil
	}
	prefix, command := splitCommand(command)
	reply := ""
	var packets []transmission
	switch {
	case command == "help":
		reply = "help get|set|wifi|radio|region|owner|stats; stats; ver; board; clock; advert; discover.neighbors; neighbors; reboot"
	case command == "stats" || strings.HasPrefix(command, "stats "):
		reply = s.statsCommand(strings.TrimPrefix(command, "stats"), telemetry, telemetryErr)
	case strings.HasPrefix(command, "help "):
		reply = s.commandHelp(strings.TrimSpace(strings.TrimPrefix(command, "help ")))
	case command == "wifi" || command == "wifi help" || strings.HasPrefix(command, "wifi "):
		reply = s.commandHelp("wifi")
	case strings.HasPrefix(command, "set prv.key "):
		if s.cfg.StageIdentity == nil {
			reply = "Error, key import disabled"
		} else {
			stage, err := parseIdentityStage(strings.TrimPrefix(command, "set prv.key "))
			if err != nil {
				reply = "Error, bad key"
			} else {
				stage.prefix = strings.Clone(prefix)
				s.identityStage = stage
				return "", nil, nil
			}
		}
	case strings.HasPrefix(command, "reboot"):
		if s.cfg.RequestRestart == nil {
			reply = "Error, role restart unavailable"
		} else {
			s.requestRestart, s.restartPrefix = true, prefix
			return "", nil, nil
		}
	case command == "log start":
		if err := s.preparePacketLog(); err != nil {
			return "", nil, err
		}
		s.state.Logging = true
		reply = "   logging on"
	case command == "log stop":
		s.state.Logging = false
		reply = "   logging off"
	case command == "log erase":
		s.eraseLog = true
		reply = "   log erased"
	case strings.HasPrefix(command, "password "):
		value := strings.TrimPrefix(command, "password ")
		if len(value) > 15 {
			value = value[:15]
		}
		s.state.AdminPasswordOverride = &value
		disableInitialAdverts(&s.state.Preferences)
		reply = "password now: " + value
	case command == "gps advert none":
		s.state.AdvertLocation = 0
		disableInitialAdverts(&s.state.Preferences)
		reply = "ok"
	case command == "gps advert prefs":
		s.state.AdvertLocation = 2
		disableInitialAdverts(&s.state.Preferences)
		reply = "ok"
	case command == "gps advert":
		reply = "> none"
		if s.state.AdvertLocation == 2 {
			reply = "> prefs"
		}
	case command == "region" || strings.HasPrefix(command, "region "):
		reply = s.regionCommand(command)
	case command == "get name":
		reply = "> " + s.state.Name
	case command == "ver":
		level := 2
		if s.room {
			level = 1
		}
		reply = fmt.Sprintf("Birch %s (MeshCore %s; room/repeater protocol level %d)",
			buildinfo.ProductVersion, buildinfo.MeshCoreReference, level)
	case !s.room && command == "neighbors":
		list := append([]neighbour(nil), s.neighbours...)
		sort.SliceStable(list, func(i, j int) bool { return list[i].Heard > list[j].Heard })
		for _, n := range list {
			if len(reply) >= 134 {
				break
			}

			if reply != "" {
				reply += "\n"
			}
			reply += fmt.Sprintf("%X:%d:%d", n.Key[:4], s.now()-n.Heard, n.SNR)
		}
		if reply == "" {
			reply = "-none-"
		}
	case !s.room && strings.HasPrefix(command, "neighbor.remove "):
		key, err := hex.DecodeString(strings.TrimSpace(strings.TrimPrefix(command, "neighbor.remove ")))
		if err != nil || len(key) == 0 || len(key) > 32 {
			reply = "ERR: bad pubkey"
			break
		}
		kept := s.neighbours[:0]
		for _, n := range s.neighbours {
			if !strings.HasPrefix(hex.EncodeToString(n.Key[:]), hex.EncodeToString(key)) {
				kept = append(kept, n)
			}
		}
		s.neighbours, reply = kept, "OK"
	case !s.room && strings.HasPrefix(command, "discover.neighbors"):
		if strings.TrimLeft(command[len("discover.neighbors"):], " ") != "" {
			reply = "Err - discover.neighbors has no options"
		} else {
			packet, err := s.startDiscovery()
			if err != nil {
				return "", nil, err
			}
			packets, reply = one(packet), "OK - Discover sent"
		}
	case command == "board":
		reply = "Host; external KISS modem"
	case command == "clock":
		reply = time.Unix(int64(s.now()), 0).UTC().Format("15:04 - 2/1/2006 UTC")
	case command == "clock sync" || strings.HasPrefix(command, "time "):
		requested := uint64(timestamp) + 1
		if strings.HasPrefix(command, "time ") {
			var err error
			requested, err = strconv.ParseUint(strings.TrimSpace(strings.TrimPrefix(command, "time ")), 10, 32)
			if err != nil {
				reply = "Error, invalid time"
				break
			}
		}
		if requested <= uint64(s.now()) || requested > 0xffffffff {
			reply = "ERR: clock cannot go backwards"
		} else {
			s.state.RTCOffset = int64(requested) - time.Now().Unix()
			reply = "OK - clock set: " + time.Unix(int64(requested), 0).UTC().Format("15:04 - 2/1/2006 UTC")
		}
	case command == "get stats":
		reply = fmt.Sprintf("recv=%d sent=%d uptime=%ds", s.received.Load(), s.sent.Load(), int(time.Since(s.started).Seconds()))
	case command == "advert" || command == "advert.zerohop":
		p, err := s.advertisement(command == "advert")
		if err != nil {
			return "", nil, err
		}
		packets = delayed(p, 1500*time.Millisecond)
		reply = "OK - Advert sent"
		if command == "advert.zerohop" {
			reply = "OK - zerohop advert sent"
		}
	case command == "tempradio" || strings.HasPrefix(command, "tempradio "):
		reply = sharedPHYOwnerRequired
	case strings.HasPrefix(command, "setperm "):
		reply = s.setPermission(strings.Fields(command))
	case s.room && (command == "room.post" || strings.HasPrefix(command, "room.post ")):
		text := strings.TrimSpace(strings.TrimPrefix(command, "room.post"))
		if text == "" {
			reply = "ERR empty message"
		} else {
			s.addPost(s.id.PublicKey(), text)
			reply = "OK"
		}
	default:
		reply = s.preferenceCommand(command)
	}
	return prefix + reply, packets, nil
}

func (s *Service) commandHelp(topic string) string {
	switch topic {
	case "get":
		return "get name|owner.info|radio|freq|tx|lat|lon|rxdelay|txdelay|direct.txdelay|repeat|path.hash.mode|advert.interval|flood.advert.interval"
	case "set":
		return "set SETTING VALUE; role settings: name,owner.info,lat,lon,delays,repeat,path/adverts; shared RF changes require the modem administrator"
	case "wifi":
		return "Error: WiFi is managed by the external modem, not this host role; use the modem's Management console"
	case "radio":
		return "get radio|freq|tx; shared PHY setters require the modem administrator"
	case "owner":
		return "get owner.info; set owner.info TEXT (| separates lines); this role's owner text only"
	case "region":
		return "region; region def NAME ...; region put NAME [PARENT]; region home NAME; region default NAME; region allowf|denyf NAME; region save"
	case "stats":
		return "stats [role|sensors|radio|signal|airtime]; key=value schema=1; units in keys; role counters reset on role restart"
	default:
		return "Error: unknown help topic; use help"
	}
}

func (s *Service) setPermission(parts []string) string {
	if len(parts) != 3 {
		return "Err - bad params"
	}
	key, err := hex.DecodeString(parts[1])
	if err != nil || len(key) == 0 || len(key) > 32 {
		return "Err - bad pubkey"
	}
	value, err := strconv.ParseUint(parts[2], 10, 8)
	if err != nil {
		return "Err - bad permissions"
	}
	if byte(value)&3 == 0 {
		matches := []string{}
		for k := range s.state.Members {
			if strings.HasPrefix(k, strings.ToLower(parts[1])) {
				matches = append(matches, k)
			}
		}
		if len(matches) != 1 {
			return "Err - ambiguous or unknown pubkey"
		}
		delete(s.state.Members, matches[0])
		s.state.MemberOrder = s.memberKeys()
		return "OK"
	}
	if len(key) != 32 {
		return "Err - full pubkey required"
	}
	id, err := meshcore.NewIdentityFromBytes(key)
	if err != nil {
		return "Err - bad pubkey"
	}
	if _, err := s.Node.SharedSecret(id); err != nil {
		return "Err - bad pubkey"
	}
	m := s.state.Members[id.String()]
	if m == nil {
		m, err = s.putMember(id)
		if err != nil {
			return "Err - " + err.Error()
		}
	}
	m.Permissions = byte(value)
	return "OK"
}
