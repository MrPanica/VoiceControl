FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update
RUN dpkg --add-architecture i386
RUN apt update && apt install -y git python3 python3-pip

RUN mkdir /sdks
RUN git config --global http.version HTTP/1.1
RUN bash -lc "set -e; for i in 1 2 3 4 5; do git clone --depth 1 --shallow-submodules https://github.com/alliedmodders/sourcemod --recurse-submodules -b 1.12-dev && exit 0; rm -rf sourcemod; sleep 5; done; exit 1"
RUN bash -lc "set -e; for i in 1 2 3 4 5; do git clone --depth 1 --shallow-submodules https://github.com/alliedmodders/metamod-source --recurse-submodules -b 1.12-dev && exit 0; rm -rf metamod-source; sleep 5; done; exit 1"
RUN bash -lc "set -e; for i in 1 2 3 4 5; do git clone --depth 1 --shallow-submodules https://github.com/alliedmodders/hl2sdk --recurse-submodules -b tf2 /sdks/hl2sdk-tf2 && exit 0; rm -rf /sdks/hl2sdk-tf2; sleep 5; done; exit 1"
RUN bash -lc "set -e; for i in 1 2 3 4 5; do git clone --depth 1 --shallow-submodules https://github.com/alliedmodders/hl2sdk --recurse-submodules -b sdk2013 /sdks/hl2sdk-sdk2013 && exit 0; rm -rf /sdks/hl2sdk-sdk2013; sleep 5; done; exit 1"
RUN bash -lc "set -e; for i in 1 2 3 4 5; do git clone --depth 1 --shallow-submodules https://github.com/alliedmodders/ambuild --recurse-submodules && exit 0; rm -rf ambuild; sleep 5; done; exit 1"

RUN apt install -y \
    autoconf \
    automake \
    clang \
    g++-multilib \
    libtool \
    make \
    nasm \
    pkg-config \
    libiberty-dev:i386 \
    libelf-dev:i386 \
    libboost-dev:i386 \
    libbsd-dev:i386 \
    libunwind-dev:i386 \
    lib32stdc++-11-dev \
    lib32z1-dev \
    libc6-dev:i386 \
    linux-libc-dev:i386 \
    libopus-dev \
    libopus-dev:i386

RUN git clone https://github.com/xiph/opus.git -b v1.3.1 --depth 1
RUN cd opus && ./autogen.sh && ./configure CFLAGS="-m32 -g -O2" LDFLAGS=-m32 && make && make install && cd ..
RUN cd opus && make distclean && ./configure CFLAGS="-fPIC -g -O2" --prefix=/usr/local/opus64 && make && make install && cd ..

RUN pip install ./ambuild
