package roles

import (
	"errors"
	"fmt"
	"strings"

	"meshcore.local/meshcore/internal/policy"
)

type regionLoad struct {
	owner  string
	prefs  policy.Preferences
	stack  [8]uint16
	nextID uint32
}

func managedDefaultScope(p policy.Preferences, id uint16) (policy.Scope, error) {
	for _, r := range p.Regions {
		if r.ID != id {
			continue
		}
		if strings.HasPrefix(r.Name, "$") {
			if len(r.Keys) == 0 || r.Keys[0] == [16]byte{} {
				return policy.Scope{}, errors.New("private default region has no usable transport key")
			}
			return policy.Scope{Name: r.Name, Key: r.Keys[0]}, nil
		}
		return policy.AutoScope(r.Name), nil
	}
	return policy.Scope{}, nil
}

func (s *Service) regionLoadLine(line string) string {
	load := s.regionLoad
	if strings.TrimSpace(line) == "" {
		if err := policy.Validate(s.profile, load.prefs); err != nil {
			return "Err - " + err.Error()
		}
		s.state.Preferences, s.state.HomeRegion = load.prefs, 0
		s.state.NextRegionID = load.nextID
		s.state.DefaultRegion, s.state.ManagedDefaultRegion = 0, true
		s.regionLoad = nil
		return fmt.Sprintf("OK - loaded %d regions", len(load.prefs.Regions))
	}
	trimmed := strings.TrimLeft(line, " ")
	indent := len(line) - len(trimmed)
	if indent == 0 {
		return ""
	}
	if indent >= 8 {
		return "Err - region nesting exceeds 7 levels"
	}
	parts := strings.Fields(trimmed)
	if len(parts) == 0 {
		return ""
	}
	name := strings.TrimSuffix(parts[0], "^")
	r := policy.Region{Name: name, Parent: load.stack[indent-1], Flags: policy.RegionDenyFlood}
	if indent > 1 && r.Parent == 0 {
		return "Err - missing region parent"
	}
	if len(parts) > 1 && parts[1] == "F" {
		r.Flags = 0
	}
	for _, old := range s.state.Preferences.Regions {
		if strings.TrimPrefix(old.Name, "#") == strings.TrimPrefix(name, "#") {
			r.ID, r.Flags, r.Keys = old.ID, old.Flags, old.Keys
		}
	}
	if r.ID == 0 {
		if load.nextID > 65535 {
			return "Err - region identifiers exhausted"
		}
		r.ID = uint16(load.nextID)
	}
	next := append(append([]policy.Region(nil), load.prefs.Regions...), r)
	prefs, err := policy.ApplyOverrides(s.profile, load.prefs, policy.Overrides{Regions: &next})
	if err != nil {
		return "Err - " + err.Error()
	}
	load.prefs = prefs
	if uint32(r.ID) == load.nextID {
		load.nextID++
	}
	load.stack[indent] = r.ID
	for i := indent + 1; i < len(load.stack); i++ {
		load.stack[i] = 0
	}
	return ""
}

func regionIndex(p policy.Preferences, name string) int {
	if name == "*" {
		return -1
	}
	name = strings.TrimPrefix(name, "#")
	index := -2
	for i, r := range p.Regions {
		candidate := strings.TrimPrefix(r.Name, "#")
		if candidate == name {
			return i
		}
		if strings.HasPrefix(candidate, name) {
			index = i
		}
	}
	return index
}

func regionNames(p policy.Preferences, denied bool, limit int) string {
	var names []string
	if (p.WildcardFlags&policy.RegionDenyFlood != 0) == denied {
		names = append(names, "*")
	}
	length := len(strings.Join(names, ","))
	for _, r := range p.Regions {
		name := strings.TrimPrefix(r.Name, "#")
		if (r.Flags&policy.RegionDenyFlood != 0) == denied && length+len(name)+2 < limit {
			names = append(names, name)
			length += len(name) + 1
		}
	}
	return strings.Join(names, ",")
}

func regionTree(p policy.Preferences, home uint16) string {
	var output strings.Builder
	var write func(uint16, string, uint8, int)
	write = func(id uint16, name string, flags uint8, depth int) {
		output.WriteString(strings.Repeat(" ", depth) + strings.TrimPrefix(name, "#"))
		if id == home {
			output.WriteByte('^')
		}
		if flags&policy.RegionDenyFlood == 0 {
			output.WriteString(" F")
		}
		output.WriteByte('\n')
		for _, r := range p.Regions {
			if r.Parent == id {
				write(r.ID, r.Name, r.Flags, depth+1)
			}
		}
	}
	write(0, "*", p.WildcardFlags, 0)
	value := output.String()
	if len(value) > 159 {
		value = value[:159]
	}
	return value
}

func (s *Service) regionCommand(command string) string {
	p, err := policy.ApplyOverrides(s.profile, s.state.Preferences, policy.Overrides{})
	if err != nil {
		return "Err - " + err.Error()
	}
	parts := strings.Fields(command)
	home := s.state.HomeRegion
	nextID := s.state.NextRegionID
	defaultID, managedDefault := s.state.DefaultRegion, s.state.ManagedDefaultRegion
	if len(parts) == 1 {
		return regionTree(p, home)
	}
	lookup := func(name string) (int, uint16) {
		i := regionIndex(p, name)
		if i >= 0 {
			return i, p.Regions[i].ID
		}
		return i, 0
	}
	put := func(name string, parent uint16) int {
		if name == "*" {
			return -2
		}
		i := regionIndex(p, name)
		if i >= 0 && strings.TrimPrefix(name, "#") != strings.TrimPrefix(p.Regions[i].Name, "#") {
			i = -2
		}
		if i >= -1 {
			if i >= 0 {
				p.Regions[i].Parent = parent
				p.Regions[i].Flags = 0
			}
			return i
		}
		if len(p.Regions) >= 32 {
			return -2
		}
		if nextID > 65535 {
			return -2
		}
		id := uint16(nextID)
		nextID++
		p.Regions = append(p.Regions, policy.Region{ID: id, Parent: parent, Name: name})
		return len(p.Regions) - 1
	}
	reply := "OK"
	switch parts[1] {
	case "load":
		p.Regions = nil
		p.WildcardFlags = 0
		s.regionLoad = &regionLoad{owner: s.commandOwner, prefs: p, nextID: nextID}
		return ""
	case "save":
		s.state.DiscoveryModified = s.now()
	case "list":
		if len(parts) != 3 || (parts[2] != "allowed" && parts[2] != "denied") {
			return "Err - use 'allowed' or 'denied'"
		}
		reply = regionNames(p, parts[2] == "denied", 160)
		if reply == "" {
			reply = "-none-"
		}
	case "default":
		if len(parts) == 2 {
			name := p.DefaultScope.Name
			if managedDefault {
				name = ""
				for _, r := range p.Regions {
					if r.ID == defaultID {
						name = r.Name
					}
				}
			}
			if name == "" {
				name = "<null>"
			}
			return " default scope is " + name
		}
		selected := parts[2]
		if parts[2] == "<null>" {
			p.DefaultScope = policy.Scope{}
			defaultID = 0
		} else {
			i, _ := lookup(parts[2])
			if i < -1 {
				i = put(parts[2], 0)
			}
			if i < -1 {
				return "Err - region table full"
			}
			if i == -1 {
				p.DefaultScope = policy.AutoScope("*")
				defaultID = 0
				p.WildcardFlags = 0
			} else {
				r := &p.Regions[i]
				r.Flags = 0
				selected = r.Name
				defaultID = r.ID
				p.DefaultScope, err = managedDefaultScope(p, r.ID)
				if err != nil {
					return "Err - " + err.Error()
				}
			}
		}
		managedDefault = true
		reply = " default scope is now " + selected
	case "home":
		if len(parts) == 2 {
			name := "*"
			for _, r := range p.Regions {
				if r.ID == home {
					name = r.Name
				}
			}
			return " home is " + name
		}
		i, id := lookup(parts[2])
		if i < -1 {
			return "Err - unknown region"
		}
		home = id
		selected := "*"
		if i >= 0 {
			selected = p.Regions[i].Name
		}
		reply = " home is now " + selected
	case "put":
		if len(parts) < 3 {
			return "Err - ??"
		}
		parent := uint16(0)
		if len(parts) > 3 {
			i, id := lookup(parts[3])
			if i < -1 {
				return "Err - unknown parent"
			}
			parent = id
		}
		if put(parts[2], parent) < -1 {
			return "Err - unable to put"
		}
		reply = "OK - (flood allowed)"
	case "allowf", "denyf", "get", "remove":
		if len(parts) < 3 {
			return "Err - ??"
		}
		i, id := lookup(parts[2])
		if i < -1 {
			return "Err - unknown region"
		}
		flags := &p.WildcardFlags
		name := "*"
		parent := uint16(0)
		if i >= 0 {
			flags = &p.Regions[i].Flags
			name = p.Regions[i].Name
			parent = p.Regions[i].Parent
		}
		switch parts[1] {
		case "allowf":
			*flags &^= policy.RegionDenyFlood
		case "denyf":
			*flags |= policy.RegionDenyFlood
		case "get":
			flag := ""
			if *flags&policy.RegionDenyFlood == 0 {
				flag = "F"
			}
			for _, r := range p.Regions {
				if r.ID == parent {
					return fmt.Sprintf(" %s (%s) %s", name, r.Name, flag)
				}
			}
			return fmt.Sprintf(" %s %s", name, flag)
		case "remove":
			if i < 0 {
				return "Err - not empty"
			}
			if strings.TrimPrefix(parts[2], "#") != strings.TrimPrefix(name, "#") {
				return "Err - not found"
			}
			for _, r := range p.Regions {
				if r.Parent == id {
					return "Err - not empty"
				}
			}
			p.Regions = append(p.Regions[:i], p.Regions[i+1:]...)
		}
	case "def":
		if len(parts) < 3 {
			return "Err - empty def"
		}
		cursor := uint16(0)
		for _, segment := range parts[2:] {
			name, jump, hasJump := strings.Cut(segment, "|")
			if comma := strings.IndexByte(name, ','); comma >= 0 {
				name, jump, hasJump = name[:comma], segment[comma+1:], true
			}
			if name == "" {
				return "Err - empty name"
			}
			if hasJump && jump == "" {
				return "Err - empty jump"
			}
			i := put(name, cursor)
			if i < -1 {
				return "Err - put failed: " + name
			}
			cursor = 0
			if i >= 0 {
				cursor = p.Regions[i].ID
			}
			if hasJump {
				j, id := lookup(jump)
				if j < -1 {
					return "Err - unknown jump: " + jump
				}
				cursor = id
			}
		}
		reply = regionTree(p, home)
	default:
		return "Err - unsupported region operation"
	}
	if err := policy.Validate(s.profile, p); err != nil {
		return "Err - " + err.Error()
	}
	s.state.Preferences, s.state.HomeRegion = p, home
	s.state.NextRegionID = nextID
	s.state.DefaultRegion, s.state.ManagedDefaultRegion = defaultID, managedDefault
	return reply
}
