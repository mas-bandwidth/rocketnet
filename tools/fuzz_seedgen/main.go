// seedgen writes the seed corpora for two fuzz targets: rocketnet_fuzz_xdp_filter
// (tests/fuzz/corpus_xdp_filter) and rocketnet_fuzz_endpoint
// (tests/fuzz/corpus_endpoint).
// usage: go run . <xdp_filter_out_dir> <endpoint_out_dir>
package main

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
)

var filter = []byte{0xb7, 0x59, 0x01, 0x00}

type ptype struct {
	name  string
	t     byte
	sizes []int
}

var types = []ptype{
	{"request", 0, []int{710}},
	{"response", 3, []int{50}},
	{"keepalive", 4, []int{41, 60}},
	{"payload", 5, []int{34, 200, 1416 + 17 + 16}},
	{"disconnect", 6, []int{33}},
	{"ack", 8, []int{33, 64}},
	{"denied", 1, []int{34}},
	{"challenge", 2, []int{66}},
	{"cookie", 7, []int{38}},
}

func payload(t byte, n int) []byte {
	p := make([]byte, n)
	p[0] = t
	p[1] = 0
	copy(p[2:6], filter)
	for i := 6; i < n; i++ {
		p[i] = byte(i * 13)
	}
	return p
}

func put16(b []byte, v int) { binary.BigEndian.PutUint16(b, uint16(v)) }

// a whole frame, as the harness's structured mode builds it
func frame(ipv6 bool, p []byte) []byte {
	f := make([]byte, 14)
	f[0] = 2
	f[6] = 2
	f[11] = 1
	if ipv6 {
		put16(f[12:], 0x86dd)
		ip := make([]byte, 40)
		ip[0] = 0x60
		put16(ip[4:], 8+len(p))
		ip[6] = 17
		ip[7] = 64
		copy(ip[8:], []byte{0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10})
		copy(ip[24:], []byte{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x0a})
		f = append(f, ip...)
	} else {
		put16(f[12:], 0x0800)
		ip := make([]byte, 20)
		ip[0] = 0x45
		put16(ip[2:], 20+8+len(p))
		ip[8] = 64
		ip[9] = 17
		copy(ip[12:], []byte{198, 51, 100, 10})
		copy(ip[16:], []byte{203, 0, 113, 10})
		f = append(f, ip...)
	}
	u := make([]byte, 8)
	put16(u[0:], 50000)
	put16(u[2:], 40000)
	put16(u[4:], 8+len(p))
	f = append(f, u...)
	return append(f, p...)
}

func write(dir, name string, b []byte) {
	if err := os.WriteFile(filepath.Join(dir, name), b, 0o644); err != nil {
		panic(err)
	}
}

func xdp(dir string) {
	n := 0
	for _, pt := range types {
		for _, sz := range pt.sizes {
			p := payload(pt.t, sz)
			for _, v6 := range []bool{false, true} {
				fam := byte(0)
				tag := "v4"
				if v6 {
					fam = 0x80
					tag = "v6"
				}
				// structured, socket populated, no replay entry
				h := []byte{0x21 | fam, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0}
				write(dir, fmt.Sprintf("s_%s_%s_%d", pt.name, tag, sz), append(h, p...))
				// structured, with a replay entry at high water 1000
				h2 := []byte{0x61 | fam, 3, 0, 0xe8, 0x03, 0, 0, 0, 0, 0, 0}
				write(dir, fmt.Sprintf("r_%s_%s_%d", pt.name, tag, sz), append(h2, p...))
				// raw, the same frame
				hr := []byte{0x20, 3, 0, 0, 0, 0, 0}
				write(dir, fmt.Sprintf("w_%s_%s_%d", pt.name, tag, sz), append(hr, frame(v6, p)...))
				n += 3
			}
		}
	}
	fmt.Println("xdp seeds", n)
}

// endpoint scripts
func endpoint(dir string) {
	type op []byte
	send := func(rel, ch byte, size int) op {
		// odd size_bits: 4 + (bits>>1)%2048; even: scaled to max_message_bytes
		b := []byte{1, rel, ch, 0, 0}
		binary.LittleEndian.PutUint16(b[3:], uint16(size))
		return b
	}
	write2 := op{2}
	deliver := func(idx, mut byte) op { return op{3, idx, mut, 0, 0, 0} }
	update := op{4, 10, 0}
	n := 0
	for cfg := byte(0); cfg < 4; cfg++ {
		for rel := byte(0); rel < 8; rel++ {
			for _, size := range []int{2*20 + 1, 2*1000 + 1, 0x4000, 0x8000, 0xffff &^ 1} {
				s := []byte{cfg}
				for k := 0; k < 3; k++ {
					s = append(s, send(rel, byte(k), size)...)
				}
				for k := 0; k < 4; k++ {
					s = append(s, write2...)
					for j := 0; j < 12; j++ {
						s = append(s, deliver(byte(j%3), 0)...)
					}
					s = append(s, update...)
				}
				// a reordered pass: the index bytes run 200 down to 189 and the
				// harness takes index % (packets in flight), so the delivery
				// order is a rotation that depends on how many are in flight,
				// not a fixed last-first
				s = append(s, send(rel, 1, size)...)
				s = append(s, write2...)
				for j := 0; j < 12; j++ {
					s = append(s, deliver(byte(200-j), 0)...)
				}
				s = append(s, update...)
				// B sends and gets acks
				s = append(s, 6, rel, 100, 0)
				s = append(s, 5, 1, 0, 0, 0, 0, 0, 0, 0)
				s = append(s, update...)
				write(dir, fmt.Sprintf("ep_cfg%d_rel%d_size%d", cfg, rel, size), s)
				n++
			}
		}
	}
	fmt.Println("endpoint seeds", n)
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: go run . <xdp_filter_out_dir> <endpoint_out_dir>")
		os.Exit(2)
	}
	for _, dir := range os.Args[1:] {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			fmt.Fprintln(os.Stderr, "seedgen:", err)
			os.Exit(1)
		}
	}
	xdp(os.Args[1])
	endpoint(os.Args[2])
}
