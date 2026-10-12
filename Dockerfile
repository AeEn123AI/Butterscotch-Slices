# Butterscotch Slices: hardened dedicated server image.
# Build:  docker build -t butterscotch-slices .
# Run it with the flags from docker-compose.yml / the README (read-only root, no capabilities, non-root user, ...).

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config ca-certificates libsdl2-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_NETPLAY=ON \
 && cmake --build build -j"$(nproc)" \
 && strip build/butterscotch

FROM debian:bookworm-slim
# SDL2 is only linked, the server never opens a window or an audio device.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libsdl2-2.0-0 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --no-create-home --uid 10001 --shell /usr/sbin/nologin bsserver \
 && mkdir /game /saves && chown bsserver /saves
COPY --from=build /src/build/butterscotch /usr/local/bin/butterscotch
USER 10001:10001
WORKDIR /saves
EXPOSE 7777/tcp
# The game folder is mounted read-only at /game; saves go to the /saves volume.
ENTRYPOINT ["butterscotch", "--server", "/game/data.win", "--bind", "0.0.0.0", "--port", "7777", "--save-folder", "/saves"]
CMD ["--max-clients", "8"]
