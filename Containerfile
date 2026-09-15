FROM docker.io/library/ubuntu:26.04@sha256:513c074113a871b51a8d16ab445c88779d6452d937a164fb5cc479f32668a41d

ARG LLVM_VERSION=22

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates \
      cmake \
      cppcheck \
      libc6-dev \
      make \
      ninja-build \
      wget \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /etc/apt/keyrings \
    && wget --quiet https://apt.llvm.org/llvm-snapshot.gpg.key \
      --output-document=/etc/apt/keyrings/apt.llvm.org.asc \
    && echo \
      "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
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
