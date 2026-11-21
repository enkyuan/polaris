# Compile with the host CPU, including when building the amd64 image on ARM.
FROM --platform=$BUILDPLATFORM debian:bookworm-slim AS build
ARG BUILDARCH
ARG TARGETARCH
RUN set -eu; \
    case "$BUILDARCH:$TARGETARCH" in \
        amd64:amd64|arm64:arm64) compiler=g++ ;; \
        arm64:amd64) compiler=g++-x86-64-linux-gnu ;; \
        amd64:arm64) compiler=g++-aarch64-linux-gnu ;; \
        *) exit 2 ;; \
    esac; \
    apt-get update; \
    apt-get install -y --no-install-recommends cmake "$compiler" make; \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY src/ src/
RUN set -eu; \
    case "$BUILDARCH:$TARGETARCH" in \
        arm64:amd64) arch=x86_64 ;; \
        amd64:arm64) arch=aarch64 ;; \
        *) arch= ;; \
    esac; \
    if [ -n "$arch" ]; then \
        set -- -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR="$arch" \
            -DCMAKE_CXX_COMPILER="$arch-linux-gnu-g++"; \
        strip="$arch-linux-gnu-strip"; \
    else \
        set --; strip=strip; \
    fi; \
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"; \
    cmake --build build --parallel 4; \
    "$strip" build/shopping-sessions

FROM debian:bookworm-slim
COPY --from=build /src/build/shopping-sessions /app/shopping-sessions
ENV HOME=/scratch
USER 1000:1000
ENTRYPOINT ["/app/shopping-sessions"]
