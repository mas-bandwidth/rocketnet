# RocketNet transports on Amazon GameLift Servers

Use `gso` as the starting Linux transport for managed fleets. Evaluate
`io_uring` only after verifying its build and kernel requirements. RocketNet's
`xdp` transport requires effective UID 0 and host provisioning that a managed
fleet or container image alone does not establish. This is a deployment
assessment, not a recorded GameLift qualification run.

AWS and ENA references below were checked on 2026-10-31. Fleet images and
available instance types change; inspect the actual deployed instance.

## Managed EC2 fleets: installation and runtime permissions

AWS runs the build install script during fleet creation to establish the AMI
used by later instances. It does not run on every instance startup, and it
may run more than once during fleet creation. On Linux the installation user
has limited access outside `/local/game`; administrator actions must request
`sudo`. Make installation idempotent.
[Install-script documentation](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html).

Those install-script permissions do not establish the effective user or
capabilities of the launched game server. Check the runtime process. In the
current RocketNet source, file capabilities alone cannot satisfy XDP's
explicit `geteuid() == 0` pre-flight check
(`sdk/rocketnet_server_socket_linux.cpp`). Even a root process must satisfy
the remaining host checks.

## ENA, the kernel, and AF_XDP

The ENA driver documents native XDP and AF_XDP zero-copy support. Its AF_XDP
path requires an attached XDP program, has MTU restrictions, and supports
4 KiB UMEM buffers. These features describe the driver implementation;
they do not prove that a particular fleet's installed driver and kernel
provide them. [ENA driver reference](https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).

AWS's AMI table currently lists Amazon Linux 2023 with kernel 6.1 and Amazon
Linux 2 with kernel 5.10 for server SDK 5+, including x86_64 and arm64 images.
Use the table for the selected fleet image rather than assuming all fleets
have the latest image. [GameLift AMI versions](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/reference-ec2-ami-version-history.html).
RocketNet's own supported platform claim is separate; Linux arm64 is not
currently a supported row in [PLATFORMS.md](PLATFORMS.md).

On an allocated instance, inspect the actual interface and kernel:

```sh
uname -r
cat /proc/<server-pid>/status
ethtool -i <interface>
ethtool -l <interface>
```

For XDP qualification, record the loaded driver version, available queues,
UMEM/memlock resources, pre-flight results, and actual bind mode on every
queue. A successful XDP attachment or a sufficiently new kernel version alone
is not evidence of zero-copy operation. Follow [LINUX.md](LINUX.md#6-xdp-and-the-provisioning-script).

## Anywhere fleets

Anywhere uses compute resources you supply and manage. You install the server
and launch its processes; GameLift establishes the fleet resource and connects
to those computes. [Anywhere fleets](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-intro-anywhere.html).

This gives an operator a route to provision the required host, NIC and process
permissions. RocketNet still requires effective UID 0 for XDP; choosing
capabilities instead of root does not pass that check. Qualify the exact
machine and workload before selecting XDP in production.

## Managed container fleets

AWS's image template creates a non-root `gamelift` user with passwordless
`sudo` and launches the game under that user.
[Container image instructions](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/containers-prepare-images.html).
The underlying EC2 instances and container deployment are managed by GameLift;
network access also includes fleet permissions and container port mappings.
[How managed containers work](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/containers-howitworks.html).

Changing the image's user does not establish access to the host NIC, kernel
boot arguments, IRQ affinity, memory-lock limits or the required kernel
operations. Do not treat image ownership, `USER root`, or `setcap` as proof
that RocketNet XDP can run in a managed container. Verify each requirement
against the deployed runtime and the service's supported controls. The
[Linux container requirements](LINUX.md#9-containers) describe what the SDK
actually checks.

## Transport choice

For an ordinary managed Linux deployment, pin `gso` or use `gso,portable` if
an explicit fallback is acceptable. Read `rocketnet_server_transport_name`
after start and retain startup refusals and socket-buffer grants. A server
pin does not select the clients' transport.

Pin `io_uring` only in a build that includes it and on a runtime where its
probe succeeds. An unavailable io_uring name is skipped, and a list containing
only skipped names reverts to auto with a warning; the name query is essential.
See [Transport selection](TRANSPORTS.md) and [Linux io_uring requirements](LINUX.md#5-io_uring).

XDP additionally requires the provisioned CPU-isolation boot tier, IRQ
placement, SMT and channel configuration, interface access, and memlock
headroom. Follow the provision-and-verify procedure in [DEPLOYING.md](DEPLOYING.md).
A managed-fleet permission to install software does not establish those
requirements. Size the workload with [MEMORY.md](MEMORY.md), including the
current GSO/io_uring staging limitations, and qualify it on the target fleet.
