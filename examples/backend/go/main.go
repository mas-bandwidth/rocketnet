/*
   RocketNet. Copyright 2026 Más Bandwidth LLC.
   Licensed under the GNU Affero General Public License v3.0

   A very simple backend, in the spirit of yojimbo's matcher: one HTTP
   endpoint that mints a RocketNet connect token for a client and returns it
   as base64. The client passes the string to rocketnet_client_connect, and
   the token sends it to the one server it names.

   The token is the 768 byte connect token of STANDARD.md, byte for byte the
   one sdk/rocketnet_connect_token.cpp writes: the backend signs the signed
   connect data with its Ed25519 key, and the private block inside it is
   sealed (crypto_box_seal) to the server's X25519 public key, so only that
   server can read the session keys.

   Keys and settings come from the environment (see README.md). With none
   set, the backend uses the published development backend key, which the
   standard prints, so ANYONE can mint tokens a server using it accepts. It
   is for local runs only, and a release server refuses it at start.

   A real backend authenticates the player before minting a token for them;
   this one mints for any client id it is asked for.
*/

package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/netip"
	"os"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/nacl/box"
)

// The connect token wire format (sdk/rocketnet_connect_token.h).

const (
	wireVersion            = 0x4E7C1A93
	connectTokenBytes      = 768
	signedConnectDataBytes = 576
	privateBlockBytes      = 384
	sealedBlockBytes       = 432
	signatureBytes         = 64
	keyBytes               = 32
	userDataBytes          = 256
	addressFlagIPv4        = 1 << 0
	addressFlagIPv6        = 1 << 1
	offsetSealedBlock      = 89
)

// The published development backend key, from the standard. DEVELOPMENT
// ONLY: its secret half is public, so a server that accepts it accepts
// tokens from anyone. Its public half is ROCKETNET_DEV_BACKEND_PUBLIC_KEY.

var devBackendPrivateKey = []byte{
	0x75, 0x29, 0x02, 0x15, 0xe1, 0xff, 0x4d, 0xf2, 0x09, 0x92, 0x57, 0x8a,
	0x22, 0x3c, 0x7c, 0xd4, 0x63, 0xb9, 0xa2, 0x7c, 0xf8, 0x3a, 0xa7, 0x1c,
	0x7a, 0x16, 0x93, 0x1c, 0x01, 0x9b, 0x7c, 0x30, 0xb0, 0xab, 0x03, 0xb3,
	0xe2, 0x6e, 0xf1, 0x7c, 0x72, 0xe3, 0xbd, 0x99, 0x29, 0xa5, 0xb4, 0xbf,
	0x48, 0xbd, 0xba, 0x50, 0x57, 0x3f, 0x1b, 0x8d, 0x08, 0x4a, 0x08, 0xd8,
	0x58, 0xbd, 0x24, 0x2c,
}

// the examples' protocol id (examples/dev_keys.h)
const exampleProtocolID = 0x52434b544e455431

// The fields the backend signs: everything a server checks before it opens
// the sealed private block.

type signedConnectData struct {
	ProtocolID      uint64
	CreateTimestamp uint64
	ExpireTimestamp uint64
	TimeoutSeconds  int32
	AddressFlags    uint8
	IPv4Address     [6]byte  // a,b,c,d octets then the port little-endian
	IPv6Address     [18]byte // 8 uint16 groups little-endian then the port little-endian
	ServerPublicKey [keyBytes]byte
}

// The fields sealed to the server. The private block's protocol id and
// expire are always the signed ones.

type connectTokenPrivate struct {
	ClientID          uint64
	ClientToServerKey [keyBytes]byte
	ServerToClientKey [keyBytes]byte
	UserData          [userDataBytes]byte
}

// seals the private block to the server public key: box.SealAnonymous, which
// is crypto_box_seal, in production; the known-answer tests pass a seal with
// a fixed ephemeral key

type sealFunction func(block []byte, serverPublicKey *[keyBytes]byte) ([]byte, error)

func sealAnonymous(block []byte, serverPublicKey *[keyBytes]byte) ([]byte, error) {
	return box.SealAnonymous(nil, block, serverPublicKey, rand.Reader)
}

func writeConnectTokenPrivate(signed *signedConnectData, private *connectTokenPrivate) []byte {
	block := make([]byte, privateBlockBytes)
	binary.LittleEndian.PutUint64(block[0:], private.ClientID)
	binary.LittleEndian.PutUint64(block[8:], signed.ProtocolID)
	binary.LittleEndian.PutUint64(block[16:], signed.ExpireTimestamp)
	copy(block[24:], private.ClientToServerKey[:])
	copy(block[24+keyBytes:], private.ServerToClientKey[:])
	copy(block[24+keyBytes*2:], private.UserData[:])
	return block
}

func writeConnectToken(signed *signedConnectData, private *connectTokenPrivate, backendPrivateKey ed25519.PrivateKey, seal sealFunction) ([]byte, error) {
	block := writeConnectTokenPrivate(signed, private)
	sealed, err := seal(block, &signed.ServerPublicKey)
	clear(block)
	if err != nil {
		return nil, fmt.Errorf("failed to seal the private block: %w", err)
	}
	if len(sealed) != sealedBlockBytes {
		return nil, fmt.Errorf("sealed block is %d bytes, not %d", len(sealed), sealedBlockBytes)
	}

	token := make([]byte, connectTokenBytes)
	binary.LittleEndian.PutUint32(token[0:], wireVersion)
	binary.LittleEndian.PutUint64(token[4:], signed.ProtocolID)
	binary.LittleEndian.PutUint64(token[12:], signed.CreateTimestamp)
	binary.LittleEndian.PutUint64(token[20:], signed.ExpireTimestamp)
	binary.LittleEndian.PutUint32(token[28:], uint32(signed.TimeoutSeconds))
	token[32] = signed.AddressFlags
	copy(token[33:], signed.IPv4Address[:])
	copy(token[39:], signed.IPv6Address[:])
	copy(token[57:], signed.ServerPublicKey[:])
	copy(token[offsetSealedBlock:], sealed)

	signature := ed25519.Sign(backendPrivateKey, token[:signedConnectDataBytes])
	copy(token[signedConnectDataBytes:], signature)
	copy(token[signedConnectDataBytes+signatureBytes:], private.ClientToServerKey[:])
	copy(token[signedConnectDataBytes+signatureBytes+keyBytes:], private.ServerToClientKey[:])
	return token, nil
}

// one server's addresses: at most one IPv4 and one IPv6, as "a.b.c.d:port"
// and "[ipv6]:port"

func writeServerAddresses(signed *signedConnectData, addresses []string) error {
	if len(addresses) < 1 || len(addresses) > 2 {
		return fmt.Errorf("a token names one server by one or two addresses, not %d", len(addresses))
	}
	for _, text := range addresses {
		address, err := netip.ParseAddrPort(strings.TrimSpace(text))
		if err != nil || address.Port() == 0 || address.Addr().Zone() != "" {
			return fmt.Errorf("invalid server address %q", text)
		}
		ip := address.Addr()
		if ip.Is4() {
			if signed.AddressFlags&addressFlagIPv4 != 0 {
				return errors.New("more than one IPv4 server address")
			}
			signed.AddressFlags |= addressFlagIPv4
			octets := ip.As4()
			copy(signed.IPv4Address[:], octets[:])
			binary.LittleEndian.PutUint16(signed.IPv4Address[4:], address.Port())
		} else {
			if signed.AddressFlags&addressFlagIPv6 != 0 {
				return errors.New("more than one IPv6 server address")
			}
			signed.AddressFlags |= addressFlagIPv6
			octets := ip.As16()
			for i := 0; i < 8; i++ {
				binary.LittleEndian.PutUint16(signed.IPv6Address[i*2:], binary.BigEndian.Uint16(octets[i*2:]))
			}
			binary.LittleEndian.PutUint16(signed.IPv6Address[16:], address.Port())
		}
	}
	return nil
}

type config struct {
	port              int
	backendPrivateKey ed25519.PrivateKey
	protocolID        uint64
	serverAddresses   []string
	serverPublicKey   [keyBytes]byte
	expireSeconds     uint64
	timeoutSeconds    int32
	developmentKey    bool
}

// mints a token for clientID with fresh session keys, created at
// createTimestamp (unix seconds)

func generateConnectToken(config *config, clientID uint64, createTimestamp uint64, random io.Reader, seal sealFunction) ([]byte, error) {
	signed := signedConnectData{
		ProtocolID:      config.protocolID,
		CreateTimestamp: createTimestamp,
		ExpireTimestamp: createTimestamp + config.expireSeconds,
		TimeoutSeconds:  config.timeoutSeconds,
		ServerPublicKey: config.serverPublicKey,
	}
	if err := writeServerAddresses(&signed, config.serverAddresses); err != nil {
		return nil, err
	}
	private := connectTokenPrivate{ClientID: clientID}
	if _, err := io.ReadFull(random, private.ClientToServerKey[:]); err != nil {
		return nil, fmt.Errorf("failed to generate the client to server key: %w", err)
	}
	if _, err := io.ReadFull(random, private.ServerToClientKey[:]); err != nil {
		return nil, fmt.Errorf("failed to generate the server to client key: %w", err)
	}
	token, err := writeConnectToken(&signed, &private, config.backendPrivateKey, seal)
	clear(private.ClientToServerKey[:])
	clear(private.ServerToClientKey[:])
	return token, err
}

// reads the config from the environment through getenv, so tests pass a map

func readConfig(getenv func(string) string) (*config, error) {
	config := &config{
		port:            8080,
		protocolID:      exampleProtocolID,
		serverAddresses: []string{"127.0.0.1:40000"},
		expireSeconds:   30,
		timeoutSeconds:  10,
	}

	if text := getenv("ROCKETNET_BACKEND_PORT"); text != "" {
		port, err := strconv.Atoi(text)
		if err != nil || port < 1 || port > 65535 {
			return nil, fmt.Errorf("ROCKETNET_BACKEND_PORT is not 1 to 65535: %q", text)
		}
		config.port = port
	}

	if text := getenv("ROCKETNET_BACKEND_PRIVATE_KEY"); text != "" {
		key, err := base64.StdEncoding.DecodeString(text)
		if err != nil || len(key) != ed25519.PrivateKeySize {
			return nil, errors.New("ROCKETNET_BACKEND_PRIVATE_KEY is not the base64 of a 64 byte Ed25519 secret key (keygen prints one)")
		}
		config.backendPrivateKey = ed25519.PrivateKey(key)
	} else {
		config.backendPrivateKey = ed25519.PrivateKey(devBackendPrivateKey)
		config.developmentKey = true
	}
	// a 64 byte Ed25519 secret key is its seed then its public key; refuse one whose halves disagree
	if !ed25519.NewKeyFromSeed(config.backendPrivateKey.Seed()).Equal(config.backendPrivateKey) {
		return nil, errors.New("ROCKETNET_BACKEND_PRIVATE_KEY: the public half does not match the seed")
	}

	if text := getenv("ROCKETNET_BACKEND_PROTOCOL_ID"); text != "" {
		protocolID, err := strconv.ParseUint(text, 0, 64)
		if err != nil || protocolID == 0 {
			return nil, fmt.Errorf("ROCKETNET_BACKEND_PROTOCOL_ID is not a nonzero 64 bit id: %q", text)
		}
		config.protocolID = protocolID
	}

	if text := getenv("ROCKETNET_BACKEND_SERVER_ADDRESSES"); text != "" {
		config.serverAddresses = strings.Split(text, ",")
	}
	if err := writeServerAddresses(&signedConnectData{}, config.serverAddresses); err != nil {
		return nil, fmt.Errorf("ROCKETNET_BACKEND_SERVER_ADDRESSES: %w", err)
	}

	// each server makes its own keypair at create; the backend needs its public half
	text := getenv("ROCKETNET_BACKEND_SERVER_PUBLIC_KEY")
	key, err := base64.StdEncoding.DecodeString(text)
	if text == "" || err != nil || len(key) != keyBytes {
		return nil, errors.New("ROCKETNET_BACKEND_SERVER_PUBLIC_KEY is not the base64 of the server's 32 byte public key (example_backend_server prints it)")
	}
	copy(config.serverPublicKey[:], key)

	if text := getenv("ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS"); text != "" {
		seconds, err := strconv.ParseUint(text, 10, 32)
		if err != nil || seconds == 0 {
			return nil, fmt.Errorf("ROCKETNET_BACKEND_TOKEN_EXPIRE_SECONDS is not a positive number of seconds: %q", text)
		}
		config.expireSeconds = seconds
	}
	if text := getenv("ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS"); text != "" {
		seconds, err := strconv.ParseInt(text, 10, 32)
		if err != nil || seconds <= 0 {
			return nil, fmt.Errorf("ROCKETNET_BACKEND_TOKEN_TIMEOUT_SECONDS is not a positive number of seconds: %q", text)
		}
		config.timeoutSeconds = int32(seconds)
	}

	return config, nil
}

var stderrLogger = log.New(os.Stderr, "rocketnet-backend: ", log.LstdFlags)

func matchHandler(config *config) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		clientID, err := strconv.ParseUint(r.PathValue("clientID"), 10, 64)
		if err != nil {
			http.Error(w, "the client id is not a decimal 64 bit number", http.StatusBadRequest)
			return
		}
		token, err := generateConnectToken(config, clientID, uint64(time.Now().Unix()), rand.Reader, sealAnonymous)
		if err != nil {
			stderrLogger.Printf("failed to generate a connect token: %v", err)
			http.Error(w, "failed to generate a connect token", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		// a token is for one client, once: never cached
		w.Header().Set("Cache-Control", "no-store")
		io.WriteString(w, base64.StdEncoding.EncodeToString(token))
		stderrLogger.Printf("matched client %.16x to %s", clientID, strings.Join(config.serverAddresses, ","))
	}
}

// writeBackendKeys emits the environment used by the backend and its servers.
// The caller supplies entropy so the unit fixture can use a known signing seed.
func writeBackendKeys(output io.Writer, random io.Reader) error {
	publicKey, privateKey, err := ed25519.GenerateKey(random)
	if err != nil {
		return err
	}
	defer clear(privateKey)
	_, err = fmt.Fprintf(output, "ROCKETNET_SERVER_BACKEND_PUBLIC_KEY=%s\nROCKETNET_BACKEND_PRIVATE_KEY=%s\n",
		base64.StdEncoding.EncodeToString(publicKey), base64.StdEncoding.EncodeToString(privateKey))
	return err
}

func main() {
	mint := flag.String("mint", "", "print one connect token for this client id and exit, instead of serving")
	keygen := flag.Bool("keygen", false, "print a fresh backend signing keypair as environment assignments and exit")
	flag.Parse()
	mintSet := false
	flag.Visit(func(f *flag.Flag) {
		if f.Name == "mint" {
			mintSet = true
		}
	})
	if flag.NArg() != 0 || (*keygen && mintSet) {
		stderrLogger.Fatal("usage: rocketnet-backend [--keygen | --mint <client id>]")
	}
	if *keygen {
		if err := writeBackendKeys(os.Stdout, rand.Reader); err != nil {
			stderrLogger.Fatalf("key generation failed: %v", err)
		}
		return
	}

	config, err := readConfig(os.Getenv)
	if err != nil {
		stderrLogger.Fatalf("error: %v", err)
	}
	if config.developmentKey {
		stderrLogger.Printf("WARNING: using the published development backend key. DEVELOPMENT ONLY: anyone can mint tokens for a server that accepts it. Set ROCKETNET_BACKEND_PRIVATE_KEY to deploy.")
	}

	if *mint != "" {
		clientID, err := strconv.ParseUint(*mint, 10, 64)
		if err != nil {
			stderrLogger.Fatalf("error: --mint takes a decimal client id, not %q", *mint)
		}
		token, err := generateConnectToken(config, clientID, uint64(time.Now().Unix()), rand.Reader, sealAnonymous)
		if err != nil {
			stderrLogger.Fatalf("error: %v", err)
		}
		fmt.Println(base64.StdEncoding.EncodeToString(token))
		return
	}

	router := http.NewServeMux()
	router.HandleFunc("GET /match/{clientID}", matchHandler(config))

	stderrLogger.Printf("started backend on port %d, minting tokens for %s", config.port, strings.Join(config.serverAddresses, ","))
	err = http.ListenAndServe(":"+strconv.Itoa(config.port), router)
	if err != nil {
		stderrLogger.Fatalf("error: %v", err)
	}
}
