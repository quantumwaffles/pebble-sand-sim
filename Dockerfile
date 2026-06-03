# Touch-capable Pebble build environment (Core Devices SDK 4.9+, pebble-tool 5.x).
# The old rebble/pebble-sdk image is stuck at SDK 4.3, which predates the
# TouchService API — this image installs the current SDK so PBL_TOUCH builds.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# System deps for pebble-tool + the bundled QEMU emulator
RUN apt-get update && apt-get install -y --no-install-recommends \
      nodejs npm \
      libsdl2-2.0-0 libglib2.0-0 libpixman-1-0 zlib1g libsndio7.0 \
      curl ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

# uv manages the Python 3.13 runtime pebble-tool needs
RUN curl -LsSf https://astral.sh/uv/install.sh | sh
ENV PATH="/root/.local/bin:${PATH}"

# pebble-tool (CLI) + the SDK itself (toolchain + QEMU installed on demand).
# </dev/null so any first-run prompt gets EOF and falls back to non-interactive.
RUN uv tool install pebble-tool --python 3.13 \
    && pebble sdk install latest </dev/null

WORKDIR /app
