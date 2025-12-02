#!/bin/bash
# This script runs N relay-node containers using Docker Compose.
# Usage:
#   ./run-multi.sh -n NUM_CONTAINERS [-runonce]
#     -n NUM_CONTAINERS: number of containers (default: 5)
#     -runonce: if set, nodes exit after one send window (default: on)

# ----------------------------------------------------------------------------------
# This script builds and starts NUM_CONTAINERS instances of the `relay-node`
# service and it streams logs while the containers run. All containers and networks
# are removed when the relay processes exit normally or when you press Ctrl+C.
# ----------------------------------------------------------------------------------

# Parse flags: -n <num> and optional -runonce
NUM_CONTAINERS=5
# Default behavior is to unlimited; unset with no -runonce flag sets to 1
RUN_ONCE=0

# Ensure cleanup only runs once
CLEANED_UP=0

usage() {
    echo "Usage: $0 -n NUM_CONTAINERS [-runonce]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n)
            if [[ -n "$2" && "$2" != -* ]]; then
                NUM_CONTAINERS="$2"
                shift 2
            else
                echo "Error: -n requires a numeric argument"
                usage
            fi
            ;;
        -runonce)
            RUN_ONCE=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Validate NUM_CONTAINERS is a positive integer
if ! [[ "$NUM_CONTAINERS" =~ ^[0-9]+$ ]] || [ "$NUM_CONTAINERS" -le 0 ]; then
    echo "Error: NUM_CONTAINERS must be a positive integer"
    usage
fi

# 1. Define the cleanup function (called on Ctrl+C)
cleanup() {
    # Prevent double cleanup from multiple traps
    if [ "$CLEANED_UP" -eq 1 ]; then
         return
    fi
    CLEANED_UP=1

    echo "" # Add a newline for readability

    # Always run docker compose down. Stops any running containers
    # and removes them along with networks.
    echo "Stopping and removing containers..."
    docker compose down

    # Combine the logs.
    echo "Combining node logs into ./logs/merged_log.csv..."
    if ls ./logs/node_*.csv >/dev/null 2>&1; then
        # Remove per-file headers, sort, and write merged output
        cat ./logs/node_*.csv | grep -v "^Timestamp" | sort > ./logs/merged_log.csv
    else
        echo "No node logs found; skipping merge."
    fi
    
    echo "Cleanup complete."
}

# 2. Set traps to call 'cleanup' on Ctrl+C, termination, or normal exit
trap cleanup SIGINT SIGTERM EXIT

# Create and clear the logs directory
echo "Creating/clearing ./logs directory..."
mkdir -p ./logs
rm -f ./logs/*.csv

# 3. Generate docker-compose.yml with N containers
echo "Generating docker-compose.yml for $NUM_CONTAINERS containers (RUN_ONCE=$RUN_ONCE)..."
./generate-compose.sh "$NUM_CONTAINERS" "$RUN_ONCE"

# 4. Build and start all containers
echo "Building and starting containers (RUN_ONCE=$RUN_ONCE)..."
# Build using args from compose and start containers
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
cleanup
echo "Containers finished."