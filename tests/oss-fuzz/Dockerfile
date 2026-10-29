# OSS-Fuzz image for rocketnet (https://github.com/google/oss-fuzz). PREPARED
# ONLY: nothing has been submitted. See README.md.
FROM gcr.io/oss-fuzz-base/base-builder
# rocketnet's Linux platform layer compiles the XDP transport unconditionally,
# so its headers and libraries are build dependencies even though the fuzz
# build embeds no BPF object (the xdp transport then refuses at create).
# io_uring is optional and left out: build.sh does not define ROCKETNET_IO_URING.
RUN apt-get update && apt-get install -y libbpf-dev libxdp-dev zip
RUN git clone --depth 1 https://github.com/mas-bandwidth/rocketnet $SRC/rocketnet
WORKDIR $SRC/rocketnet
COPY build.sh $SRC/
