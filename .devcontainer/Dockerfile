FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential \
       cmake \
       ninja-build \
       gdb \
       git \
       doxygen \
       graphviz \
       python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspaces/openEtherCAT
