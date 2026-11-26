package main

// Unit tests for users.go and users_http.go: no sockets, no real time. The
// handlers run in process through httptest's recorder, with a fixed clock.

import (
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"net/http"
	"net/http/httptest"
	"slices"
	"strings"
	"testing"

	"golang.org/x/crypto/nacl/box"
)

const testNow = 1_800_000_000

func testUsers() (*mockProvider, *users) {
	mock := &mockProvider{}
	copy(mock.key[:], bytes.Repeat([]byte{0x31}, secretKeyBytes))
	var hashKey [secretKeyBytes]byte
	copy(hashKey[:], bytes.Repeat([]byte{0x47}, secretKeyBytes))
	return mock, newUsers(hashKey)
}

func mustHash(t *testing.T, users *users, provider string, userID string) uint64 {
	t.Helper()
	userHash, err := users.hash(provider, userID)
	if err != nil {
		t.Fatal(err)
	}
	return userHash
}

func mustTicket(t *testing.T, mock *mockProvider, userID string, now uint64) string {
	t.Helper()
	ticket, err := mock.issue(userID, now, mockTicketSeconds)
	if err != nil {
		t.Fatal(err)
	}
	return ticket
}

func TestMockTicket(t *testing.T) {
	mock, _ := testUsers()
	ticket := mustTicket(t, mock, "alice", testNow)
	if userID, err := mock.verify(ticket, testNow); err != nil || userID != "alice" {
		t.Fatalf("a fresh ticket did not verify: %q %v", userID, err)
	}
	if userID, err := mock.verify(ticket, testNow+mockTicketSeconds-1); err != nil || userID != "alice" {
		t.Fatal("a ticket did not verify in its last second")
	}

	// expired
	if _, err := mock.verify(ticket, testNow+mockTicketSeconds); err == nil {
		t.Fatal("an expired ticket verified")
	}

	// edited: every byte of it, one bit at a time
	raw, _ := base64.StdEncoding.DecodeString(ticket)
	for i := range raw {
		edited := bytes.Clone(raw)
		edited[i] ^= 0x01
		if _, err := mock.verify(base64.StdEncoding.EncodeToString(edited), testNow); err == nil {
			t.Fatalf("a ticket with byte %d edited verified", i)
		}
	}

	// another user's name over alice's MAC
	bob := bytes.Clone(raw)
	copy(bob[ticketHeaderBytes:], "bobby")
	if _, err := mock.verify(base64.StdEncoding.EncodeToString(bob), testNow); err == nil {
		t.Fatal("a ticket with its user id replaced verified")
	}

	// forged: the right shape under another key
	forger := &mockProvider{}
	copy(forger.key[:], bytes.Repeat([]byte{0x32}, secretKeyBytes))
	if _, err := mock.verify(mustTicket(t, forger, "alice", testNow), testNow); err == nil {
		t.Fatal("a ticket signed with another key verified")
	}

	for _, text := range []string{"", "@@@@", base64.StdEncoding.EncodeToString(raw[:len(raw)-1]), strings.Repeat("A", maxTicketTextBytes+4)} {
		if _, err := mock.verify(text, testNow); err == nil {
			t.Fatalf("ticket %q verified", text)
		}
	}
	if _, err := mock.issue("", testNow, 60); err == nil {
		t.Fatal("an empty user id got a ticket")
	}
	if _, err := mock.issue(strings.Repeat("x", maxUserIDBytes+1), testNow, 60); err == nil {
		t.Fatal("a long user id got a ticket")
	}
}

func TestUserHash(t *testing.T) {
	_, users := testUsers()
	alice := mustHash(t, users, "mock", "alice")
	if mustHash(t, users, "mock", "alice") != alice {
		t.Fatal("the same user hashed two ways")
	}
	if mustHash(t, users, "other", "alice") == alice {
		t.Fatal("the same user id on another provider has the same hash")
	}
	if mustHash(t, users, "mock", "bob") == alice {
		t.Fatal("another user has the same hash")
	}
	var otherKey [secretKeyBytes]byte
	if mustHash(t, newUsers(otherKey), "mock", "alice") == alice {
		t.Fatal("another backend secret gives the same hash")
	}
	if _, err := users.hash("mo:ck", "alice"); err == nil {
		t.Fatal("a provider name with ':' was accepted")
	}
	if _, err := users.hash("", "alice"); err == nil {
		t.Fatal("an empty provider name was accepted")
	}
}

func TestWhitelist(t *testing.T) {
	_, users := testUsers()
	alice := mustHash(t, users, "mock", "alice")
	bob := mustHash(t, users, "mock", "bob")
	carol := mustHash(t, users, "mock", "carol")

	if len(users.whitelist("one")) != 0 {
		t.Fatal("a server nobody matched to has a whitelist")
	}
	for _, id := range []uint64{alice, bob} {
		if !users.seat(id, "one") {
			t.Fatal("an unbanned user was refused a seat")
		}
	}
	users.seat(carol, "two")
	if !slices.Equal(users.whitelist("one"), slices.Sorted(slices.Values([]uint64{alice, bob}))) {
		t.Fatalf("whitelist one is %x", users.whitelist("one"))
	}
	if !slices.Equal(users.whitelist("two"), []uint64{carol}) {
		t.Fatalf("whitelist two is %x", users.whitelist("two"))
	}

	// one seat per user: a new match moves the user
	users.seat(alice, "two")
	if !slices.Equal(users.whitelist("one"), []uint64{bob}) || !slices.Equal(users.whitelist("two"), slices.Sorted(slices.Values([]uint64{alice, carol}))) {
		t.Fatal("a user is on two servers' whitelists")
	}

	// a ban takes the user off the list and refuses a new seat
	users.setBanned(alice, true)
	if !slices.Equal(users.whitelist("two"), []uint64{carol}) {
		t.Fatal("a banned user is still on a whitelist")
	}
	if users.seat(alice, "one") || slices.Contains(users.whitelist("one"), alice) {
		t.Fatal("a banned user was seated")
	}
	users.setBanned(alice, false)
	if !users.seat(alice, "one") || !slices.Contains(users.whitelist("one"), alice) {
		t.Fatal("a lifted ban did not let the user back")
	}
}

func TestWhitelistWire(t *testing.T) {
	if len(writeWhitelist(nil)) != 0 {
		t.Fatal("an empty whitelist is not an empty body")
	}
	data := writeWhitelist([]uint64{1, 0x0102030405060708})
	want := []byte{1, 0, 0, 0, 0, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1}
	if !bytes.Equal(data, want) {
		t.Fatalf("whitelist wire %x", data)
	}
}

// the routes together, in process: tickets, match, bans and whitelist

type testBackend struct {
	mock     *mockProvider
	users    *users
	registry *serverRegistry
	router   *http.ServeMux
	now      uint64
}

func newTestBackend(t *testing.T, seal sealFunction) *testBackend {
	t.Helper()
	config, err := readServiceConfig(environment(map[string]string{}))
	if err != nil {
		t.Fatal(err)
	}
	backend := &testBackend{now: testNow}
	backend.mock, backend.users = testUsers()
	// always the last server id in order, so a test picks the server
	backend.registry = newServerRegistry(func(max int) (int, error) { return max - 1, nil })
	now := func() uint64 { return backend.now }
	backend.router = http.NewServeMux()
	backend.router.HandleFunc("PUT /servers/{serverID}", registerHandler(backend.registry))
	backend.router.HandleFunc("GET /servers/{serverID}/whitelist", whitelistHandler(backend.registry, backend.users))
	backend.router.HandleFunc("POST /mock/ticket/{userID}", mockTicketHandler(backend.mock, now))
	backend.router.HandleFunc("POST /match", matchTicketHandler(config, backend.registry, backend.users, verifyTicketWith(backend.mock), now, bytes.NewReader(bytes.Repeat([]byte{0x5a}, 1<<16)), seal))
	backend.router.HandleFunc("PUT /bans/{provider}/{userID}", banHandler(backend.users, true))
	backend.router.HandleFunc("DELETE /bans/{provider}/{userID}", banHandler(backend.users, false))
	return backend
}

func (backend *testBackend) do(method string, path string, body string) *httptest.ResponseRecorder {
	recorder := httptest.NewRecorder()
	backend.router.ServeHTTP(recorder, httptest.NewRequest(method, path, strings.NewReader(body)))
	return recorder
}

func (backend *testBackend) register(t *testing.T, serverID string) {
	t.Helper()
	body := `{"addresses":["127.0.0.1:40000"],"public_key":"` + base64.StdEncoding.EncodeToString(devServerPublicKey[:]) + `"}`
	if response := backend.do("PUT", "/servers/"+serverID, body); response.Code != http.StatusNoContent {
		t.Fatalf("register %s: %d %s", serverID, response.Code, response.Body)
	}
}

func (backend *testBackend) ticket(t *testing.T, userID string) string {
	t.Helper()
	response := backend.do("POST", "/mock/ticket/"+userID, "")
	if response.Code != http.StatusOK {
		t.Fatalf("ticket for %s: %d", userID, response.Code)
	}
	return response.Body.String()
}

func (backend *testBackend) whitelist(t *testing.T, serverID string) []uint64 {
	t.Helper()
	response := backend.do("GET", "/servers/"+serverID+"/whitelist", "")
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "application/octet-stream" {
		t.Fatalf("whitelist %s: %d %s", serverID, response.Code, response.Header().Get("Content-Type"))
	}
	data := response.Body.Bytes()
	if len(data)%whitelistEntryBytes != 0 {
		t.Fatalf("whitelist is %d bytes", len(data))
	}
	list := make([]uint64, 0, len(data)/whitelistEntryBytes)
	for i := 0; i < len(data); i += whitelistEntryBytes {
		list = append(list, binary.LittleEndian.Uint64(data[i:]))
	}
	return list
}

// the client id sealed in a token, read the way the server reads it
func tokenClientID(t *testing.T, text string) uint64 {
	t.Helper()
	token, err := base64.StdEncoding.DecodeString(text)
	if err != nil || len(token) != connectTokenBytes {
		t.Fatalf("match answered %q, not a token", text)
	}
	block, ok := box.OpenAnonymous(nil, token[offsetSealedBlock:offsetSealedBlock+sealedBlockBytes], &devServerPublicKey, &devServerSecretKey)
	if !ok {
		t.Fatal("the token's private block does not open")
	}
	return binary.LittleEndian.Uint64(block[0:])
}

func TestMatchRoutes(t *testing.T) {
	backend := newTestBackend(t, sealAnonymous)
	alice := mustHash(t, backend.users, "mock", "alice")
	bob := mustHash(t, backend.users, "mock", "bob")

	if response := backend.do("POST", "/match", backend.ticket(t, "alice")); response.Code != http.StatusServiceUnavailable {
		t.Fatalf("match with no servers: %d", response.Code)
	}
	if response := backend.do("GET", "/servers/one/whitelist", ""); response.Code != http.StatusNotFound {
		t.Fatalf("whitelist of an unregistered server: %d", response.Code)
	}
	backend.register(t, "one")
	if len(backend.whitelist(t, "one")) != 0 {
		t.Fatal("a fresh server's whitelist is not empty")
	}

	// a good ticket gets a token whose client id is the user hash, and a seat
	ticket := backend.ticket(t, "alice")
	response := backend.do("POST", "/match", ticket)
	if response.Code != http.StatusOK || response.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("match: %d %s", response.Code, response.Body)
	}
	if tokenClientID(t, response.Body.String()) != alice {
		t.Fatal("the token's client id is not the user hash")
	}
	if !slices.Equal(backend.whitelist(t, "one"), []uint64{alice}) {
		t.Fatal("the matched user is not on the server's whitelist")
	}

	// the same user again, the same client id
	if tokenClientID(t, backend.do("POST", "/match", backend.ticket(t, "alice")).Body.String()) != alice {
		t.Fatal("the same user got another client id")
	}

	// forged, edited and expired tickets get no token
	raw, _ := base64.StdEncoding.DecodeString(ticket)
	raw[ticketHeaderBytes] ^= 0x20
	forger := &mockProvider{}
	refused := map[string]string{
		"edited": base64.StdEncoding.EncodeToString(raw),
		"forged": mustTicket(t, forger, "alice", testNow),
		"empty":  "",
	}
	for name, text := range refused {
		if response := backend.do("POST", "/match", text); response.Code != http.StatusUnauthorized {
			t.Fatalf("a %s ticket: %d", name, response.Code)
		}
	}
	backend.now = testNow + mockTicketSeconds
	if response := backend.do("POST", "/match", ticket); response.Code != http.StatusUnauthorized {
		t.Fatalf("an expired ticket: %d", response.Code)
	}

	// one seat: alice matched to a second server leaves the first's list
	backend.register(t, "two")
	if tokenClientID(t, backend.do("POST", "/match", backend.ticket(t, "alice")).Body.String()) != alice {
		t.Fatal("match to server two")
	}
	if len(backend.whitelist(t, "one")) != 0 || !slices.Equal(backend.whitelist(t, "two"), []uint64{alice}) {
		t.Fatal("alice is not on exactly one server's whitelist")
	}

	// a ban: off the whitelist at once, and no token
	backend.do("POST", "/match", backend.ticket(t, "bob"))
	if response := backend.do("PUT", "/bans/mock/alice", ""); response.Code != http.StatusNoContent {
		t.Fatalf("ban: %d", response.Code)
	}
	if !slices.Equal(backend.whitelist(t, "two"), []uint64{bob}) {
		t.Fatal("a banned user is still on the whitelist")
	}
	response = backend.do("POST", "/match", backend.ticket(t, "alice"))
	if response.Code != http.StatusForbidden || strings.Contains(response.Body.String(), "AAAA") {
		t.Fatalf("a banned user's match: %d", response.Code)
	}
	if response := backend.do("PUT", "/bans/mo:ck/alice", ""); response.Code != http.StatusBadRequest {
		t.Fatalf("a ban with a bad provider name: %d", response.Code)
	}

	// the ban is by user, not by provider user id: the same id on another provider is not banned
	if mustHash(t, backend.users, "other", "alice") == alice {
		t.Fatal("hash collision across providers")
	}

	// lifted
	if response := backend.do("DELETE", "/bans/mock/alice", ""); response.Code != http.StatusNoContent {
		t.Fatalf("unban: %d", response.Code)
	}
	if backend.do("POST", "/match", backend.ticket(t, "alice")).Code != http.StatusOK {
		t.Fatal("an unbanned user got no token")
	}
}

// The same values the C# backend's test_cross_backend_vectors checks: both
// backends sign tickets and hash users the same way.
const (
	aliceTicketVector = "PNJJawAAAAAFYWxpY2UVSwku1PojgU3PTAD0HSHXU7W62CcrHPOs0kJVr/0h0A=="
	aliceHashVector   = uint64(0x5098a1e5575a941f)
)

// alice's ticket with each of these put inside it is refused by both
var whitespaceVectors = []string{" ", "\n", "\r\n", "\t"}

func TestCrossBackendVectors(t *testing.T) {
	mock, users := testUsers()
	if ticket := mustTicket(t, mock, "alice", testNow); ticket != aliceTicketVector {
		t.Fatalf("alice's ticket is %s", ticket)
	}
	if userHash := mustHash(t, users, "mock", "alice"); userHash != aliceHashVector {
		t.Fatalf("alice's hash is %.16x", userHash)
	}
	if userID, err := mock.verify(aliceTicketVector, testNow); err != nil || userID != "alice" {
		t.Fatalf("alice's ticket did not verify: %q %v", userID, err)
	}
	for _, inserted := range whitespaceVectors {
		text := aliceTicketVector[:8] + inserted + aliceTicketVector[8:]
		if _, err := mock.verify(text, testNow); err == nil {
			t.Fatalf("alice's ticket with %q inside verified", inserted)
		}
	}
}

func TestMatchCommitsSeatAfterMint(t *testing.T) {
	seal := sealFunction(sealAnonymous)
	backend := newTestBackend(t, func(block []byte, key *[keyBytes]byte) ([]byte, error) {
		return seal(block, key)
	})
	backend.register(t, "one")
	ticket := backend.ticket(t, "alice")
	alice := mustHash(t, backend.users, "mock", "alice")
	if response := backend.do("POST", "/match", ticket); response.Code != http.StatusOK {
		t.Fatalf("initial match: %d", response.Code)
	}
	backend.register(t, "two")
	seal = func([]byte, *[keyBytes]byte) ([]byte, error) { return nil, errors.New("test mint failure") }
	if response := backend.do("POST", "/match", ticket); response.Code != http.StatusInternalServerError {
		t.Fatalf("failed mint: %d", response.Code)
	}
	if !slices.Equal(backend.whitelist(t, "one"), []uint64{alice}) || len(backend.whitelist(t, "two")) != 0 {
		t.Fatal("failed mint moved the existing seat")
	}

	seal = func(block []byte, key *[keyBytes]byte) ([]byte, error) {
		backend.users.setBanned(alice, true)
		return sealAnonymous(block, key)
	}
	if response := backend.do("POST", "/match", ticket); response.Code != http.StatusForbidden {
		t.Fatalf("ban during mint: %d", response.Code)
	}
	backend.users.setBanned(alice, false)
	if !slices.Equal(backend.whitelist(t, "one"), []uint64{alice}) || len(backend.whitelist(t, "two")) != 0 {
		t.Fatal("ban during mint moved the existing seat")
	}
}
