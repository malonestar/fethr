#!/usr/bin/env bash
# whisper-flow: sudo-free whisper.cpp Vulkan server install (tested on an AMD Radeon R9700, Vulkan backend).
# Everything lives in ~/whisper-flow. Idempotent — safe to re-run.
set -euo pipefail
BASE="$HOME/whisper-flow"
mkdir -p "$BASE"
cd "$BASE"

echo "=== [1/6] cmake (user-local) ==="
if [ ! -x "$BASE/cmake/bin/cmake" ]; then
  curl -fsSL -o cmake.tgz https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz
  mkdir -p cmake && tar xzf cmake.tgz -C cmake --strip-components=1 && rm cmake.tgz
fi
export PATH="$BASE/cmake/bin:$PATH"
cmake --version | head -1

echo "=== [2/6] Vulkan SDK (glslc + headers, user-local) ==="
if [ ! -d "$BASE/vulkansdk" ]; then
  curl -fsSL -o vulkansdk.tar.xz "https://sdk.lunarg.com/sdk/download/latest/linux/vulkan_sdk.tar.xz"
  mkdir -p vulkansdk && tar xJf vulkansdk.tar.xz -C vulkansdk && rm vulkansdk.tar.xz
fi
SETUP=$(find "$BASE/vulkansdk" -maxdepth 2 -name setup-env.sh | head -1)
echo "sourcing $SETUP"
# shellcheck disable=SC1090
set +u; source "$SETUP"; set -u
glslc --version | head -1

echo "=== [3/6] clone whisper.cpp ==="
if [ ! -d "$BASE/whisper.cpp" ]; then
  git clone --depth 1 https://github.com/ggml-org/whisper.cpp "$BASE/whisper.cpp"
fi
cd "$BASE/whisper.cpp"

echo "=== [4/6] build (Vulkan) ==="
cmake -B build -DGGML_VULKAN=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)" --config Release
ls -la build/bin/whisper-server build/bin/whisper-cli

echo "=== [5/6] model large-v3-turbo-q8_0 ==="
if [ ! -f models/ggml-large-v3-turbo-q8_0.bin ]; then
  bash models/download-ggml-model.sh large-v3-turbo-q8_0
fi
ls -la models/ggml-large-v3-turbo-q8_0.bin

echo "=== [6/6] smoke test (GPU device 0, jfk.wav) ==="
GGML_VK_VISIBLE_DEVICES=0 ./build/bin/whisper-cli -m models/ggml-large-v3-turbo-q8_0.bin -f samples/jfk.wav -np 2>&1 | tail -5
echo "=== SETUP COMPLETE ==="
