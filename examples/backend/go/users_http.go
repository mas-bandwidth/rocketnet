/*
   RocketNet. Copyright 2026 Más Bandwidth LLC.
   Licensed under the GNU Affero General Public License v3.0 or later, or the Más Bandwidth Source License 1.0 (MBSL-1.0). See LICENSE and MBSL.md.

   The HTTP routes for users (users.go):

       POST   /mock/ticket/{userID}            the mock auth provider signs a ticket
       POST   /match                           ticket in, connect token out
       PUT    /bans/{provider}/{userID}        ban a user
       DELETE /bans/{provider}/{userID}        lift the ban
       GET    /servers/{serverID}/whitelist    the client ids allowed on a server

   Each handler takes the time (unix seconds) from a function, so the unit
   tests pass a fixed clock.
*/

package main

import (
	"encoding/base64"
	"io"
	"net/http"
	"strconv"
	"strings"
)

// the mock auth provider's sign-in: any user id gets a ticket. It stands in
// for a platform's own sign-in, which runs on the client and on the
// platform's servers, not in the game's backend
func mockTicketHandler(mock *mockProvider, now func() uint64) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ticket, err := mock.issue(r.PathValue("userID"), now(), mockTicketSeconds)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		io.WriteString(w, ticket)
	}
}

// the match: the body is the ticket. A bad or expired ticket is 401, a
// banned user 403, no servers 503; otherwise the user is seated on one
// server and gets a connect token for it whose client id is their user hash
func matchTicketHandler(config *config, registry *serverRegistry, users *users, verify verifyTicketFunction, now func() uint64, random io.Reader, seal sealFunction) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, maxTicketTextBytes+2)
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "the ticket is too long", http.StatusBadRequest)
			return
		}
		timestamp := now()
		provider, userID, err := verify(strings.TrimSpace(string(body)), timestamp)
		if err != nil {
			http.Error(w, err.Error(), http.StatusUnauthorized)
			return
		}
		userHash, err := users.hash(provider, userID)
		if err != nil {
			http.Error(w, err.Error(), http.StatusUnauthorized)
			return
		}
		serverID, server, found, err := registry.pick()
		if err != nil {
			http.Error(w, "failed to select a server", http.StatusInternalServerError)
			return
		}
		if !found {
			http.Error(w, "no servers are registered", http.StatusServiceUnavailable)
			return
		}
		if users.isBanned(userHash) {
			http.Error(w, "this user is banned", http.StatusForbidden)
			return
		}
		selected := *config
		selected.serverAddresses = server.addresses
		selected.serverPublicKey = server.publicKey
		token, err := generateConnectToken(&selected, userHash, timestamp, random, seal)
		if err != nil {
			stderrLogger.Printf("failed to generate a connect token: %v", err)
			http.Error(w, "failed to generate a connect token", http.StatusInternalServerError)
			return
		}
		// Commit the seat only after minting succeeds. A ban during minting
		// must still refuse this match before the token goes to the client.
		if !users.seat(userHash, serverID) {
			http.Error(w, "this user is banned", http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		// a token is for one client, once: never cached
		w.Header().Set("Cache-Control", "no-store")
		io.WriteString(w, base64.StdEncoding.EncodeToString(token))
		stderrLogger.Printf("matched user %.16x to server %s", userHash, serverID)
	}
}

func banHandler(users *users, banned bool) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		userHash, err := users.hash(r.PathValue("provider"), r.PathValue("userID"))
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		users.setBanned(userHash, banned)
		if banned {
			stderrLogger.Printf("banned user %.16x", userHash)
		} else {
			stderrLogger.Printf("unbanned user %.16x", userHash)
		}
		w.WriteHeader(http.StatusNoContent)
	}
}

// the whitelist: 8 bytes little endian per client id, ascending; 404 for a
// server that is not registered, so it registers again
func whitelistHandler(registry *serverRegistry, users *users) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		serverID := r.PathValue("serverID")
		if !registry.has(serverID) {
			http.Error(w, "this server is not registered", http.StatusNotFound)
			return
		}
		data := writeWhitelist(users.whitelist(serverID))
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", strconv.Itoa(len(data)))
		w.Header().Set("Cache-Control", "no-store")
		w.Write(data)
	}
}
