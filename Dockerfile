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

# Port and password come from the env; override at `docker run` for cross-machine bootstrap.
ENV MESH_PORT=9001 MESH_PASSWORD=mysecretpassword
CMD ["sh", "-c", "./mesh_cli \"$MESH_PORT\" \"$MESH_PASSWORD\""]