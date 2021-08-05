#!/bin/bash

set -e

# must be run from build directory

cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-g3 -O0 -fsanitize=address"
cmake --build .
../dev/scripts/unused_values.sh ../src/app.c ../src/peer.c
