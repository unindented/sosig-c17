FROM docker.io/library/ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c

ARG LLVM_VERSION=22
ARG GCC_VERSION=14

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      cppcheck \
      "gcc-${GCC_VERSION}" \
      libc6-dev \
      make \
      ninja-build \
      wget \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/apt/keyrings \
    && wget --quiet https://apt.llvm.org/llvm-snapshot.gpg.key \
      --output-document=/etc/apt/keyrings/apt.llvm.org.asc \
    && echo \
      "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/resolute/ llvm-toolchain-resolute-${LLVM_VERSION} main" \
      > /etc/apt/sources.list.d/apt.llvm.org.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "clang-${LLVM_VERSION}" \
      "clang-format-${LLVM_VERSION}" \
      "clang-tidy-${LLVM_VERSION}" \
      "libclang-rt-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-22

COPY . /src
WORKDIR /src

ENTRYPOINT ["cmake", "--workflow", "--preset"]
CMD ["ci-debug"]
