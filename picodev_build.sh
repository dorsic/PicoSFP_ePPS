docker build \
  --build-arg PICO_SDK_VERSION=2.3.1 \
  --build-arg USER_UID=$(id -u) \
  --build-arg USER_GID=$(id -g) \
  -f picodev.dockerfile \
  -t picodev:2.3.1 .
