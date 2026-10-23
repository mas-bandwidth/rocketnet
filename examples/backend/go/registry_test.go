package main

import (
	"bytes"
	"encoding/base64"
	"testing"
)

func TestServerRegistry(t *testing.T) {
	registry := newServerRegistry(func(max int) (int, error) { return max - 1, nil })
	if _, found, err := registry.random(); err != nil || found {
		t.Fatal("empty registry selected a server")
	}

	keyA := bytes.Repeat([]byte{0x11}, keyBytes)
	keyB := bytes.Repeat([]byte{0x22}, keyBytes)
	if err := registry.register("one", []string{"127.0.0.1:40000"}, base64.StdEncoding.EncodeToString(keyA)); err != nil {
		t.Fatal(err)
	}
	if err := registry.register("one", []string{"127.0.0.1:40001"}, base64.StdEncoding.EncodeToString(keyB)); err != nil {
		t.Fatal(err)
	}
	server, found, err := registry.random()
	if err != nil || !found || len(server.addresses) != 1 || server.addresses[0] != "127.0.0.1:40001" || !bytes.Equal(server.publicKey[:], keyB) {
		t.Fatal("re-registration did not replace the server")
	}

	if err := registry.register("two", []string{"[::1]:40002"}, base64.StdEncoding.EncodeToString(keyA)); err != nil {
		t.Fatal(err)
	}
	server, found, err = registry.random()
	valid := found && ((server.addresses[0] == "127.0.0.1:40001" && bytes.Equal(server.publicKey[:], keyB)) ||
		(server.addresses[0] == "[::1]:40002" && bytes.Equal(server.publicKey[:], keyA)))
	if err != nil || !valid {
		t.Fatal("random selection returned a server outside the registered set")
	}

	if err := registry.register("bad", []string{"127.0.0.1:0"}, base64.StdEncoding.EncodeToString(keyA)); err == nil {
		t.Fatal("invalid address was accepted")
	}
	if err := registry.register("bad", []string{"127.0.0.1:40000"}, "AAAA"); err == nil {
		t.Fatal("invalid public key was accepted")
	}
}

func TestServiceConfigAllowsNoInitialServer(t *testing.T) {
	config, err := readServiceConfig(environment(map[string]string{}))
	if err != nil || config.serverConfigured || len(config.serverAddresses) != 0 {
		t.Fatal("service config did not allow an empty registry")
	}
}
