#!/bin/sh
docker run --rm -it \
  -v "$PWD":/work \
  picodev:2.3.1 \
  bash -lc 'cmake -S /work -B /work/build -G Ninja -DPICO_BOARD=pico2 && cmake --build /work/build'
