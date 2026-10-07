package sharedroom

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

type event struct {
	Type      string `json:"type"`
	ID        uint64 `json:"id,string,omitempty"`
	Result    Result `json:"result"`
	Error     string `json:"error"`
	PublicKey string `json:"publicKey"`
	Name      string `json:"name"`
	Delivery
}
type reply struct {
	result Result
	err    error
}

// Client has one inbound hibernating connection to one advertised identity.
// Lost calls are failed as uncertain, never automatically resubmitted.
type Client struct {
	ctx      context.Context
	identity Identity
	endpoint string
	token    string
	delivery func(Delivery)
	report   func(error)
	mu       sync.Mutex
	conn     *websocket.Conn
	pending  map[uint64]chan reply
	nextID   uint64
	write    sync.Mutex
	done     chan struct{}
}

func newClient(ctx context.Context, base, token string, identity Identity, delivery func(Delivery), report func(error)) (*Client, error) {
	u, err := url.Parse(base)
	if err != nil {
		return nil, errors.New("invalid shared room API URL")
	}
	if u.Scheme == "https" {
		u.Scheme = "wss"
	} else if u.Scheme == "http" {
		u.Scheme = "ws"
	}
	u.Path = "/v1/aliases/" + identity.Alias + "/socket"
	c := &Client{ctx: ctx, identity: identity, endpoint: u.String(), token: token, delivery: delivery, report: report,
		pending: make(map[uint64]chan reply), done: make(chan struct{})}
	conn, err := c.dial()
	if err != nil {
		return nil, err
	}
	c.conn = conn
	go c.run(conn)
	return c, nil
}
func (c *Client) dial() (*websocket.Conn, error) {
	ctx, cancel := context.WithTimeout(c.ctx, 15*time.Second)
	defer cancel()
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = 15 * time.Second
	dialer.Subprotocols = []string{"aspen-room.v1.json"}
	conn, response, err := dialer.DialContext(ctx, c.endpoint, http.Header{"Authorization": []string{"Bearer " + c.token}})
	if response != nil && response.Body != nil {
		response.Body.Close()
	}
	if err != nil {
		return nil, fmt.Errorf("%s API connection failed: %w", c.identity.Alias, err)
	}
	conn.SetReadLimit(4096)
	conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	var ready event
	if err := conn.ReadJSON(&ready); err != nil {
		conn.Close()
		return nil, fmt.Errorf("%s API ready message failed: %w", c.identity.Alias, err)
	}
	if ready.Type != "ready" || ready.Alias != c.identity.Alias || ready.PublicKey != c.identity.Key.String() || ready.Name != c.identity.Name {
		conn.Close()
		return nil, fmt.Errorf("%s advertised identity differs from server configuration", c.identity.Alias)
	}
	conn.SetReadDeadline(time.Time{}) // No application ping/timer keeps the DO awake.
	return conn, nil
}
func (c *Client) run(conn *websocket.Conn) {
	defer close(c.done)
	backoff := time.Second
	for {
		err := c.read(conn)
		c.disconnect(conn)
		if c.ctx.Err() != nil {
			return
		}
		c.report(fmt.Errorf("%s API disconnected: %w", c.identity.Alias, err))
		for {
			timer := time.NewTimer(backoff)
			select {
			case <-c.ctx.Done():
				timer.Stop()
				return
			case <-timer.C:
			}
			next, err := c.dial()
			if err != nil {
				c.report(err)
				backoff = min(backoff*2, 30*time.Second)
				continue
			}
			c.mu.Lock()
			c.conn = next
			c.mu.Unlock()
			conn, backoff = next, time.Second
			break
		}
	}
}
func (c *Client) read(conn *websocket.Conn) error {
	for {
		var e event
		if err := conn.ReadJSON(&e); err != nil {
			return err
		}
		switch e.Type {
		case "result", "error":
			c.mu.Lock()
			waiter := c.pending[e.ID]
			delete(c.pending, e.ID)
			c.mu.Unlock()
			if waiter != nil {
				r := reply{result: e.Result}
				if e.Type == "error" {
					r.err = errors.New(e.Error)
				}
				waiter <- r
			}
		case "delivery":
			if e.Alias != c.identity.Alias {
				return errors.New("API delivery alias mismatch")
			}
			c.delivery(e.Delivery)
		}
	}
}
func (c *Client) disconnect(conn *websocket.Conn) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.conn != conn {
		return
	}
	c.conn = nil
	conn.Close()
	for id, waiter := range c.pending {
		delete(c.pending, id)
		waiter <- reply{err: errors.New("API connection lost; operation outcome may be unknown")}
	}
}
func (c *Client) Call(ctx context.Context, operation Operation) (Result, error) {
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	c.mu.Lock()
	conn := c.conn
	if conn == nil {
		c.mu.Unlock()
		return Result{}, errors.New("shared room API disconnected")
	}
	c.nextID++
	id := c.nextID
	waiter := make(chan reply, 1)
	c.pending[id] = waiter
	c.mu.Unlock()
	defer func() { c.mu.Lock(); delete(c.pending, id); c.mu.Unlock() }()
	frame := struct {
		ID        uint64    `json:"id,string"`
		Operation Operation `json:"operation"`
	}{id, operation}
	c.write.Lock()
	conn.SetWriteDeadline(time.Now().Add(15 * time.Second))
	encoded, err := json.Marshal(frame)
	if err == nil && len(encoded) > 4096 {
		err = errors.New("API operation exceeds 4096 bytes")
	}
	if err == nil {
		err = conn.WriteMessage(websocket.TextMessage, encoded)
	}
	c.write.Unlock()
	if err != nil {
		c.disconnect(conn)
		return Result{}, errors.New("API write failed; operation outcome may be unknown")
	}
	select {
	case response := <-waiter:
		return response.result, response.err
	case <-ctx.Done():
		return Result{}, fmt.Errorf("API response incomplete; operation outcome may be unknown: %w", ctx.Err())
	case <-c.ctx.Done():
		return Result{}, c.ctx.Err()
	}
}
func (c *Client) Members(ctx context.Context, prefix string) ([]Member, error) {
	result, err := c.Call(ctx, Operation{Op: "members", Prefix: prefix})
	if err == nil {
		return result.Members, nil
	}
	if err.Error() != "Use a longer member key prefix" || len(prefix) == 64 {
		return nil, err
	}
	var members []Member
	for _, nibble := range "0123456789abcdef" {
		part, err := c.Members(ctx, prefix+string(nibble))
		if err != nil {
			return nil, err
		}
		members = append(members, part...)
	}
	return members, nil
}
func (c *Client) Connected() bool { c.mu.Lock(); defer c.mu.Unlock(); return c.conn != nil }
func (c *Client) Close() {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()
	if conn != nil {
		conn.Close()
	}
	<-c.done
}
