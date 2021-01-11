#!/bin/bash

setcap cap_net_raw+ep ./src/ports/POSIX/OpENer
./src/ports/POSIX/OpENer eth0
