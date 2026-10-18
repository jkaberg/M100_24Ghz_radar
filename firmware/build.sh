#!/bin/sh
# Builds the app and both OTA files (A/B slot scheme, no bootloader).
set -e
cd "$(dirname "$0")"
mkdir -p bin
rm -f bin/*
# the version in src/include/version_cfg.h, packaged twice: with the stock module's OTA ids (first
# install) and with our own (every update after that)
make clean > /dev/null 2>&1 || true
rm -rf out
make -f makefile
