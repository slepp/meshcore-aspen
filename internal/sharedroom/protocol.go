// Package sharedroom bridges native MeshCore room packets to the authoritative
// shared-room Worker. It owns no local message history or membership database.
package sharedroom

import meshcore "github.com/meshcore-go/meshcore-go"

type Identity struct {
	Alias        string
	Name         string
	Key          meshcore.LocalIdentity
	PathHashMode byte
}
type Message struct {
	Seq             uint64 `json:"seq"`
	Timestamp       uint32 `json:"timestamp"`
	OriginAlias     string `json:"originAlias"`
	Author          string `json:"author"`
	ClientTimestamp uint32 `json:"clientTimestamp"`
	Text            string `json:"text"`
}
type Pending struct {
	DeliveryID string  `json:"deliveryId"`
	Proof      *string `json:"proof"`
	State      string  `json:"state"`
}
type Member struct {
	Client  string   `json:"client"`
	Cursor  uint64   `json:"cursor"`
	Route   string   `json:"route,omitempty"`
	Pending *Pending `json:"pending,omitempty"`
}
type Operation struct {
	Op         string  `json:"op"`
	Client     string  `json:"client,omitempty"`
	Timestamp  uint32  `json:"timestamp,omitempty"`
	Since      *uint32 `json:"since,omitempty"`
	Password   string  `json:"password"`
	Attempt    string  `json:"attempt,omitempty"`
	Route      string  `json:"route"`
	Text       string  `json:"text,omitempty"`
	Source     string  `json:"source,omitempty"`
	DeliveryID string  `json:"deliveryId,omitempty"`
	Proof      string  `json:"proof,omitempty"`
	Outcome    string  `json:"outcome,omitempty"`
	Prefix     string  `json:"prefix,omitempty"`
}
type Result struct {
	Respond   bool     `json:"respond"`
	Transmit  bool     `json:"transmit"`
	Cursor    uint64   `json:"cursor"`
	Remaining byte     `json:"remaining"`
	Message   *Message `json:"message"`
	Members   []Member `json:"members"`
}
type Delivery struct {
	Alias      string  `json:"alias"`
	Client     string  `json:"client"`
	DeliveryID string  `json:"deliveryId"`
	Route      string  `json:"route"`
	Message    Message `json:"message"`
}
