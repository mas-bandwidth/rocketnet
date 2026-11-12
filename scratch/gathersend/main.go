// gathersend: the X2 question, measured. One send worker sends a bank of
// packets to many destinations as UDP_SEGMENT runs, two ways:
//
//	contiguous  each destination's run is one buffer (the contiguous-claim layout)
//	scattered   each destination's run is an iovec per packet, the packets
//	            spread through a bank staged in interleaved order (the X2
//	            gather over scattered slots)
//
// It reports packets per second and the worker thread's CPU time per packet
// (RUSAGE_THREAD), the cost a per-queue worker pays. Linux only.
//
//	gathersend -mode scattered -dst 10.0.0.2 -clients 1000 -run 8 -seconds 10
//	gathersend -sink -clients 1000          bind the destination ports, never read
package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

const (
	solUDP     = 17
	udpSegment = 103
	stride     = 1472
)

func main() {
	mode := flag.String("mode", "scattered", "contiguous or scattered")
	dst := flag.String("dst", "127.0.0.1", "destination IPv4")
	base := flag.Int("port", 30000, "first destination port")
	clients := flag.Int("clients", 1000, "destinations")
	run := flag.Int("run", 8, "packets per destination per bank (one run)")
	size := flag.Int("size", 1200, "packet bytes")
	seconds := flag.Float64("seconds", 10, "duration")
	sink := flag.Bool("sink", false, "bind the destination ports and wait")
	flag.Parse()

	if *sink {
		var socks []*net.UDPConn
		for i := 0; i < *clients; i++ {
			c, err := net.ListenUDP("udp4", &net.UDPAddr{Port: *base + i})
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
				os.Exit(1)
			}
			socks = append(socks, c)
		}
		fmt.Printf("SINK ports=%d-%d\n", *base, *base+*clients-1)
		time.Sleep(time.Duration(*seconds * float64(time.Second)))
		_ = socks
		return
	}

	runtime.LockOSThread()

	fd, err := syscall.Socket(syscall.AF_INET, syscall.SOCK_DGRAM, 0)
	if err != nil {
		panic(err)
	}
	_ = syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_SNDBUF, 64<<20)

	ip := net.ParseIP(*dst).To4()
	addrs := make([]syscall.RawSockaddrInet4, *clients)
	for i := range addrs {
		addrs[i].Family = syscall.AF_INET
		p := uint16(*base + i)
		addrs[i].Port = p<<8 | p>>8
		copy(addrs[i].Addr[:], ip)
	}

	// the bank: clients * run slots of stride bytes. contiguous: client c's run
	// is slots c*run .. c*run+run-1, one buffer. scattered: staged
	// interleaved, so client c's k-th packet is slot k*clients + c.
	slots := *clients * *run
	bank := make([]byte, slots*stride)
	for i := range bank {
		bank[i] = byte(i)
	}

	contiguous := *mode == "contiguous"
	if !contiguous && *mode != "scattered" {
		fmt.Fprintln(os.Stderr, "mode is contiguous or scattered")
		os.Exit(2)
	}
	if contiguous {
		// one destination's run is one buffer of run*size bytes
		bank = make([]byte, *clients**run**size)
	}

	iov := make([]syscall.Iovec, *run)
	cmsg := make([]byte, syscall.CmsgSpace(2))
	h := (*syscall.Cmsghdr)(unsafe.Pointer(&cmsg[0]))
	h.Level = solUDP
	h.Type = udpSegment
	h.SetLen(syscall.CmsgLen(2))
	*(*uint16)(unsafe.Pointer(&cmsg[syscall.CmsgLen(0)])) = uint16(*size)

	var msg syscall.Msghdr
	msg.Namelen = syscall.SizeofSockaddrInet4
	msg.Control = &cmsg[0]
	msg.SetControllen(len(cmsg))

	var before syscall.Rusage
	_ = syscall.Getrusage(1, &before) // RUSAGE_THREAD
	start := time.Now()
	deadline := start.Add(time.Duration(*seconds * float64(time.Second)))

	var packets, calls, errs uint64
	for time.Now().Before(deadline) {
		for c := 0; c < *clients; c++ {
			if contiguous {
				iov[0].Base = &bank[c**run**size]
				iov[0].SetLen(*run * *size)
				msg.Iov = &iov[0]
				msg.Iovlen = 1
			} else {
				for k := 0; k < *run; k++ {
					iov[k].Base = &bank[(k**clients+c)*stride]
					iov[k].SetLen(*size)
				}
				msg.Iov = &iov[0]
				msg.Iovlen = uint64(*run)
			}
			msg.Name = (*byte)(unsafe.Pointer(&addrs[c]))
			_, _, e := syscall.Syscall(syscall.SYS_SENDMSG, uintptr(fd), uintptr(unsafe.Pointer(&msg)), 0)
			calls++
			if e != 0 {
				errs++
				continue
			}
			packets += uint64(*run)
		}
	}
	elapsed := time.Since(start).Seconds()
	var after syscall.Rusage
	_ = syscall.Getrusage(1, &after)
	cpu := float64(after.Utime.Nano()-before.Utime.Nano()+after.Stime.Nano()-before.Stime.Nano()) / 1e9
	sys := float64(after.Stime.Nano()-before.Stime.Nano()) / 1e9

	fmt.Printf("ROW mode=%s clients=%d run=%d size=%d seconds=%.2f calls=%d errors=%d packets=%d pps=%.0f gbps=%.2f cpu_us_per_packet=%.3f sys_us_per_packet=%.3f\n",
		*mode, *clients, *run, *size, elapsed, calls, errs, packets, float64(packets)/elapsed,
		float64(packets)*float64(*size)*8/elapsed/1e9, cpu*1e6/float64(packets), sys*1e6/float64(packets))
}
