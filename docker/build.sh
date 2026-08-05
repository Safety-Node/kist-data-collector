#!/bin/bash
# Build the image from the repo root (context must include the source tree).
set -e
cd "$(dirname "$0")/.."
docker build -t kist-data-collector -f docker/Dockerfile .
