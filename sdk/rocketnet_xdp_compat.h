/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

#ifndef ROCKETNET_XDP_COMPAT_H
#define ROCKETNET_XDP_COMPAT_H

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>

#ifndef ROCKETNET_XDP_LEGACY_LIBBPF
#define ROCKETNET_XDP_LEGACY_LIBBPF 0
#endif

#if ROCKETNET_XDP_LEGACY_LIBBPF
#include <bpf/xsk.h>
typedef struct bpf_program rocketnet_xdp_program_t;
#else
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
typedef struct xdp_program rocketnet_xdp_program_t;
#endif

static inline int rocketnet_xdp_probe_program( const struct bpf_insn * instructions, size_t count )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    struct bpf_load_program_attr attributes = {};
    attributes.prog_type = BPF_PROG_TYPE_XDP;
    attributes.name = "rocketnet_pf";
    attributes.license = "GPL";
    attributes.insns = instructions;
    attributes.insns_cnt = count;
    return bpf_load_program_xattr( &attributes, NULL, 0 );
#else
    return bpf_prog_load( BPF_PROG_TYPE_XDP, "rocketnet_pf", "GPL", instructions, count, NULL );
#endif
}

// The programs attached to an interface, one id per attach mode. prog_id is
// the single-mode id and reads 0 when programs are attached in more than one
// mode (XDP_ATTACHED_MULTI); the per-mode ids are filled in either case.
typedef struct rocketnet_xdp_attached_t
{
    unsigned int prog_id;
    unsigned int drv_prog_id;
    unsigned int skb_prog_id;
    unsigned int hw_prog_id;
} rocketnet_xdp_attached_t;

static inline int rocketnet_xdp_query_attached( int interface_index, rocketnet_xdp_attached_t * attached )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    struct xdp_link_info info = {};
    const int result = bpf_get_link_xdp_info( interface_index, &info, sizeof( info ), 0 );
#else
    struct bpf_xdp_query_opts info = {};
    info.sz = sizeof( info );
    const int result = bpf_xdp_query( interface_index, 0, &info );
#endif
    attached->prog_id = info.prog_id;
    attached->drv_prog_id = info.drv_prog_id;
    attached->skb_prog_id = info.skb_prog_id;
    attached->hw_prog_id = info.hw_prog_id;
    return result;
}

// The caller owns object on every outcome. Legacy libbpf loads the single
// program directly; newer systems retain libxdp's loader and attachment.
static inline rocketnet_xdp_program_t * rocketnet_xdp_program_open( struct bpf_object * object )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    struct bpf_program * program = bpf_object__find_program_by_name( object, "server_xdp_filter" );
    if ( !program )
        return NULL;
    bpf_program__set_type( program, BPF_PROG_TYPE_XDP );
    if ( bpf_object__load( object ) != 0 )
        return NULL;
    return program;
#else
    struct xdp_program * program = xdp_program__from_bpf_obj( object, "server_xdp" );
    return libxdp_get_error( program ) ? NULL : program;
#endif
}

static inline int rocketnet_xdp_program_attach( rocketnet_xdp_program_t * program, int interface_index, bool skb )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    const __u32 flags = ( skb ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE ) | XDP_FLAGS_UPDATE_IF_NOEXIST;
    return bpf_set_link_xdp_fd( interface_index, bpf_program__fd( program ), flags );
#else
    return xdp_program__attach( program, interface_index, skb ? XDP_MODE_SKB : XDP_MODE_NATIVE, 0 );
#endif
}

static inline int rocketnet_xdp_program_detach( rocketnet_xdp_program_t * program, int interface_index, bool skb )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    // Compare-and-detach: never remove a program that replaced ours after
    // attachment. The kernel checks old_fd atomically with the detach.
    struct bpf_xdp_set_link_opts options = {};
    options.sz = sizeof( options );
    options.old_fd = bpf_program__fd( program );
    const __u32 flags = ( skb ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE ) | XDP_FLAGS_REPLACE;
    return bpf_set_link_xdp_fd_opts( interface_index, -1, flags, &options );
#else
    return xdp_program__detach( program, interface_index, skb ? XDP_MODE_SKB : XDP_MODE_NATIVE, 0 );
#endif
}

static inline void rocketnet_xdp_program_close( rocketnet_xdp_program_t * program )
{
#if ROCKETNET_XDP_LEGACY_LIBBPF
    (void) program; // owned by the bpf_object; the caller closes it once
#else
    xdp_program__close( program );
#endif
}

#endif
