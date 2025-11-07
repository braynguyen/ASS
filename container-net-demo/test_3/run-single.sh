#!/bin/bash
# This script runs the relay app in a single container (non-interactive).
# Dummy hosts are added to simulate network environment.

docker run --rm \
  --init \
  --env SERVICE_NAME=relay-node \
  --add-host=relay-node:10.10.0.2 \
  --add-host=relay-node:10.10.0.3 \
  -v "./:/app" \
  -w "/app" \
  gcc:latest \
  bash -c "gcc -o relay relay.c && ./relay"
