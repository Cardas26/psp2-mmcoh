FROM registry.fedoraproject.org/fedora@sha256:3fb969dd07e631c17d12f5955bcb6db2078a6997eb68be76b6492e7bd6052e88

RUN dnf -y install git cmake ninja-build gcc gcc-c++ make patch \
      python3 wget curl unzip zip xz binutils sudo && \
    dnf clean all

ENV VITASDK=/usr/local/vitasdk
ENV PATH=$VITASDK/bin:$PATH

ARG VDPM_COMMIT=e255587d4deff255db68fcb73f4e05a82751de15
ARG SHACCCGEXT_COMMIT=fb0e9d338525b067f3679ab33571323336493cca
ARG VITASHARK_COMMIT=df24065e65098b2d1ac533760109ad4367573f28

RUN git clone https://github.com/vitasdk-softfp/vdpm /tmp/vdpm && \
    cd /tmp/vdpm && git checkout --detach ${VDPM_COMMIT} && \
    ./bootstrap-vitasdk.sh && \
    rm -rf /tmp/vdpm

RUN vdpm pacman -S --noconfirm taihen libmathneon kubridge libvorbis libogg libsndfile lame opus opusfile flac opensles mpg123 zlib

RUN git clone https://github.com/bythos14/SceShaccCgExt /tmp/SceShaccCgExt && \
    cd /tmp/SceShaccCgExt && git checkout --detach ${SHACCCGEXT_COMMIT} && \
    cmake -Bbuild -GNinja -DCMAKE_C_FLAGS=-std=gnu17 -DCMAKE_CXX_FLAGS=-std=gnu++17 && \
    cmake --build build && cmake --install build && \
    rm -rf /tmp/SceShaccCgExt

RUN git clone https://github.com/Rinnegatamante/vitaShaRK /tmp/vitaShaRK && \
    cd /tmp/vitaShaRK && git checkout --detach ${VITASHARK_COMMIT} && \
    make install && \
    rm -rf /tmp/vitaShaRK
