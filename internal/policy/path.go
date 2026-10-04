package policy

import "errors"

// Path stores an ordinary encoded path. Its zero value is unknown, not direct.
// Private storage prevents callers from invalidating a successfully parsed path.
type Path struct {
	known   bool
	encoded uint8
	data    [64]byte
}

func UnknownPath() Path { return Path{} }

func ParsePath(encoded uint8, data []byte) (Path, error) {
	if encoded == 255 && len(data) == 0 {
		return UnknownPath(), nil
	}
	width, count := (encoded>>6)+1, encoded&63
	if width > 3 || int(width)*int(count) > 64 || len(data) != int(width)*int(count) {
		return Path{}, errors.New("invalid ordinary encoded path")
	}
	p := Path{known: true, encoded: encoded}
	copy(p.data[:], data)
	return p, nil
}

func NewPath(width uint8, data []byte) (Path, error) {
	if width < 1 || width > 3 || len(data)%int(width) != 0 || len(data)/int(width) > 63 || len(data) > 64 {
		return Path{}, errors.New("invalid ordinary path width or length")
	}
	return ParsePath((width-1)<<6|uint8(len(data)/int(width)), data)
}

func (p Path) Known() bool { return p.known }
func (p Path) Encoded() uint8 {
	if !p.known {
		return 255
	}
	return p.encoded
}
func (p Path) Width() uint8 {
	if !p.known {
		return 0
	}
	return (p.encoded >> 6) + 1
}
func (p Path) Count() uint8 {
	if !p.known {
		return 0
	}
	return p.encoded & 63
}
func (p Path) Bytes() []byte {
	if !p.known {
		return nil
	}
	return append([]byte{}, p.data[:int(p.Width())*int(p.Count())]...)
}

func (p Path) Append(hash []byte) (Path, error) {
	if !p.known || len(hash) != int(p.Width()) {
		return Path{}, errors.New("hash does not match a known path width")
	}
	return NewPath(p.Width(), append(p.Bytes(), hash...))
}

// TracePath is a different wire encoding: flags select widths 1, 2, 4 or 8.
// Hashes belong in the TRACE payload, not the ordinary packet path field.
type TracePath struct {
	Flags  uint8
	Hashes []byte
}

// NewTracePath takes a TRACE hash width, not an ordinary 1/2/3-byte path
// width. There is no native three-byte TRACE encoding.
func NewTracePath(width uint8, hashes []byte) (TracePath, error) {
	var flags uint8
	switch width {
	case 1:
		flags = 0
	case 2:
		flags = 1
	case 4:
		flags = 2
	case 8:
		flags = 3
	default:
		return TracePath{}, errors.New("TRACE hash width must be 1, 2, 4 or 8 bytes")
	}
	return ParseTracePath(flags, hashes)
}

func ParseTracePath(flags uint8, hashes []byte) (TracePath, error) {
	width := 1 << (flags & 3)
	// Native SEND_TRACE_PATH: len > 10 && len-10 < MAX_PACKET_PAYLOAD-5.
	if len(hashes) == 0 || len(hashes) >= 179 || len(hashes)%width != 0 || len(hashes)/width > 64 {
		return TracePath{}, errors.New("invalid trace hash path")
	}
	return TracePath{Flags: flags, Hashes: append([]byte(nil), hashes...)}, nil
}

func (p TracePath) Width() uint8 { return 1 << (p.Flags & 3) }
func (p TracePath) Count() int   { return len(p.Hashes) / int(p.Width()) }
