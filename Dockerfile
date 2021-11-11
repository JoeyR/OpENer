FROM ubuntu:20.04

ENV TZ=US/Pacific
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

RUN apt-get update && apt-get install -y git cmake libcap-dev libcap2-bin

WORKDIR /
ADD / /OpENer
WORKDIR /OpENer/bin/posix

RUN cmake -DOpENer_PLATFORM:STRING="POSIX" -DCMAKE_BUILD_TYPE:STRING="Debug" -DBUILD_SHARED_LIBS:BOOL=OFF ../../source
RUN make