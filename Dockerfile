# syntax=docker/dockerfile:1.7
FROM ubuntu:24.04 AS build
ARG VCPKG_COMMIT=93ad765c7b66ab9ce97469ec23a90b15c3a14ee5
RUN apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
      build-essential ca-certificates clang-18 cmake curl git ninja-build pkg-config tar unzip zip \
    && rm -rf /var/lib/apt/lists/*
ENV VCPKG_ROOT=/opt/vcpkg CXX=clang++-18 CC=clang-18
RUN git clone -q https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" checkout -q "$VCPKG_COMMIT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
WORKDIR /src
COPY . .
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    cmake --preset release -DBUILD_TESTING=OFF -DRME_ARCH=x86-64-v3 \
    && cmake --build --preset release --target rme_node_bin \
    && strip build/release/node/rme_node

FROM ubuntu:24.04
RUN apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
      iproute2 iptables \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 rme \
    && mkdir /data && chown rme /data
COPY --from=build /src/build/release/node/rme_node /usr/local/bin/rme_node
USER rme
EXPOSE 7000 7100
ENTRYPOINT ["/usr/local/bin/rme_node"]
