# Can RocketNet use XDP or AF_XDP on AWS GameLift managed fleets

This note answers five questions about running RocketNet's XDP/AF_XDP server
transport on AWS GameLift managed EC2 fleets. A managed EC2 fleet is a set of
Amazon EC2 instances that Amazon GameLift Servers owns and operates based on
your configuration (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-intro-managed.html).

## 1. What user the game server process runs as

On a Linux managed fleet, installation runs as a limited "run-as user" that
has full rights only to the directory where build files are installed
(`/local/game`), and any action requiring administrator permissions must
declare admin access with `sudo`
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html).

The install-script documentation does not specify which user runs the launched
game server process. RocketNet's XDP transport requires an effective user ID of
zero, so use XDP only when the deployed process has that permission. Otherwise,
select a transport that does not require it, such as `gso` or `io_uring`.

## 2. Whether the build's install script runs as root and can setcap

The install script runs once during fleet creation to establish the AMI used
on all future fleet instances (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html). For Linux
fleets it runs as the limited run-as user, not as root, and uses `sudo` when
admin permissions are required (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html). Because
`sudo` is available to an install script for administrator actions. However,
the GameLift documentation does not specify whether file capabilities survive
when the runtime starts the server. Do not rely on `setcap` alone to enable
XDP; confirm the launched process has the required privileges before selecting
that transport.

The kernel applies file capabilities on `execve` only when the mount allows
suid execution, so a `nosuid` mount defeats them
(https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/security/commoncap.c), and with `no_new_privs` set, `execve` does not add file
capabilities to the permitted set (https://docs.kernel.org/userspace-api/no_new_privs.html);
GameLift's launch documentation does not describe the mount flags or
`no_new_privs` state for the launched process. Treat file capabilities as
insufficient unless the deployed process's effective privileges have been
verified.

## 3. ENA and XDP

The ENA driver supports native XDP
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).
The XDP DROP and TX actions were
implemented in ENA r2.2.0, and the XDP_REDIRECT implementation was added in
r2.3.0 (https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/RELEASENOTES.md).
The supported XDP actions are XDP_PASS, XDP_DROP, XDP_TX, XDP_REDIRECT and
XDP_ABORTED (https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).

Without multi-buffer, the maximum allowed MTU is ENA_XDP_MAX_SINGLE_FRAME_SIZE;
with 4 KB pages on kernel 6.17 this is 3502, and multi-buffer
(`BPF_F_XDP_HAS_FRAGS`) removes the MTU limit
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).
The number of ENA queues exposed per ENI is `MIN(MAX_NUM_QUEUES_PER_ENI, NUM_OF_VCPUS)`, where the maximum is 8 for most instance types and up to 32
for network-accelerated instances
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/ENA_Linux_Best_Practices.rst).

GameLift managed fleets run on Amazon EC2 instances and support a wide range of
instance types with x86_64 or Arm (Graviton) compute architectures
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-compute.html).
All Nitro-based EC2 instances use ENA for enhanced networking
(https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/enhanced-networking.html),
so the ENA driver is present on the Nitro instance families GameLift offers.
The exact set of instance types offered is chosen in the GameLift console and
varies by location (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-compute.html).

## 4. ENA and AF_XDP

ENA provides native AF_XDP support with zero-copy
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).
AF_XDP requires an XDP program to be loaded on the interface, and the MTU
requirements that apply to XDP also apply to AF_XDP
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).
The ENA AF_XDP UMEM supports only 4 KB buffers, with a minimum buffer size of
2 KB and a mandatory XDP_PACKET_HEADROOM of 256 bytes
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst).
AF_XDP zero-copy support was added in ENA r2.7.0, marked experimental in
r2.7.3, and re-enabled in r2.13.0
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/RELEASENOTES.md).
Binding an AF_XDP socket to an interface and updating socket options requires
the CAP_NET_RAW capability
(https://docs.kernel.org/networking/af_xdp.html).

GameLift managed EC2 fleets with server SDK 5+ run Amazon Linux 2023 (kernel
6.1, x86_64 or arm64) or Amazon Linux 2 (kernel 5.10)
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/reference-ec2-ami-version-history.html).
The ENA driver version can vary with the selected AMI. Inspect the active
version with `ethtool -i eth0` or `/sys/module/ena/version`, and check the
kernel with `uname -r`.

## 5. Other routes: GameLift Anywhere and container fleets

GameLift Anywhere fleets are computes you supply and manage: you deploy the
game server software and launch the game server processes yourself, and
GameLift only sets up the fleet resource
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-intro-anywhere.html).
Because you control the compute and the launch, you can run the game server as
root or grant it the capabilities the XDP transport needs, which gives a different
answer from managed fleets for the user and capability questions.

GameLift container fleets run your game server from a container image you
author (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/containers-prepare-images.html).
The documented image template creates a non-root `gamelift` user with
passwordless `sudo` and runs the game under it
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/containers-prepare-images.html).
Because you build the image, you can instead run the entrypoint as root or set
file capabilities in the image, so container fleets also put the user and
capability decision in your hands; host-level remote access to the containers
uses `sudo docker` on the underlying fleet instance
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/containers-remote-access.html).

## Recommendation

ENA supports native XDP and AF_XDP zero-copy
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst),
and the managed fleet OS (Amazon Linux 2023, kernel 6.1) is new enough for both
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/reference-ec2-ami-version-history.html).

The XDP transport requires euid 0. File capabilities alone do not pass its
pre-flight. It also requires the provisioned boot tier (`isolcpus`, `nohz_full`
and `irqaffinity` on the kernel command line), IRQ pinning, SMT off and a
matching NIC channel count. GameLift documents the install-script user's
permissions but not the launched server process's effective user. Use `gso`
or `io_uring` on managed fleets unless the runtime process is configured and
verified to meet XDP's privilege and host requirements. Anywhere and container
fleets let the operator control the compute and launch configuration.
