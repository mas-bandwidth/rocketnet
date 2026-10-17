# Can RocketNet use XDP or AF_XDP on AWS GameLift managed fleets

This note answers five questions about running RocketNet's XDP/AF_XDP server
driver on AWS GameLift managed EC2 fleets. A managed EC2 fleet is a set of
Amazon EC2 instances that Amazon GameLift Servers owns and operates based on
your configuration (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-intro-managed.html).

## 1. What user the game server process runs as

On a Linux managed fleet, installation runs as a limited "run-as user" that
has full rights only to the directory where build files are installed
(`/local/game`), and any action requiring administrator permissions must
declare admin access with `sudo`
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html).

Whether the launched game server process runs under this same run-as user is
not stated in these sources: the process is launched by the fleet's runtime
configuration, and the user id of that process is not documented. This is
unknown; it would be settled on a real fleet by connecting via SSM remote
access (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-remote-access.html) and running `ps -o user= -p <pid>` or `id`
against the running game server process. Nothing in the sources lets a build
change the user the process runs as.

## 2. Whether the build's install script runs as root and can setcap

The install script runs once during fleet creation to establish the AMI used
on all future fleet instances (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html). For Linux
fleets it runs as the limited run-as user, not as root, and uses `sudo` when
admin permissions are required (https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html). Because
`sudo` is available, an install script can run `sudo setcap
cap_net_admin,cap_bpf,cap_net_raw+ep <server>` on the server binary.

Whether those file capabilities survive into the process GameLift launches is
unknown and not documented in these sources: file capabilities are honored on
`execve` unless the launch path sets `no_new_privs` or the filesystem is
mounted `nosuid`, and GameLift's launch mechanics are not specified here. This
is the deciding unknown; it would be settled by a test on a real managed fleet
that installs with `setcap`, launches, and checks `getcap <server>` and
whether the XDP create succeeds in the launched process.

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
The number of ENA queues exposed per ENI is `MIN(MAX_NUM_QUEUES_PER_ENI,
NUM_OF_VCPUS)`, where the maximum is 8 for most instance types and up to 32
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
2 KB and a mandatory XDP messagePACKET_HEADROOM of 256 bytes
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
The ENA driver version bundled in those AMIs is not pinned by these sources
(unknown); a build would find it at run time with `ethtool -i eth0` or by
reading `/sys/module/ena/version`, and the kernel with `uname -r`.

## 5. Other routes: GameLift Anywhere and container fleets

GameLift Anywhere fleets are computes you supply and manage: you deploy the
game server software and launch the game server processes yourself, and
GameLift only sets up the fleet resource
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-intro-anywhere.html).
Because you control the compute and the launch, you can run the game server as
root or grant it the capabilities the XDP driver needs, which gives a different
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

The deciding fact is whether the game server process GameLift launches on a
managed fleet retains root or the CAP_NET_ADMIN/CAP_BPF/CAP_NET_RAW
capabilities. ENA supports native XDP and AF_XDP zero-copy
(https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/README.rst),
and the managed fleet OS (Amazon Linux 2023, kernel 6.1) is new enough for both
(https://docs.aws.amazon.com/gameliftservers/latest/developerguide/reference-ec2-ami-version-history.html),
so the only open question is capability possession at launch.

(a) If capabilities survive, the XDP driver works on managed fleets: build for
Linux, use `sudo setcap` in the install script, and let the auto ladder grant
xdp; verify on a real fleet. (b) If they do not survive, XDP works only on
Anywhere or container fleets, where the builder controls the user and can run
with root or set capabilities, and managed fleets fall back to RocketNet's GSO
(or io_uring) driver. (c) If the capabilities are not retained, the managed
fleet path is the GSO driver, which is RocketNet's non-root Linux driver for
hosts without CAP_NET_ADMIN (https://github.com/mas-bandwidth/rocketnet/blob/main/BUILDING.md).

A ten-minute test on a real managed EC2 fleet settles it: connect via SSM,
install the build with `sudo setcap cap_net_admin,cap_bpf,cap_net_raw+ep
<server>`, launch the game server, then check that `getcap <server>` still
shows the capabilities and that RocketNet's xdp driver create succeeds and
binds an AF_XDP socket on the ENA interface.
