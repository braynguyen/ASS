#!/bin/bash
# This script runs N relay-node containers using Docker Compose.
# Usage: ./run-multi.sh [num_containers]    (default: 3)

# ----------------------------------------------------------------------------------
# This script builds and starts NUM_CONTAINERS instances of the `relay-node`
# service and it streams logs while the containers run. All containers and networks
# are removed when the relay processes exit normally or when you press Ctrl+C.
# ----------------------------------------------------------------------------------

NUM_CONTAINERS=${1:-3}

# 1. Define the cleanup function (called on Ctrl+C)
cleanup() {
    echo "" # Add a newline after the logs
    echo "Caught Ctrl+C. Stopping and removing containers..."
    docker compose down
    exit 1 # Exit the script
}

# 2. Set the trap to call the 'cleanup' function on Ctrl+C
trap cleanup SIGINT

# 3. Build and run the containers in detached mode
echo "Building and starting containers..."
docker compose up -d --build --scale relay-node=3

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

# 7. Run the final cleanup
echo "Cleaning up..."
docker compose down
