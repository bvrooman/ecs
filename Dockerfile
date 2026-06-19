# syntax=docker/dockerfile:1
# -----------------------------------------------------------------------------
# Dev / test toolchain for the `ecs` project.
#
# Why this exists: the project uses std::move_only_function (see
# include/ecs/command_buffer.hpp and include/ecs/schedule.hpp), which is a
# libstdc++ feature. macOS's libc++ (Apple Clang *and* Homebrew LLVM Clang) does
# not ship it, so the project cannot be built natively on a Mac. This image
# provides GCC + libstdc++ on Linux, matching the environment the project was
# tested against.
#
# Intended use: a CLion "Docker" toolchain (Settings | Build, Execution,
# Deployment | Toolchains -> add Docker -> Image: ecs-dev). It also works
# standalone -- see the commands at the bottom of this file.
# -----------------------------------------------------------------------------
FROM gcc:14

# The gcc:14 image already provides g++-14, libstdc++, git and build-essential.
# Add the rest of the toolchain CLion expects:
#   cmake     - CMakeLists.txt requires >= 3.24 (Debian trixie ships 3.31)
#   ninja     - generator used by the project's CMake profiles
#   gdb       - so CLion can debug test targets inside the container
#   rsync     - used by CLion to sync headers for indexing
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      cmake \
      ninja-build \
      gdb \
      rsync \
 && rm -rf /var/lib/apt/lists/*

# Fail the image build early if the toolchain is too old, so problems surface
# here rather than deep in a CLion CMake run.
RUN cmake --version && ninja --version && g++ --version

# -----------------------------------------------------------------------------
# Standalone use (no CLion required):
#
#   docker build -t ecs-dev .
#   docker run --rm -v "$PWD":/src -w /work ecs-dev bash -c '\
#       cmake -S /src -B /work -G Ninja -DCMAKE_BUILD_TYPE=Debug && \
#       cmake --build /work && \
#       ctest --test-dir /work --output-on-failure'
# -----------------------------------------------------------------------------
