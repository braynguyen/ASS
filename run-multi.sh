#!/bin/bash
# This script runs N relay-node containers using Docker Compose.
# Usage: ./run-multi.sh [num_containers]    (default: 5)

# ----------------------------------------------------------------------------------
# This script builds and starts NUM_CONTAINERS instances of the `relay-node`
# service and it streams logs while the containers run. All containers and networks
# are removed when the relay processes exit normally or when you press Ctrl+C.
# ----------------------------------------------------------------------------------

NUM_CONTAINERS=${1:-5}

# 1. Define the cleanup function (called on Ctrl+C)
cleanup() {
   echo "" # Add a newline for readability

    # Always run docker compose down. Stops any running containers
    # and removes them along with networks.
    echo "Stopping and removing containers..."
    docker compose down

    # Combine the logs.
    echo "Combining node logs into ./logs/merged_log.csv..."
    cat ./logs/node_*.csv | grep -v "Timestamp" | sort > ./logs/merged_log.csv
    
    echo "Cleanup complete."
}

# 2. Set the trap to call the 'cleanup' function on Ctrl+C
trap cleanup SIGINT

# Create and clear the logs directory
echo "Creating/clearing ./logs directory..."
mkdir -p ./logs
rm -f ./logs/*.csv

# 3. Generate docker-compose.yml with N containers
echo "Generating docker-compose.yml for $NUM_CONTAINERS containers..."
./generate-compose.sh $NUM_CONTAINERS

# 4. Build and start all containers
echo "Building and starting containers..."
docker compose up -d --build


# 4. Stream the logs in the background
echo "Streaming logs... (press Ctrl+C to stop)"
docker compose logs -f &

# Save the Process ID (PID) of the background log command
LOGS_PID=$!

# 5. Wait for the logger to exit naturally (after all containers stop).
#   This is now the main "foreground" task.
#   This command blocks until $LOGS_PID finishes.
wait $LOGS_PID

# 6. Once 'wait' finishes, the logger and the containers have stopped.
echo "Containers finished."