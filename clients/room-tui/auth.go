package main

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"strings"
	"time"
)

const accountDeviceProtocol = "aspen-account.device.v1"

type deviceLink struct {
	Code      string `json:"code"`
	Claim     string `json:"claim"`
	PublicKey string `json:"publicKey"`
	Expires   int64  `json:"expires"`
	URL       string `json:"url"`
}

type accountSession struct {
	Username  string `json:"username"`
	PublicKey string `json:"publicKey"`
	Name      string `json:"name"`
	Expires   int64  `json:"expires"`
}

func (c *roomClient) accountProof(ctx context.Context, saved *savedState, purpose string) (map[string]string, error) {
	var challenge struct {
		ID        string `json:"id"`
		PublicKey string `json:"publicKey"`
		Origin    string `json:"origin"`
		Purpose   string `json:"purpose"`
		Message   string `json:"message"`
		Protocol  string `json:"protocol"`
		Expires   int64  `json:"expires"`
	}
	if err := c.request(ctx, c.origin+"/v1/auth/device-challenge",
		map[string]string{"publicKey": saved.PublicKey, "purpose": purpose}, &challenge); err != nil {
		return nil, err
	}
	text := strings.Join([]string{accountDeviceProtocol, c.origin, purpose, saved.PublicKey, challenge.ID}, "\n")
	if challenge.Protocol != accountDeviceProtocol || !publicKeyPattern.MatchString(challenge.ID) ||
		challenge.PublicKey != saved.PublicKey || challenge.Origin != c.origin || challenge.Purpose != purpose ||
		challenge.Expires <= time.Now().Unix() || challenge.Expires > time.Now().Unix()+125 ||
		challenge.Message != text || len(text) > 512 {
		return nil, errors.New("account challenge is expired or belongs to a different service, device or action")
	}
	key := saved.privateKey()
	signature := ed25519.Sign(key, []byte(text))
	clear(key)
	return map[string]string{"id": challenge.ID, "signature": hex.EncodeToString(signature)}, nil
}

func validateAccount(s accountSession, publicKey string) error {
	if s.PublicKey != publicKey || !usernamePattern.MatchString(s.Username) ||
		!validName(s.Name) || s.Expires <= time.Now().Unix() {
		return errors.New("account sign-in returned a different device key or invalid session")
	}
	return nil
}

func (c *roomClient) deviceLogin(ctx context.Context, saved *savedState) error {
	proof, err := c.accountProof(ctx, saved, "device-login")
	if err != nil {
		return err
	}
	var session accountSession
	if err := c.request(ctx, c.origin+"/v1/auth/device-login", proof, &session); err != nil {
		return err
	}
	return validateAccount(session, saved.PublicKey)
}

func (c *roomClient) beginLink(ctx context.Context, saved *savedState) (deviceLink, error) {
	var link deviceLink
	proof, err := c.accountProof(ctx, saved, "link")
	if err != nil {
		return link, err
	}
	proof["label"] = "Aspen Linux TUI"
	if err := c.request(ctx, c.origin+"/v1/auth/link/start", proof, &link); err != nil {
		return link, err
	}
	if len(link.Code) != 24 || strings.Trim(link.Code, "0123456789ABCDEF") != "" ||
		!publicKeyPattern.MatchString(link.Claim) || link.PublicKey != saved.PublicKey ||
		link.Expires <= time.Now().Unix() || link.Expires > time.Now().Unix()+305 ||
		link.URL != c.origin+"/#link="+link.Code {
		return link, errors.New("device approval request has an invalid code, key, URL or five-minute expiry")
	}
	return link, nil
}

func (c *roomClient) pollLink(ctx context.Context, link deviceLink) (bool, error) {
	if link.Expires <= time.Now().Unix() {
		return false, errors.New("device link expired; press Enter to create a new five-minute request")
	}
	var status struct {
		Approved  bool   `json:"approved"`
		PublicKey string `json:"publicKey"`
		Expires   int64  `json:"expires"`
	}
	body := map[string]string{"code": link.Code, "claim": link.Claim}
	if err := c.request(ctx, c.origin+"/v1/auth/link/status", body, &status); err != nil {
		return false, err
	}
	if status.PublicKey != link.PublicKey || status.Expires != link.Expires {
		return false, errors.New("device approval status changed its requesting key or expiry")
	}
	if !status.Approved {
		return false, nil
	}
	var session accountSession
	if err := c.request(ctx, c.origin+"/v1/auth/link/claim", body, &session); err != nil {
		return false, err
	}
	if err := validateAccount(session, link.PublicKey); err != nil {
		return false, err
	}
	return true, nil
}

func (c *roomClient) roomTicket(ctx context.Context, alias, publicKey string) (map[string]string, error) {
	var result struct {
		Ticket    string `json:"ticket"`
		PublicKey string `json:"publicKey"`
		Alias     string `json:"alias"`
		Expires   int64  `json:"expires"`
	}
	if err := c.request(ctx, c.origin+"/v1/auth/room-ticket",
		map[string]string{"alias": alias, "publicKey": publicKey}, &result); err != nil {
		return nil, err
	}
	if !publicKeyPattern.MatchString(result.Ticket) || result.PublicKey != publicKey ||
		result.Alias != alias || result.Expires <= time.Now().Unix() || result.Expires > time.Now().Unix()+65 {
		return nil, errors.New("account room ticket belongs to a different device or room, or is expired")
	}
	return map[string]string{"ticket": result.Ticket}, nil
}

func (c *roomClient) ticketLogin(ctx context.Context, alias, publicKey string, body map[string]string) (session, error) {
	var result session
	if err := c.request(ctx, c.endpoint(alias, "login"), body, &result); err != nil {
		return result, err
	}
	if err := validateSession(result); err != nil {
		return result, err
	}
	if result.Author != publicKey || !usernamePattern.MatchString(result.Username) {
		return result, errors.New("room session does not match this device key and account")
	}
	return result, nil
}
