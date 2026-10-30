/*
   RocketNet. Copyright 2026 Más Bandwidth LLC.
   Licensed under the GNU Affero General Public License v3.0

   Unit tests: no sockets, no real time. The known answers are the C
   library's own, standard/vectors/connect_token_v1.json, minted by
   rocketnet_write_connect_token_deterministic; this backend must write
   every one of them byte for byte.
*/

package main

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"strconv"
	"testing"

	"golang.org/x/crypto/blake2b"
	"golang.org/x/crypto/curve25519"
	"golang.org/x/crypto/nacl/box"
)

type vector struct {
	Name               string `json:"name"`
	BackendSigningSeed string `json:"backend_signing_seed"`
	ServerPublicKey    string `json:"server_public_key"`
	EphemeralSecret    string `json:"ephemeral_secret"`
	ClientID           string `json:"client_id"`
	ProtocolID         string `json:"protocol_id"`
	CreateTimestamp    string `json:"create_timestamp"`
	ExpireTimestamp    string `json:"expire_timestamp"`
	TimeoutSeconds     string `json:"timeout_seconds"`
	AddressFlags       string `json:"address_flags"`
	IPv4Address        string `json:"ipv4_address"`
	IPv6Address        string `json:"ipv6_address"`
	UserData           string `json:"user_data"`
	ClientToServerKey  string `json:"client_to_server_key"`
	ServerToClientKey  string `json:"server_to_client_key"`
	Token              string `json:"token"`
}

func loadVectors(t *testing.T) []vector {
	data, err := os.ReadFile("../../../standard/vectors/connect_token_v1.json")
	if err != nil {
		t.Fatal(err)
	}
	var file struct {
		Version string   `json:"version"`
		Vectors []vector `json:"vectors"`
	}
	if err := json.Unmarshal(data, &file); err != nil {
		t.Fatal(err)
	}
	if file.Version != strconv.FormatUint(wireVersion, 16) || len(file.Vectors) < 4 {
		t.Fatalf("vectors file version %s with %d vectors", file.Version, len(file.Vectors))
	}
	return file.Vectors
}

func unhex(t *testing.T, text string, bytes int) []byte {
	data, err := hex.DecodeString(text)
	if err != nil || len(data) != bytes {
		t.Fatalf("%q is not %d bytes of hex", text, bytes)
	}
	return data
}

// the vectors' integers are fixed-width big-endian hex

func unhexUint64(t *testing.T, text string) uint64 {
	return binary.BigEndian.Uint64(unhex(t, text, 8))
}

// crypto_box_seal with the ephemeral secret supplied, as the test-only C
// writer does it: epk = X25519(esk, basepoint), nonce = BLAKE2b-192(epk ||
// server_pk), sealed = epk || crypto_box_easy(block, nonce, server_pk, esk)

func sealWithEphemeral(ephemeralSecret []byte) sealFunction {
	return func(block []byte, serverPublicKey *[keyBytes]byte) ([]byte, error) {
		epk, err := curve25519.X25519(ephemeralSecret, curve25519.Basepoint)
		if err != nil {
			return nil, err
		}
		hash, err := blake2b.New(24, nil)
		if err != nil {
			return nil, err
		}
		hash.Write(epk)
		hash.Write(serverPublicKey[:])
		var nonce [24]byte
		copy(nonce[:], hash.Sum(nil))
		var esk [keyBytes]byte
		copy(esk[:], ephemeralSecret)
		return box.Seal(epk, block, &nonce, serverPublicKey, &esk), nil
	}
}

func TestConnectTokenVectors(t *testing.T) {
	for _, v := range loadVectors(t) {
		signed := signedConnectData{
			ProtocolID:      unhexUint64(t, v.ProtocolID),
			CreateTimestamp: unhexUint64(t, v.CreateTimestamp),
			ExpireTimestamp: unhexUint64(t, v.ExpireTimestamp),
			TimeoutSeconds:  int32(binary.BigEndian.Uint32(unhex(t, v.TimeoutSeconds, 4))),
			AddressFlags:    unhex(t, v.AddressFlags, 1)[0],
		}
		copy(signed.IPv4Address[:], unhex(t, v.IPv4Address, 6))
		copy(signed.IPv6Address[:], unhex(t, v.IPv6Address, 18))
		copy(signed.ServerPublicKey[:], unhex(t, v.ServerPublicKey, keyBytes))
		private := connectTokenPrivate{ClientID: unhexUint64(t, v.ClientID)}
		copy(private.ClientToServerKey[:], unhex(t, v.ClientToServerKey, keyBytes))
		copy(private.ServerToClientKey[:], unhex(t, v.ServerToClientKey, keyBytes))
		copy(private.UserData[:], unhex(t, v.UserData, userDataBytes))
		backendPrivateKey := ed25519.NewKeyFromSeed(unhex(t, v.BackendSigningSeed, ed25519.SeedSize))

		token, err := writeConnectToken(&signed, &private, backendPrivateKey, sealWithEphemeral(unhex(t, v.EphemeralSecret, keyBytes)))
		if err != nil {
			t.Fatalf("%s: %v", v.Name, err)
		}
		if !bytes.Equal(token, unhex(t, v.Token, connectTokenBytes)) {
			t.Fatalf("%s: token differs from the vector\n got %x\nwant %s", v.Name, token, v.Token)
		}
	}
}

// the address strings that encode to each vector's address fields

func TestServerAddressesMatchVectors(t *testing.T) {
	addresses := map[string][]string{
		"ipv4_appendix_inputs":             {"127.0.0.1:40000"},
		"ipv6_loopback_counting_user_data": {"[::1]:50000"},
		"dual_stack_negative_timeout":      {"10.0.0.1:1", "[2001:db8::1]:65535"},
		"extreme_values_text_user_data":    {"255.255.255.255:65535"},
	}
	for _, v := range loadVectors(t) {
		list, ok := addresses[v.Name]
		if !ok {
			t.Fatalf("no address strings for vector %s", v.Name)
		}
		var signed signedConnectData
		if err := writeServerAddresses(&signed, list); err != nil {
			t.Fatalf("%s: %v", v.Name, err)
		}
		if signed.AddressFlags != unhex(t, v.AddressFlags, 1)[0] ||
			!bytes.Equal(signed.IPv4Address[:], unhex(t, v.IPv4Address, 6)) ||
			!bytes.Equal(signed.IPv6Address[:], unhex(t, v.IPv6Address, 18)) {
			t.Fatalf("%s: %v encode to flags %02x ipv4 %x ipv6 %x", v.Name, list, signed.AddressFlags, signed.IPv4Address, signed.IPv6Address)
		}
	}
}

func TestServerAddressesRefused(t *testing.T) {
	for _, list := range [][]string{
		{},
		{"127.0.0.1:1", "[::1]:1", "10.0.0.1:1"},
		{"127.0.0.1:1", "10.0.0.1:1"},
		{"[::1]:1", "[::2]:1"},
		{"127.0.0.1"},
		{"127.0.0.1:0"},
		{"localhost:40000"},
		{"[fe80::1%en0]:40000"},
	} {
		var signed signedConnectData
		if writeServerAddresses(&signed, list) == nil {
			t.Fatalf("%v was accepted", list)
		}
	}
}

// the published development server keypair (sdk/rocketnet_connect_token.cpp),
// standing in for a server's own

var devServerPublicKey = [keyBytes]byte{
	0x3d, 0x24, 0x90, 0x88, 0x9d, 0x37, 0x4c, 0x29, 0x6f, 0xab, 0x1d, 0x57,
	0x70, 0xcb, 0x71, 0x47, 0x6a, 0x06, 0xf7, 0x4e, 0xf2, 0xf2, 0x54, 0x0c,
	0xf2, 0x21, 0x5f, 0x76, 0x14, 0xf4, 0x80, 0x1c,
}

var devServerSecretKey = [keyBytes]byte{
	0x6f, 0xfb, 0x8f, 0x73, 0x37, 0xec, 0x37, 0x76, 0x97, 0xf5, 0x68, 0xde,
	0x31, 0x18, 0xd8, 0x39, 0x59, 0x56, 0x0b, 0x25, 0x92, 0x8d, 0x22, 0x70,
	0x7b, 0xa2, 0x29, 0x75, 0x89, 0xd0, 0x8b, 0x97,
}

func environment(values map[string]string) func(string) string {
	return func(name string) string { return values[name] }
}

// a production mint (fresh ephemeral seal) verifies against the development
// backend public key and opens with the server's secret key, in the order a
// server checks it

func TestGeneratedTokenOpens(t *testing.T) {
	config, err := readConfig(environment(map[string]string{
		"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": base64.StdEncoding.EncodeToString(devServerPublicKey[:]),
		"ROCKETNET_BACKEND_SERVER_ADDRESSES":  "127.0.0.1:40000,[::1]:40001",
	}))
	if err != nil {
		t.Fatal(err)
	}
	if !config.developmentKey {
		t.Fatal("no ROCKETNET_BACKEND_PRIVATE_KEY did not select the development key")
	}
	keys := bytes.Repeat([]byte{0x5a}, keyBytes*2)
	token, err := generateConnectToken(config, 0x0123456789abcdef, 1000, bytes.NewReader(keys), sealAnonymous)
	if err != nil {
		t.Fatal(err)
	}
	if len(token) != connectTokenBytes || len(base64.StdEncoding.EncodeToString(token)) != 1024 {
		t.Fatalf("token is %d bytes", len(token))
	}

	devBackendPublicKey := ed25519.PrivateKey(devBackendPrivateKey).Public().(ed25519.PublicKey)
	if !ed25519.Verify(devBackendPublicKey, token[:signedConnectDataBytes], token[signedConnectDataBytes:signedConnectDataBytes+signatureBytes]) {
		t.Fatal("signature does not verify")
	}
	if binary.LittleEndian.Uint32(token[0:]) != wireVersion ||
		binary.LittleEndian.Uint64(token[4:]) != exampleProtocolID ||
		binary.LittleEndian.Uint64(token[12:]) != 1000 ||
		binary.LittleEndian.Uint64(token[20:]) != 1030 ||
		binary.LittleEndian.Uint32(token[28:]) != 10 ||
		token[32] != addressFlagIPv4|addressFlagIPv6 {
		t.Fatalf("signed fields %x", token[:57])
	}
	block, ok := box.OpenAnonymous(nil, token[offsetSealedBlock:offsetSealedBlock+sealedBlockBytes], &devServerPublicKey, &devServerSecretKey)
	if !ok || len(block) != privateBlockBytes {
		t.Fatal("sealed block does not open with the server key")
	}
	if binary.LittleEndian.Uint64(block[0:]) != 0x0123456789abcdef ||
		binary.LittleEndian.Uint64(block[8:]) != exampleProtocolID ||
		binary.LittleEndian.Uint64(block[16:]) != 1030 ||
		!bytes.Equal(block[24:24+keyBytes*2], keys) ||
		!bytes.Equal(token[signedConnectDataBytes+signatureBytes:signedConnectDataBytes+signatureBytes+keyBytes*2], keys) {
		t.Fatalf("private block %x", block[:88])
	}
	if !bytes.Equal(block[88:], make([]byte, privateBlockBytes-88)) || !bytes.Equal(token[704:], make([]byte, connectTokenBytes-704)) {
		t.Fatal("padding is not zero")
	}
}

func TestConfigRefused(t *testing.T) {
	serverKey := base64.StdEncoding.EncodeToString(devServerPublicKey[:])
	badPrivateKey := append([]byte{}, devBackendPrivateKey...)
	badPrivateKey[63] ^= 1
	for _, values := range []map[string]string{
		{},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": "AAAA"},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_PRIVATE_KEY": base64.StdEncoding.EncodeToString(devBackendPrivateKey[:32])},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_PRIVATE_KEY": base64.StdEncoding.EncodeToString(badPrivateKey)},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_PROTOCOL_ID": "0"},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_SERVER_ADDRESSES": "127.0.0.1:1,10.0.0.1:1"},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_PORT": "65536"},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS": "0"},
		{"ROCKETNET_BACKEND_SERVER_PUBLIC_KEY": serverKey, "ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS": "-1"},
	} {
		if _, err := readConfig(environment(values)); err == nil {
			t.Fatalf("%v was accepted", values)
		}
	}
}

// the backend listens on loopback unless told otherwise: registration and
// token issuance are unauthenticated

func TestListenAddress(t *testing.T) {
	config, err := readConfigMode(environment(map[string]string{}), false)
	if err != nil {
		t.Fatal(err)
	}
	if config.listenAddress != "127.0.0.1" || !listenAddressIsLoopback(config.listenAddress) || listenHostPort(config) != "127.0.0.1:8080" {
		t.Fatalf("default listen address is not loopback: %q", listenHostPort(config))
	}
	config, err = readConfigMode(environment(map[string]string{"ROCKETNET_BACKEND_LISTEN_ADDRESS": "0.0.0.0", "ROCKETNET_BACKEND_PORT": "9000"}), false)
	if err != nil {
		t.Fatal(err)
	}
	if listenHostPort(config) != "0.0.0.0:9000" || listenAddressIsLoopback(config.listenAddress) {
		t.Fatalf("explicit listen address not honoured: %q", listenHostPort(config))
	}
	config, err = readConfigMode(environment(map[string]string{"ROCKETNET_BACKEND_LISTEN_ADDRESS": "::1"}), false)
	if err != nil || listenHostPort(config) != "[::1]:8080" || !listenAddressIsLoopback(config.listenAddress) {
		t.Fatalf("::1: %v %v", err, config)
	}
	if _, err := readConfigMode(environment(map[string]string{"ROCKETNET_BACKEND_LISTEN_ADDRESS": "localhost"}), false); err == nil {
		t.Fatal("a non-IP listen address was accepted")
	}
}
