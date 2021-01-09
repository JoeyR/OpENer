# [Choice] Debian version: buster, stretch
#ARG VARIANT="buster"
#FROM buildpack-deps:${VARIANT}-curl

FROM alpine:latest

###
# [Option] Install zsh
#ARG INSTALL_ZSH="true"
# [Option] Upgrade OS packages to their latest versions
#ARG UPGRADE_PACKAGES="true"

# Install needed packages and setup non-root user. Use a separate RUN statement to add your own dependencies.
#ARG USERNAME=vscode
#ARG USER_UID=1000
#ARG USER_GID=$USER_UID
#COPY .devcontainer/library-scripts/*.sh /tmp/library-scripts/
#RUN bash /tmp/library-scripts/common-debian.sh "${INSTALL_ZSH}" "${USERNAME}" "${USER_UID}" "${USER_GID}" "${UPGRADE_PACKAGES}" \
#  && apt-get clean -y && rm -rf /var/lib/apt/lists/* /tmp/library-scripts
###

## 
RUN apk update && \
  apk upgrade && \
  apk --update add \
  gcc \
  g++ \
  build-base \
  libstdc++ \
  cmake \
  make \
  cppcheck \
  libcap-dev \
  iproute2 \
  bash \
  && rm -rf /var/cache/apk/*

WORKDIR /workspaces/OpEner
COPY source/ /source/ 
COPY bin/ /bin/
WORKDIR /bin/posix/

RUN cmake -DOpENer_PLATFORM:STRING="POSIX" -DCMAKE_BUILD_TYPE:STRING="Debug" -DBUILD_SHARED_LIBS:BOOL=OFF ../../source
RUN make

RUN setcap cap_net_raw+ep ./src/ports/POSIX/OpENer && /bin/bash

ENTRYPOINT ["./start_device.sh"]