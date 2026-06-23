# Dockerfile — build Redon and run the server + browser UI in one container.
#
#   docker build -t redon .
#   docker run --rm -p 8080:8080 -p 9090:9090 redon
#   # then open http://localhost:8080   (Prometheus metrics on :9090/metrics)
#
# To persist data across restarts, mount a volume and enable the on-disk engine:
#   docker run --rm -p 8080:8080 -v redon-data:/data -e REDON_DISK=/data/redon.db redon

# ---- build stage ----------------------------------------------------------
FROM debian:bookworm-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends cmake g++ make && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j

# ---- runtime stage --------------------------------------------------------
FROM debian:bookworm-slim
RUN useradd --create-home --uid 10001 redon
COPY --from=build /src/build/redon-server /usr/local/bin/redon-server
COPY --from=build /src/build/redon-web    /usr/local/bin/redon-web
COPY --from=build /src/build/redon-cli    /usr/local/bin/redon-cli
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh && mkdir -p /data && chown redon /data

USER redon
WORKDIR /data
# The web gateway must listen on all interfaces inside the container so the
# published port is reachable from the host.
ENV REDON_WEB_HOST=0.0.0.0
# 8080 = web UI, 6380 = Redon protocol, 9090 = Prometheus metrics.
EXPOSE 8080 6380 9090
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
