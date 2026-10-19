# Full-system big-endian run

The nightly job `.github/workflows/nightly-bigendian-system.yml` is this procedure. A hand run uses the same steps. The suite inside the guest is functional: it opens sockets and starts real drivers.

## What the image is, and how it is verified

The guest disk is the Ubuntu 24.04 server cloud image for s390x:

`https://cloud-images.ubuntu.com/releases/noble/release/ubuntu-24.04-server-cloudimg-s390x.img`

The same directory publishes `SHA256SUMS` and `SHA256SUMS.gpg`. The signature key is the UEC Image Automatic Signing Key, fingerprint `D2EB 4462 6FDD C30B 513D 5BB7 1A5D 6C4C 7DB8 7C81`. Fetch that key, refuse any other fingerprint, verify `SHA256SUMS.gpg` over `SHA256SUMS`, then check the image with `sha256sum -c`. An image that fails either check is not booted.

## How the guest is built

Create an 8G qcow2 overlay whose backing file is the verified image, so the verified file stays unmodified. Build a NoCloud seed with `cloud-localds`: one user, one ephemeral ssh key generated for the run, and root-filesystem growth. Boot it with `qemu-system-s390x`:

- machine `s390-ccw-virtio`, accelerator TCG, 4G RAM, 4 vCPUs
- virtio-blk for the overlay (boot index 1) and for the seed
- virtio-net, user mode, with a host forward onto the guest ssh port

Copy the source in over ssh. On the guest, install a compiler, cmake, pkg-config, libbpf, libxdp, and xxd. Configure a Release build with `-ffp-contract=off` on the C flags and the C++ flags. The CMake targets apply that same floor. Build the `test` and `functional` targets.

Before the suite, `readelf -h bin/test` must report a big-endian IBM S/390 binary. The functional binary is checked the same way.

## How long a run takes

One TCG run at 4 vCPUs and 4G RAM:

- ssh answers in about 2 minutes
- guest package install takes about 5 minutes
- configure takes 35 seconds
- the `test` and `functional` build takes 450 seconds
- `./bin/test` takes 89 seconds
- `./bin/functional` on the auto ladder takes 149 seconds
- `./bin/functional` with the portable transport pin takes 126 seconds

The functional suite kills a single test at 120 seconds. A socket test that never receives its reply ends there and the suite continues. It does not hold the job open.

## How to run it by hand

On a Linux host with `qemu-system-s390x`, `cloud-localds`, and `qemu-img`, repeat the verification, the boot, and the in-guest build above. Then, in the guest, from the source tree:

```
uname -sn
./bin/test
./bin/functional
ROCKETNET_SERVER_TRANSPORT=portable ROCKETNET_CLIENT_TRANSPORT=portable ./bin/functional
```

The two rungs the guest runs are the auto ladder and the portable pin. The auto ladder refuses xdp in this guest (no attached data plane) and falls through. The xdp pin test asserts that refusal. It does not attach xdp.
