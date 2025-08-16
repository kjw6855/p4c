FROM ubuntu as intermediate

MAINTAINER Jiwon Kim "kim1685@purdue.edu"


# Update aptitude with new repo
RUN apt-get update

# Install software
RUN apt-get install -y git

ARG SSH_PRIVATE_KEY
# Make ssh dir
RUN mkdir /root/.ssh/

# Copy over private key, and set permissions
# Warning! Anyone who gets their hands on this image will be able
# to retrieve this private key file from the corresponding image layer
RUN echo "${SSH_PRIVATE_KEY}" > /root/.ssh/id_rsa
RUN echo "${SSH_PUBLIC_KEY}" > /root/.ssh/id_rsa.pub
RUN chmod 400 /root/.ssh/id_rsa
RUN chmod 400 /root/.ssh/id_rsa.pub

#ADD resolv.conf /etc/resolv.conf

#RUN eval `ssh-agent -s`
#RUN eval "$(ssh-agent)"
#RUN ssh-add /root/.ssh/id_rsa

# Create known_hosts
RUN touch /root/.ssh/known_hosts
# Add github key
RUN ssh-keyscan github.com >> /root/.ssh/known_hosts

RUN cat /root/.ssh/known_hosts

#RUN git config --global credential.provider generic

# Clone the conf files into the docker container
RUN apt-get install -y iputils-ping
#RUN ssh -Tvvv https://github.com/
#RUN ssh -Tvvv git@github.com:kjw6855/p4c.git

#RUN eval `ssh-agent -s` && \
#    ssh-add /root/.ssh/id_rsa && \
#    ssh -Tvvv git@github.com

RUN ssh-agent bash -c 'ssh-add /root/.ssh/id_rsa; git clone -v -b p4fuzzer --recursive git@github.com:kjw6855/p4c.git'
RUN ssh-agent bash -c 'ssh-add /root/.ssh/id_rsa; git clone -v -b p4fuzzer --recursive git@github.com:kjw6855/grpc.git'

RUN cat /etc/issue

RUN cd /p4c && git pull && git rev-parse --verify HEAD

FROM ubuntu
COPY --from=intermediate /p4c /srv/p4c
COPY --from=intermediate /grpc /srv/grpc
RUN apt-get update
RUN apt-get install -y cmake build-essential autoconf libtool pkg-config
RUN mkdir -p /srv/grpc/cmake/build
RUN (cd /srv/grpc/cmake/build && cmake ../.. && make -j2)
RUN (cd /srv/grpc/cmake/build && make install)

COPY basic.p4.tar.gz /srv
COPY fabric.p4.tar.gz /srv
RUN (cd /srv && tar xvzf basic.p4.tar.gz)
RUN (cd /srv && tar xvzf fabric.p4.tar.gz)


# Default to using 2 make jobs, which is a good default for CI. If you're
# building locally or you know there are more cores available, you may want to
# override this.
ARG MAKEFLAGS=-j2
# Useful environment variable for scripts.
ARG IN_DOCKER=TRUE
# Select the type of image we're building. Use `build` for a normal build, which
# is optimized for image size. Use `test` if this image will be used for
# testing; in this case, the source code and build-only dependencies will not be
# removed from the image.
ARG IMAGE_TYPE=test
# Whether to do a unity build.
ARG CMAKE_UNITY_BUILD=ON
# Whether to enable translation validation
ARG VALIDATION=OFF
# This creates a release build that includes link time optimization and links
# all libraries statically.
ARG BUILD_STATIC_RELEASE=OFF
# No questions asked during package installation.
ARG DEBIAN_FRONTEND=noninteractive
# Whether to install dependencies required to run PTF-ebpf tests
ARG INSTALL_PTF_EBPF_DEPENDENCIES=OFF
# Whether to build the P4Tools back end and platform.
ARG ENABLE_TEST_TOOLS=ON
ARG CMAKE_EXPORT_COMPILE_COMMANDS=ON
ARG P4C_USE_PREINSTALLED_PROTOBUF=ON
ARG P4C_USE_PREINSTALLED_ABSEIL=ON
# Whether to treat warnings as errors.
ARG ENABLE_WERROR=OFF
# Compile with Clang compiler
ARG COMPILE_WITH_CLANG=OFF
# Compile with sanitizers (UBSan, ASan)
ARG ENABLE_SANITIZERS=OFF
# Only execute the steps necessary to successfully run CMake.
ARG CMAKE_ONLY=OFF
# Build with -ftrivial-auto-var-init=pattern to catch more bugs caused by
# uninitialized variables.
ARG BUILD_AUTO_VAR_INIT_PATTERN=OFF

# Configuration of ASAN and UBSAN sanitizers:
# - Print symbolized stack trace for each error report.
# - Disable leaks detector as p4c uses GC.
ENV UBSAN_OPTIONS=print_stacktrace=1
ENV ASAN_OPTIONS=print_stacktrace=1:detect_leaks=0

# Delegate the build to tools/ci-build.
RUN /srv/p4c/tools/ci-build.sh
# Set the workdir after building p4c.
COPY p4testgen-cont.sh /srv/p4c/build
WORKDIR /srv/p4c/
