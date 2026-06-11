FROM buildpack-deps:bookworm

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        curl \
        git \
        ninja-build \
        pkg-config \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
COPY . /opt/ruvia

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh \
    && cmake -S /opt/ruvia -B /opt/ruvia/build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DRUVIA_BUILD_TECHEMPOWER=ON \
    && cmake --build /opt/ruvia/build --target ruvia_techempower

EXPOSE 8080

CMD ["/opt/ruvia/build/benchmarks/techempower/ruvia/ruvia_techempower"]
