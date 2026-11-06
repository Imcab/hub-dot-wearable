#!/bin/sh
# Builds and runs the tests that do not need hardware. Only needs g++
set -e
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

flags="-std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined"

g++ $flags -Icomponents/vitals/include test/test_vitals.cpp -o "$out/test_vitals" -lm
"$out/test_vitals"

g++ $flags -Icomponents/fall_detect/include test/test_fall_detect.cpp components/fall_detect/fall_detect.cpp \
    -o "$out/test_fall_detect" -lm
"$out/test_fall_detect"
