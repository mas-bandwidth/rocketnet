/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

#ifndef CLIENT_BACKEND_BPF_H
#define CLIENT_BACKEND_BPF_H

#include "client_backend.h"

#ifdef __linux__
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#endif // #ifdef __linux__

struct bpf_t
{
#ifdef __linux__
    int interface_index;
    struct xdp_program * program;
    bool attached_native;
    bool attached_skb;
    int config_fd;
    int state_fd;
    int buyer_fd;
#endif // #ifdef __linux__
};

bool bpf_init( struct bpf_t * bpf, uint32_t public_address );

void bpf_shutdown( struct bpf_t * bpf );

#endif // #ifndef CLIENT_BACKEND_BPF_H
