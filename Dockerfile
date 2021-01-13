FROM debian:testing
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update -y && apt-get install -y \
  build-essential \
  clang \
  git \
  cmake \
  cmake-data \
  pkg-config \
  python3 \
  python3-sphinx \
  libcairo2-dev	\
  libxcb1-dev \
  libxcb-util0-dev \
  libxcb-randr0-dev \
  libxcb-composite0-dev \
  python3-xcbgen \
  xcb-proto \
  libxcb-image0-dev	 \
  libxcb-ewmh-dev \
  libxcb-icccm4-dev \
  libxcb-xkb-dev \
  libxcb-xrm-dev \
  libxcb-cursor-dev \
  libasound2-dev \
  libpulse-dev \
  i3-wm \
  libjsoncpp-dev \
  libmpdclient-dev \
  libcurl4-openssl-dev \
  libnl-genl-3-dev

COPY . /polybar
WORKDIR /polybar
RUN rm -rf build; exit 0
RUN mkdir -p build
WORKDIR build
RUN cmake \
  -DENABLE_I3=ON \
  -DENABLE_ALSA=ON \
  -DENABLE_PULSEAUDIO=ON \
  -DENABLE_NETWORK=ON \
  -DENABLE_CURL=ON \
  -DBUILD_POLYBAR_MSG=ON \
  -DBUILD_DOC=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  ..
RUN make

ENTRYPOINT ["/polybar/build/bin/polybar"]
