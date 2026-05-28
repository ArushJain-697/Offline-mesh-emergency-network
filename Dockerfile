FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libsodium-dev \
    iputils-ping \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /mesh

COPY . .

RUN make build

CMD ["./mesh_cli"]