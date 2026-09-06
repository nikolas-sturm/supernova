#!/usr/bin/env bash
# Only build.mjs supplies this program, with every command argument shell-quoted.
set -eu
eval "${SUPERNOVA_NATIVE_COMMAND:?Run tooling/native/build.mjs instead}"
