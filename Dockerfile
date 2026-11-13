# MiniVectorDB - evaluation image.
#
# The first stage compiles the server from the sources that live in this
# repository. The second stage is the runtime image: it only carries the
# compiled binary, so the container needs neither network access nor any third
# party library at run time.
#
# The base image is pinned by digest and the compiler package version is
# pinned as well, so the produced image is reproducible. The server itself
# links against nothing but the C++ runtime that ships with the base image.

FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5 AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG GCC_VERSION=13.3.0-6ubuntu2~24.04.1

RUN set -eux; \
    apt-get update; \
    (apt-get install -y --no-install-recommends "g++-13=${GCC_VERSION}" \
      || apt-get install -y --no-install-recommends g++); \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/
# The build script is executed by the shell, so any carriage returns that a
# checkout with Windows line endings could have introduced are removed first.
RUN sed -i 's/\r$//' /src/minivectordb/build.sh \
    && sh /src/minivectordb/build.sh /src/build/minivectordb

FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

LABEL org.opencontainers.image.title="MiniVectorDB" \
      org.opencontainers.image.version="1.0" \
      org.opencontainers.image.description="Lightweight vector database with SQL, transactions and crash recovery"

WORKDIR /app
COPY --from=builder /src/build/minivectordb /app/minivectordb
RUN mkdir -p /data

VOLUME ["/data"]

ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
