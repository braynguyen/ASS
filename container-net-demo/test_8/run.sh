#!/bin/bash
# Usage: ./run.sh [num_containers]

# ---------------------------------------------------------
# This script builds & runs containers, streams their logs,
# and automatically cleans up and combines logs when
# the script exits for ANY reason (including Ctrl+C).
# ---------------------------------------------------------

# Default number of containers
DEFAULT_NODES=3

# Simple help
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    echo "Usage: $0 [num_containers]"
    echo "Starts the compose stack and scales relay-node to num_containers (default: $DEFAULT_NODES)."
    exit 0
fi

# ---------------------------------------------------------
# Argument parsing & validation
# ---------------------------------------------------------

# Accept a single optional positional argument for number of containers
NUM_CONTAINERS="${1:-$DEFAULT_NODES}"

# Validate it's a positive integer
if ! [[ "$NUM_CONTAINERS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Error: num_containers must be a positive integer. Got: '$NUM_CONTAINERS'"
    exit 2
fi

# ---------------------------------------------------------
# Main & cleanup
# ---------------------------------------------------------

# Define the cleanup function
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

# Set the trap to call the 'cleanup' function on any exit (including Ctrl+C)
trap cleanup EXIT

# Create and clear the logs directory
echo "Creating/clearing ./logs directory..."
mkdir -p ./logs
rm -f ./logs/*.csv

# Build and run the containers in detached mode
echo "Building and starting $NUM_CONTAINERS container(s)..."
docker compose up -d --build --scale relay-node="$NUM_CONTAINERS"

# Stream the logs in the background
echo "Streaming logs... (Will auto-stop when containers finish)"
docker compose logs -f &

# Get the Process ID (PID) of the background log command
LOGS_PID=$!

# Wait for the logger to exit naturally.
#    This is the main "foreground" task.
#    The logger will only exit when the containers stop.
wait $LOGS_PID

# The script ends naturally here.
echo "Containers finished and logger exited."
# The 'trap cleanup EXIT' will run automatically right after this.
