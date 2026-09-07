FROM ubuntu:24.04@sha256:52df9b1ee71626e0088f7d400d5c6b5f7bb916f8f0c82b474289a4ece6cf3faf
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential git ca-certificates pkg-config ninja-build python3-venv \
    python3-mako python3-yaml python3-packaging bison flex file \
    libepoxy-dev libdrm-dev libegl1-mesa-dev libgbm-dev libvulkan-dev \
    libglib2.0-dev libpixman-1-dev libslirp-dev libgtk-3-dev libspice-server-dev \
    libva-dev libv4l-dev libx11-dev libxrandr-dev libxpresent-dev check \
    glslang-tools mesa-vulkan-drivers vulkan-tools \
    p7zip-full msitools cabextract clang lld llvm-18 llvm-18-tools wine64 osslsigncode openssl xorriso \
    gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix \
    gcc-mingw-w64-i686-posix g++-mingw-w64-i686-posix \
    && python3 -m venv --system-site-packages /opt/vista-build-tools \
    && /opt/vista-build-tools/bin/pip install meson==1.8.5 pefile==2024.8.26 asn1crypto==1.5.1 signify==0.9.2 \
    && rm -rf /var/lib/apt/lists/*
ENV PATH="/opt/vista-build-tools/bin:${PATH}"
WORKDIR /workspace
