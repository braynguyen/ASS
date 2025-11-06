#!/bin/bash
# This script runs N relay-node containers using Docker Compose.
# Usage: ./run-multi.sh [num_containers]    (default: 3)

# ------------------------------------------------------------------------------
# There are two ways to use this script, depending on the docker-compose.yml:
#
# 1. If docker-compose.yml uses 'build: .':
#    - Docker Compose will build a custom image using the Dockerfile.
#    - The Dockerfile will:
#        * Start FROM gcc:latest
#        * Copy relay.c into the image
#        * Compile relay.c at build time, producing a 'relay' binary
#        * Set the default command to run the compiled binary
#    - This approach bakes the compiled binary into the image, so containers start faster and are more reproducible.
#    - However, you need to rebuild the image each time you change relay.c.
#
# 2. If docker-compose.yml uses 'image: gcc:latest':
#    - Docker Compose will pull and use the official gcc:latest image for each container.
#    - The source code (relay.c) is mounted into the container at runtime using a volume.
#    - The container runs a command that compiles relay.c and then executes the resulting binary each time it starts:
#        * command: bash -c "gcc -o relay relay.c && ./relay"
#    - This approach is convenient for rapid development, as changes to relay.c on your host are immediately available in the container.
#    - However, it may be slower to start each container, and the build is not reproducible unless you pin the source code version.
# ------------------------------------------------------------------------------

NUM_CONTAINERS=${1:-3}

# docker-compose.yml uses 'build: .' (builds a custom image from your Dockerfile):
docker compose up --scale relay-node=$NUM_CONTAINERS --build

# docker-compose.yml uses 'image: gcc:latest' (compiles in the container at runtime):
# docker compose up --scale relay-node=$NUM_CONTAINERS
