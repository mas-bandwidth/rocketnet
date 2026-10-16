/*
    RocketNet. Copyright 2026 Más Bandwidth LLC.
    Licensed under the GNU Affero General Public License v3.0
*/

// The host shim for libbpf's <bpf/bpf_helpers.h>: the map declaration
// macros with libbpf's own expansions, so the filter's map structs compile
// unchanged; SEC() expanding to nothing (Mach-O section names have another
// syntax, and the host build has no loader to read them); and the two
// helpers the filter calls, declared here and defined as host functions in
// server_xdp_host.c after the filter source, where the maps are in scope.

#pragma once

#ifndef ROCKETNET_XDP_HOST_BPF_HELPERS_H
#define ROCKETNET_XDP_HOST_BPF_HELPERS_H

#include "linux/bpf.h"

#define __uint(name, val) int (*name)[val]
#define __type(name, val) typeof(val) *name
#define __array(name, val) typeof(val) *name[]

#define SEC(name)

enum libbpf_pin_type
{
    LIBBPF_PIN_NONE,
    LIBBPF_PIN_BY_NAME,
};

static void * bpf_map_lookup_elem( void * map, const void * key );

static long bpf_redirect_map( void * map, __u64 key, __u64 flags );

#endif // #ifndef ROCKETNET_XDP_HOST_BPF_HELPERS_H
