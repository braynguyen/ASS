#!/bin/bash
# This script runs the interactive test environment for the relay app.

docker run -it --rm \
  --init \
  --env SERVICE_NAME=relay-node \
  --add-host=relay-node:10.10.0.2 \
  --add-host=relay-node:10.10.0.3 \
  -v "./:/app" \
  -w "/app" \
  gcc:latest \
  bash
