# Build context: this directory plus willow-bundle.tar.gz from a Willow release.
FROM debian:bookworm-slim AS runtime
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates libcjson1 libgcc-s1 libsodium23 libssl3 libstdc++6 \
      zlib1g python3 python3-cryptography python3-nacl \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 1000 willow \
 && useradd --uid 1000 --gid 1000 --home-dir /state --no-create-home willow \
 && install -d -o willow -g willow -m 0700 /state

FROM runtime AS unpack
ARG VERSION
ARG SOURCE_REVISION
COPY willow-bundle.tar.gz /tmp/willow-bundle.tar.gz
RUN mkdir /tmp/bundle \
 && tar -xzf /tmp/willow-bundle.tar.gz -C /tmp/bundle --strip-components=1 --no-same-owner \
 && python3 -c 'import json,sys; m=json.load(open("/tmp/bundle/manifest.json")); assert (m["version"],m["source"]["commit"]) == tuple(sys.argv[1:]), "bundle version/source mismatch"' "$VERSION" "$SOURCE_REVISION" \
 && python3 /tmp/bundle/install-willow.py --bundle /tmp/bundle --prefix /opt/willow

FROM runtime
ARG VERSION
ARG SOURCE_REVISION
LABEL org.opencontainers.image.title="Willow" \
      org.opencontainers.image.description="MeshCore host, Base companion, dashboard and MQTT broker" \
      org.opencontainers.image.source="https://github.com/slepp/meshcore-aspen" \
      org.opencontainers.image.version=$VERSION \
      org.opencontainers.image.revision=$SOURCE_REVISION \
      org.opencontainers.image.licenses="Apache-2.0"
COPY --from=unpack /opt/willow /opt/willow
ENV HOME=/state HEW_WORKERS=4
WORKDIR /opt/willow/hosts/willow
USER 1000:1000
CMD ["python3", "-B", "tools/willow.py", "run", "--release", "--state", "/state/host"]
