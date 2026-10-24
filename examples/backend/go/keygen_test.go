package main

import (
	"bytes"
	"encoding/base64"
	"io"
	"strings"
	"testing"
)

func TestKeygenEnvironment(t *testing.T) {
	// The standard's published development seed provides a fixed known answer.
	var output bytes.Buffer
	if err := writeBackendKeys(&output, bytes.NewReader(devBackendPrivateKey[:32])); err != nil {
		t.Fatal(err)
	}
	expected := "ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=" + base64.StdEncoding.EncodeToString(devBackendPrivateKey[32:]) + "\n" +
		"ROCKETNET_BACKEND_PRIVATE_KEY=" + base64.StdEncoding.EncodeToString(devBackendPrivateKey) + "\n"
	if output.String() != expected {
		t.Fatal("keygen environment differs from the standard's signing keypair")
	}
	output.Reset()
	if err := writeBackendKeys(&output, strings.NewReader("")); err != io.EOF || output.Len() != 0 {
		t.Fatal("entropy failure must refuse generation without emitting keys")
	}
}
