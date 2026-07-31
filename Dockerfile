# syntax=docker/dockerfile:1.7

ARG UBUNTU_VERSION=20.04
FROM ubuntu:${UBUNTU_VERSION} AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG OPENCV_VERSION=5.0.0
ARG TARGETARCH

RUN if [ -n "${TARGETARCH}" ] && [ "${TARGETARCH}" != "amd64" ]; then \
      echo "This Dockerfile currently supports linux/amd64 only" >&2; exit 2; \
    fi
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl libjpeg-dev libpng-dev \
      libtiff-dev pkg-config python3-pip tar zlib1g-dev && \
    python3 -m pip install --no-cache-dir cmake==3.31.6 ninja==1.11.1.4 && \
    rm -rf /var/lib/apt/lists/*

RUN mkdir -p /tmp/opencv-source /tmp/opencv-build /opt/opencv && \
    curl --fail --location --retry 5 --retry-delay 5 --retry-connrefused \
      "https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz" \
      --output /tmp/opencv.tar.gz && \
    tar -xzf /tmp/opencv.tar.gz --strip-components=1 \
      -C /tmp/opencv-source && \
    cmake -S /tmp/opencv-source -B /tmp/opencv-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/opencv \
      -DCMAKE_INSTALL_RPATH='$ORIGIN' \
      -DBUILD_LIST=core,imgproc,imgcodecs,dnn \
      -DBUILD_SHARED_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_JAVA=OFF \
      -DBUILD_opencv_apps=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_TESTS=OFF \
      -DBUILD_JPEG=ON -DBUILD_PNG=ON -DBUILD_PROTOBUF=ON \
      -DBUILD_TIFF=ON -DBUILD_ZLIB=ON \
      -DWITH_EIGEN=OFF -DWITH_FFMPEG=OFF -DWITH_GDAL=OFF \
      -DWITH_GSTREAMER=OFF -DWITH_GTK=OFF -DWITH_IPP=OFF \
      -DWITH_ITT=OFF -DWITH_JASPER=OFF -DWITH_LAPACK=OFF \
      -DWITH_OPENCL=OFF -DWITH_OPENEXR=OFF -DWITH_QT=OFF \
      -DWITH_TBB=OFF -DWITH_WEBP=OFF && \
    cmake --build /tmp/opencv-build --parallel 2 && \
    cmake --install /tmp/opencv-build && \
    rm -rf /tmp/opencv-source /tmp/opencv-build /tmp/opencv.tar.gz

WORKDIR /src
COPY . .
RUN config="$(find /opt/opencv -name OpenCVConfig.cmake -print -quit)" && \
    test -n "$config" && \
    cmake -S . -B build/docker -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOpenCV_DIR="$(dirname "$config")" && \
    cmake --build build/docker --parallel 2 && \
    LD_LIBRARY_PATH=/opt/opencv/lib \
      ctest --test-dir build/docker --output-on-failure && \
    cmake --install build/docker --prefix /opt/lw-ppocr && \
    find /opt/opencv/lib -maxdepth 1 \( -type f -o -type l \) \
      -name 'libopencv*.so*' -exec cp -P '{}' /opt/lw-ppocr/ \; && \
    LD_LIBRARY_PATH=/opt/lw-ppocr \
      ldd /opt/lw-ppocr/lw-ppocr-http-service | tee /tmp/ldd.txt && \
    ! grep -q 'not found' /tmp/ldd.txt && \
    rm -rf /opt/lw-ppocr/examples/*/bin /opt/lw-ppocr/examples/*/obj \
      /opt/lw-ppocr/examples/*/__pycache__

FROM ubuntu:${UBUNTU_VERSION} AS runtime

ARG VERSION=0.2.0
LABEL org.opencontainers.image.title="lw.PPOCR.OpenCVDNN" \
      org.opencontainers.image.description="Cross-platform PP-OCR HTTP service powered by OpenCV DNN" \
      org.opencontainers.image.version="${VERSION}" \
      org.opencontainers.image.source="https://github.com/lxw112190/lw.PPOCR.OpenCVDNN" \
      org.opencontainers.image.licenses="MIT"

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl libgcc-s1 libstdc++6 && \
    rm -rf /var/lib/apt/lists/* && \
    groupadd --gid 10001 ppocr && \
    useradd --uid 10001 --gid ppocr --home-dir /nonexistent \
      --shell /usr/sbin/nologin ppocr && \
    mkdir -p /data/logs && chown -R ppocr:ppocr /data

COPY --from=build /opt/lw-ppocr /opt/lw-ppocr

ENV LD_LIBRARY_PATH=/opt/lw-ppocr \
    LW_PPOCR_LISTEN_HOST=0.0.0.0 \
    LW_PPOCR_PORT=8787 \
    LW_PPOCR_LOG_FILE=/data/logs/lw-ppocr.log

WORKDIR /opt/lw-ppocr
VOLUME ["/data/logs"]
EXPOSE 8787
USER 10001:10001

HEALTHCHECK --interval=10s --timeout=3s --start-period=30s --retries=5 \
  CMD curl --fail --silent --show-error http://127.0.0.1:8787/health >/dev/null || exit 1

ENTRYPOINT ["/opt/lw-ppocr/lw-ppocr-http-service"]
CMD ["--config", "/opt/lw-ppocr/http-service.json"]
