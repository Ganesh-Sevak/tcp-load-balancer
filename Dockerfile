FROM node:24-alpine AS web-build

WORKDIR /web
COPY web/package*.json ./
RUN npm ci
COPY web/ .
RUN npm run build

FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

FROM ubuntu:24.04

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/tcp-load-balancer /usr/local/bin/tcp-load-balancer
COPY config/backends.conf /app/config/backends.conf
COPY --from=web-build /web/dist /app/web/dist

EXPOSE 9000 9100
ENTRYPOINT ["tcp-load-balancer"]
CMD ["--config", "/app/config/backends.conf"]
