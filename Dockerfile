FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    make \
    libpci-dev \
    nvidia-cuda-toolkit \
    --no-install-recommends \
    --no-install-suggests \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile .
COPY src ./src

RUN make
