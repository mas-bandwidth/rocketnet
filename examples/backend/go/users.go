/*
   RocketNet. Copyright 2026 Más Bandwidth LLC.
   Licensed under the GNU Affero General Public License v3.0

   Users: who a player is, whether they are banned, and which one server
   they may play on.

   A player signs in with an auth provider and gets a ticket. The backend
   checks the ticket in ONE place, verifyTicket, which turns it into the
   provider's name and the provider's user id. This example has no real
   provider: mockProvider stands in for one. A title replaces verifyTicket
   with a call to its platform's own ticket check.

   The client id in every connect token this backend mints is the user hash:
   64 bits of a BLAKE2b keyed with a backend secret, over
   "<provider>:<user id>".
   The client never sends or picks a client id. The id travels inside the
   private block, sealed to the server, and the backend's signature covers
   it, so the client can neither read nor change it.

   Bans are by user hash. A banned user gets no token. Each user hash sits on
   at most one server's whitelist, the server it was last matched to, and a
   server fetches its whitelist and disconnects any client that is not on it.
   A player who shares a token only lets someone else play as them, in their
   one seat, under their bans.
*/

package main

import (
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"maps"
	"slices"
	"strings"
	"sync"
	"unicode/utf8"

	"golang.org/x/crypto/blake2b"
)

const (
	secretKeyBytes      = 32
	maxUserIDBytes      = 128
	ticketMACBytes      = 32
	ticketHeaderBytes   = 9 // expire timestamp (8, little endian), user id length (1)
	maxTicketBytes      = ticketHeaderBytes + maxUserIDBytes + ticketMACBytes
	mockProviderName    = "mock"
	mockTicketSeconds   = 60
	maxTicketTextBytes  = 4 * ((maxTicketBytes + 2) / 3)
	whitelistEntryBytes = 8
)

// The mock auth provider. It signs a ticket for any user id it is asked
// for, so it stands in for a platform's sign-in, never for a real one. A
// ticket is
//
//	expire timestamp (8 bytes, little endian) | user id length (1) | user id | MAC (32)
//
// as standard base64, where the MAC is BLAKE2b-256 keyed with the
// provider's own secret over everything before it. Only the holder of that
// secret can make a ticket or change one, and a ticket stops working at its
// expire timestamp.

type mockProvider struct {
	key [secretKeyBytes]byte
}

// a user id is 1 to 128 bytes of UTF-8
func validUserID(userID string) bool {
	return len(userID) > 0 && len(userID) <= maxUserIDBytes && utf8.ValidString(userID)
}

func (provider *mockProvider) mac(data []byte) []byte {
	hash, err := blake2b.New256(provider.key[:])
	if err != nil {
		panic(err) // a 32 byte key is always valid
	}
	hash.Write(data)
	return hash.Sum(nil)
}

// issues a ticket for userID that expires expireSeconds after now (unix seconds)
func (provider *mockProvider) issue(userID string, now uint64, expireSeconds uint64) (string, error) {
	if !validUserID(userID) {
		return "", fmt.Errorf("a user id is 1 to %d bytes", maxUserIDBytes)
	}
	ticket := make([]byte, ticketHeaderBytes, maxTicketBytes)
	binary.LittleEndian.PutUint64(ticket[0:], now+expireSeconds)
	ticket[8] = byte(len(userID))
	ticket = append(ticket, userID...)
	ticket = append(ticket, provider.mac(ticket)...)
	return base64.StdEncoding.EncodeToString(ticket), nil
}

var errTicket = errors.New("the ticket is not valid")

// checks a ticket at now (unix seconds) and returns its user id
func (provider *mockProvider) verify(text string, now uint64) (string, error) {
	if len(text) > maxTicketTextBytes {
		return "", errTicket
	}
	ticket, err := base64.StdEncoding.DecodeString(text)
	if err != nil || len(ticket) < ticketHeaderBytes+1+ticketMACBytes {
		return "", errTicket
	}
	userBytes := int(ticket[8])
	if len(ticket) != ticketHeaderBytes+userBytes+ticketMACBytes {
		return "", errTicket
	}
	signed := ticket[:ticketHeaderBytes+userBytes]
	if subtle.ConstantTimeCompare(provider.mac(signed), ticket[len(signed):]) != 1 {
		return "", errTicket
	}
	if binary.LittleEndian.Uint64(ticket[0:]) <= now {
		return "", errors.New("the ticket has expired")
	}
	userID := string(ticket[ticketHeaderBytes : ticketHeaderBytes+userBytes])
	if !validUserID(userID) {
		return "", errTicket
	}
	return userID, nil
}

// THE AUTH PROVIDER SEAM. It checks the ticket a client sent and returns the
// provider's name and that provider's user id for the player. A title
// replaces the body with its platform's own ticket check (or several, one
// per platform, each returning its own provider name). The provider name
// may not contain ':', so "<provider>:<user id>" reads only one way.

type verifyTicketFunction func(ticket string, now uint64) (provider string, userID string, err error)

func verifyTicketWith(mock *mockProvider) verifyTicketFunction {
	return func(ticket string, now uint64) (string, string, error) {
		userID, err := mock.verify(ticket, now)
		if err != nil {
			return "", "", err
		}
		return mockProviderName, userID, nil
	}
}

// The users: bans and seats, keyed by user hash, in memory like the server
// registry. A real backend keeps them in its database.

type users struct {
	mutex   sync.Mutex
	hashKey [secretKeyBytes]byte
	banned  map[uint64]bool
	seats   map[uint64]string // user hash -> the id of the one server whose whitelist holds it
}

func newUsers(hashKey [secretKeyBytes]byte) *users {
	return &users{hashKey: hashKey, banned: make(map[uint64]bool), seats: make(map[uint64]string)}
}

// the user hash, which is the client id: the first 8 bytes, read little
// endian, of BLAKE2b-256 keyed with the backend's secret over
// "<provider>:<user id>"
func (users *users) hash(provider string, userID string) (uint64, error) {
	if provider == "" || strings.Contains(provider, ":") {
		return 0, errors.New("a provider name is not empty and has no ':'")
	}
	if !validUserID(userID) {
		return 0, fmt.Errorf("a user id is 1 to %d bytes", maxUserIDBytes)
	}
	hash, err := blake2b.New256(users.hashKey[:])
	if err != nil {
		return 0, err
	}
	io.WriteString(hash, provider)
	io.WriteString(hash, ":")
	io.WriteString(hash, userID)
	return binary.LittleEndian.Uint64(hash.Sum(nil)), nil
}

func (users *users) isBanned(userHash uint64) bool {
	users.mutex.Lock()
	defer users.mutex.Unlock()
	return users.banned[userHash]
}

func (users *users) setBanned(userHash uint64, banned bool) {
	users.mutex.Lock()
	defer users.mutex.Unlock()
	if banned {
		users.banned[userHash] = true
	} else {
		delete(users.banned, userHash)
	}
}

// seats userHash on serverID, moving it off any other server's whitelist; a
// banned user is refused and seated nowhere
func (users *users) seat(userHash uint64, serverID string) bool {
	users.mutex.Lock()
	defer users.mutex.Unlock()
	if users.banned[userHash] {
		return false
	}
	users.seats[userHash] = serverID
	return true
}

// the client ids allowed on serverID: matched there and not banned, ascending
func (users *users) whitelist(serverID string) []uint64 {
	users.mutex.Lock()
	list := make([]uint64, 0, len(users.seats))
	for userHash, seated := range users.seats {
		if seated == serverID && !users.banned[userHash] {
			list = append(list, userHash)
		}
	}
	users.mutex.Unlock()
	slices.Sort(list)
	return list
}

// The whitelist on the wire: each client id as 8 bytes little endian,
// ascending, nothing else. An empty list is an empty body.

func writeWhitelist(list []uint64) []byte {
	data := make([]byte, len(list)*whitelistEntryBytes)
	for i, id := range list {
		binary.LittleEndian.PutUint64(data[i*whitelistEntryBytes:], id)
	}
	return data
}

// a backend secret from the environment (base64 of 32 bytes), or a fresh
// random one when the variable is unset
func readSecretKey(getenv func(string) string, name string, random io.Reader) ([secretKeyBytes]byte, bool, error) {
	var key [secretKeyBytes]byte
	if text := getenv(name); text != "" {
		decoded, err := base64.StdEncoding.DecodeString(text)
		if err != nil || len(decoded) != secretKeyBytes {
			return key, false, fmt.Errorf("%s is not the base64 of a 32 byte key", name)
		}
		copy(key[:], decoded)
		return key, true, nil
	}
	if _, err := io.ReadFull(random, key[:]); err != nil {
		return key, false, fmt.Errorf("failed to generate %s: %w", name, err)
	}
	return key, false, nil
}

// pick selects a registered server with choose and returns its id with it
func (registry *serverRegistry) pick() (string, registeredServer, bool, error) {
	registry.mutex.Lock()
	defer registry.mutex.Unlock()
	if len(registry.servers) == 0 {
		return "", registeredServer{}, false, nil
	}
	index, err := registry.choose(len(registry.servers))
	if err != nil || index < 0 || index >= len(registry.servers) {
		return "", registeredServer{}, false, errors.New("failed to select a registered server")
	}
	// in id order, so a choice of index always names the same server
	ids := slices.Sorted(maps.Keys(registry.servers))
	server := registry.servers[ids[index]]
	server.addresses = append([]string(nil), server.addresses...)
	return ids[index], server, true, nil
}

func (registry *serverRegistry) has(id string) bool {
	registry.mutex.Lock()
	defer registry.mutex.Unlock()
	_, found := registry.servers[id]
	return found
}
