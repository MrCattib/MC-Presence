#!/usr/bin/env bash
set -e

g++ mc-presence.cpp -o mc-presence $(pkg-config --cflags --libs Qt6Widgets Qt6Network) -ldl

echo "Build successful: ./mc-presence"
