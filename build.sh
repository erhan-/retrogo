#!/bin/bash
set -e
docker build -t retrogo-builder -f Dockerfile.retroarch .
docker run --rm -v "$(pwd):/output" retrogo-builder cp /retroarch /output/
echo "Binary: $(pwd)/retroarch"
ls -lh retroarch
