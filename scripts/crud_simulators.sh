#!/bin/bash


DOCKER_IP_BASE="172.17.0."
DEVICE_IP_START="2"
DEVICE_IP_END="5"

docker context use remote &> /dev/null;
docker pull jrapala/device_simulator:latest &> /dev/null;

for (( ip_=$DEVICE_IP_START; ip_ < $DEVICE_IP_END; ip_++ ))
do
  device_ip=$DOCKER_IP_BASE$ip_
  echo -e "removing:\t$device_ip"  

  docker stop $device_ip &> /dev/null;
  docker rm $device_ip &> /dev/null;
done

for (( ip_=$DEVICE_IP_START; ip_ < $DEVICE_IP_END; ip_++ ))
do
  device_ip=$DOCKER_IP_BASE$ip_
  echo -e "creating:\t$device_ip"

  docker run -t -d --ip="${device_ip}" --name="${device_ip}" device_simulator  &> /dev/null;

done