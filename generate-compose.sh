#!/bin/bash
# generate-compose.sh
# Generates docker-compose.yml with N relay-node services, each with a static IP
# Usage: ./generate-compose.sh [num_containers] [total_rounds]

NUM_CONTAINERS=${1:-5}
TOTAL_ROUNDS=${2:-""}

# Places generated document into docker-compose.yml using heredoc
cat > docker-compose.yml << 'YAML_START'
services:
YAML_START

# Generate a service for each container
# Start at 2 since 172.20.0.1 is reserved for docker bridge gateway
for i in $(seq 0 $((NUM_CONTAINERS-1))); do
  IP="172.20.0.$((i+2))"
  cat >> docker-compose.yml << YAML_SERVICE
  relay-node-$i:
    build:
      context: .
    container_name: relay-node-$i
    environment:
      - SERVICE_NAME=relay-node
      - TOTAL_ROUNDS=${TOTAL_ROUNDS}
    networks:
      mesh-net:
        ipv4_address: $IP
        aliases:
          - relay-node
    # Mount the local './logs' folder into the container's '/app/logs'
    volumes:
      - ./logs:/app/logs

YAML_SERVICE
done

# Add network definition
cat >> docker-compose.yml << 'YAML_END'
networks:
  mesh-net:
    driver: bridge
    ipam:
      config:
        - subnet: 172.20.0.0/16 # 16 bits of address space to work with
YAML_END

echo "Generated docker-compose.yml with $NUM_CONTAINERS containers (IPs: 172.20.0.2 - 172.20.0.$((NUM_CONTAINERS+1))) (TOTAL_ROUNDS=${TOTAL_ROUNDS:-unset})"