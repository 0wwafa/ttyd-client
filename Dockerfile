FROM ubuntu:24.04

ARG TARGETARCH

# Dependencies: CA certificates are needed to verify wss:// servers
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates tini && \
    rm -rf /var/lib/apt/lists/*

# Application
COPY ./dist/${TARGETARCH}/ttyd-client /usr/bin/ttyd-client

WORKDIR /root

ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["ttyd-client", "--help"]
