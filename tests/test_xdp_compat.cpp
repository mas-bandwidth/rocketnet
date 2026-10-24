/* RocketNet. Copyright 2026 Más Bandwidth LLC. Licensed under AGPL-3.0. */

// Legacy adapter contract. Fake libbpf calls record the actual attachment
// flags and expected fd; this executable never loads or attaches a program.
#include "rocketnet_xdp_compat.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct bpf_object {};
struct bpf_program {};
static bpf_object object;
static bpf_program program;
static bool found = true;
static int load_result = 0;
static int load_calls = 0;
static bpf_prog_type program_type = BPF_PROG_TYPE_UNSPEC;
static int query_result = 0;
static int called_index = 0;
static int called_fd = 0;
static __u32 called_flags = 0;
static int expected_fd = 0;
static int attach_result = 0;
static bpf_load_program_attr probe = {};

extern "C" bpf_program * bpf_object__find_program_by_name( const bpf_object * obj, const char * name )
{
    return obj == &object && found && strcmp(name, "server_xdp_filter") == 0 ? &program : NULL;
}
extern "C" void bpf_program__set_type( bpf_program *, bpf_prog_type type ) { program_type = type; }
extern "C" int bpf_object__load( bpf_object * ) { ++load_calls; return load_result; }
extern "C" int bpf_program__fd( const bpf_program * ) { return 42; }
extern "C" int bpf_get_link_xdp_info( int index, xdp_link_info * info, size_t info_size, __u32 flags )
{
    called_index = index; called_flags = flags;
    if ( info_size != sizeof(*info) ) return -EINVAL;
    // the multi-mode shape: no single id, a native and a generic program
    info->prog_id = 0; info->drv_prog_id = 73; info->skb_prog_id = 74; info->hw_prog_id = 0;
    return query_result;
}
extern "C" int bpf_set_link_xdp_fd( int index, int fd, __u32 flags )
{
    called_index = index; called_fd = fd; called_flags = flags; return attach_result;
}
extern "C" int bpf_set_link_xdp_fd_opts( int index, int fd, __u32 flags, const bpf_xdp_set_link_opts * opts )
{
    if ( !opts || opts->sz != sizeof(*opts) ) return -EINVAL;
    expected_fd = opts->old_fd;
    return bpf_set_link_xdp_fd(index, fd, flags);
}
extern "C" int bpf_load_program_xattr( const bpf_load_program_attr * attrs, char *, size_t )
{
    probe = *attrs; return 17;
}

#define CHECK(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); return 1; } } while (0)
int main()
{
    found = false;
    CHECK(rocketnet_xdp_program_open(&object) == NULL && load_calls == 0);
    found = true; load_result = -EPERM;
    CHECK(rocketnet_xdp_program_open(&object) == NULL && load_calls == 1);
    load_result = 0;
    CHECK(rocketnet_xdp_program_open(&object) == &program && load_calls == 2);
    CHECK(program_type == BPF_PROG_TYPE_XDP);
    rocketnet_xdp_attached_t attached = {};
    query_result = -ENODEV;
    CHECK(rocketnet_xdp_query_attached(7, &attached) == -ENODEV);
    CHECK(called_index == 7 && called_flags == 0);
    query_result = 0;
    CHECK(rocketnet_xdp_query_attached(7, &attached) == 0);
    CHECK(attached.prog_id == 0 && attached.drv_prog_id == 73 && attached.skb_prog_id == 74 && attached.hw_prog_id == 0);
    for (int mode = 0; mode < 2; ++mode)
    {
        const bool skb = mode != 0;
        const __u32 flags = skb ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
        attach_result = -EEXIST;
        CHECK(rocketnet_xdp_program_attach(&program, 7, skb) == -EEXIST);
        CHECK(called_index == 7 && called_fd == 42 && called_flags == (flags | XDP_FLAGS_UPDATE_IF_NOEXIST));
        attach_result = 0;
        CHECK(rocketnet_xdp_program_attach(&program, 7, skb) == 0);
        attach_result = -EEXIST;
        CHECK(rocketnet_xdp_program_detach(&program, 7, skb) == -EEXIST);
        CHECK(called_index == 7 && called_fd == -1 && expected_fd == 42 && called_flags == (flags | XDP_FLAGS_REPLACE));
    }
    const bpf_insn instructions[2] = {};
    CHECK(rocketnet_xdp_probe_program(instructions, 2) == 17);
    CHECK(probe.prog_type == BPF_PROG_TYPE_XDP && probe.insns == instructions && probe.insns_cnt == 2);
    CHECK(strcmp(probe.name, "rocketnet_pf") == 0 && strcmp(probe.license, "GPL") == 0);
    rocketnet_xdp_program_close(&program); // bpf_object retains ownership
    puts("Legacy XDP adapter contracts passed (no kernel operations)");
    return 0;
}
