# TDMA Relay Network Implementation

An implementation of the TDMA-based routing protocol from the paper "Routing in Mobile Cyber-Physical Systems" by Luis Oliveira & Luis Almeida. This C implementation using Docker containers demonstrates synchronized multi-hop message routing where nodes can forward messages within the sender's time slot, enabling efficient relay communication across a network topology.

## Overview

This implementation creates a network of relay nodes that communicate using TDMA scheduling as described in the "Routing in Mobile Cyber-Physical Systems" paper. Each node gets a dedicated time slot (5 seconds) to transmit messages, preventing collisions. The key innovation is **within-slot forwarding**: when a relay node receives a message during another node's time slot and it's not the final destination, it immediately forwards the message within that same time slot, enabling multi-hop communication without waiting for its own turn.

### Key Features

- **TDMA Synchronization**: Nodes take turns broadcasting using 5-second time slots
- **Within-Slot Message Forwarding**: Relay nodes forward messages during the sender's time slot for efficient multi-hop routing
- **Per-Source Message Queues**: Each node maintains separate queues for messages from each source node
- **Visibility Matrix**: Configurable topology defining which nodes can communicate directly
- **Shared Clock**: All nodes use the host system's clock for synchronization
- **Comprehensive Logging**: CSV logs with microsecond timestamps for analysis
- **Dockerized**: Runs in isolated containers with static IP assignments

## Architecture

### Network Topology

The default configuration creates a linear chain of 5 nodes (defined in [main.c:34-45](main.c#L34-L45)):

```
node-0 → node-1 → node-2 → node-3 → node-4
```

- **node-0** can see node-1
- **node-1** can see node-2
- **node-2** can see node-3
- **node-3** can see node-4
- **node-4** is the end of the chain

### TDMA Scheduling with Within-Slot Forwarding

Nodes operate in a round-robin schedule with immediate message relay:

**Example Flow:**
1. **Node 0's turn** (5 seconds):
   - Broadcasts SYNC packet to announce its turn
   - Sends MSG packets destined for node-4
   - Node-1 receives the message (node-0 → node-1)

2. **Still during Node 0's slot**:
   - Node-1 sees the message is not for itself (destination is node-4)
   - Node-1 **immediately forwards** the message to node-2 (within node-0's time slot)
   - Node-2 forwards to node-3, node-3 forwards to node-4
   - All forwarding happens within node-0's 5-second window

3. **Node 1's turn** begins after node-0's slot expires:
   - Node-1 broadcasts its SYNC packet
   - Sends its own messages, and the cycle continues

This within-slot forwarding mechanism (from the Oliveira & Almeida paper) enables messages to traverse multiple hops during a single node's transmission window, significantly reducing end-to-end latency compared to waiting for each relay node's individual time slot.

### Message Types

1. **SYNC Messages**: `SYNC|<node_id>`
   - Broadcast at the start of each node's time slot
   - Used by other nodes to synchronize their turn timers

2. **MSG Messages**: `MSG|<src_id>|DST|<dst_id>|<payload>`
   - Contains source node, destination node, and message payload
   - Nodes forward messages not addressed to them

## Running the Project

### Prerequisites

- Docker and Docker Compose
- Bash shell

### Quick Start

Run with default 5 nodes:
```bash
./run-multi.sh
```

Run with custom number of nodes (e.g., 8 nodes):
```bash
./run-multi.sh 8
```

**Note**: If you change the number of nodes, you must update the visibility matrix in [main.c:34-45](main.c#L34-L45) accordingly.

### What Happens

1. `generate-compose.sh` creates a `docker-compose.yml` with N containers
2. Each container gets a static IP: `172.20.0.2` through `172.20.0.(N+1)`
    - So the names given in docker line up with the node numbers in the program
3. All containers share the host's clock via the Docker bridge network
4. Nodes discover each other via DNS (using the `relay-node` alias)
5. Nodes self-organize by sorting IPs to determine their slot order
6. TDMA communication begins with node-0 as the leader

### Stopping the Network

Press `Ctrl+C` to gracefully shut down. The script will:
1. Stop all containers
2. Merge individual logs into `./logs/merged_log.csv`
3. Clean up Docker resources

## Log Analysis

### Individual Node Logs

Location: `./logs/node_<id>.csv`

Each node writes CSV logs with the following columns:
- **Timestamp**: High-precision Unix timestamp (microseconds)
- **NodeID**: The node's assigned index (0-based)
- **Type**: Event type (INIT, SEND, RECV, SYNC, ENQUEUE, FORWARD, ERROR)
- **Message**: Event description

### Merged Logs

Location: `./logs/merged_log.csv`

After stopping the network with `Ctrl+C`, all individual logs are combined and sorted by timestamp. This allows you to see the global timeline of events across all nodes.

### Log Event Types

- `START`: Log file opened
- `INIT`: Initialization events (socket binding, thread creation)
- `SYNC`: TDMA synchronization events (timer scheduled, turn granted)
- `SEND`: Outgoing SYNC or MSG packets
- `RECV`: Received message addressed to this node
- `ENQUEUE`: Message queued for forwarding
- `FORWARD`: Relaying a queued message
- `ERROR`: Error conditions

## Configuration

### Time Slot Duration

Edit [main.c:23](main.c#L23):
```c
#define SYNC_WINDOW_SECONDS 5  // Change to desired slot duration
```

### Visibility Matrix

Edit the matrix in [main.c:34-45](main.c#L34-L45):
```c
static const int visibility_matrix[MAX_NODES][MAX_NODES] = {
    {0, 1, 0, 0, 0},  // node-0 can see node-1
    {0, 0, 1, 0, 0},  // node-1 can see node-2
    // ... add more rows for additional nodes
};
```

**Important**: Ensure `MAX_NODES` ([main.c:20](main.c#L20)) is large enough for your network size.

### Network Parameters

- **Port**: 8080 (UDP) - [main.c:19](main.c#L19)
- **Subnet**: 172.20.0.0/16 - [generate-compose.sh:42](generate-compose.sh#L42)
- **Message Buffer**: 1024 bytes - [main.c:21](main.c#L21)

## How It Works

### Node Discovery
1. Nodes wait 2 seconds for all containers to start
2. Each node queries DNS for all IPs with the `relay-node` alias
3. IPs are sorted to establish a consistent node ordering
4. Each node determines its index based on its IP position

### Clock Synchronization
All containers share the host system's clock through Docker's bridge network. Nodes use `gettimeofday()` to get high-precision timestamps for TDMA slot calculations.

### Message Forwarding (Within-Slot Relay)

This implementation follows the routing protocol from Oliveira & Almeida's paper. Each node maintains a separate message queue for each source node ([main.c:63](main.c#L63)):

```c
Queue *buffers[MAX_NODES];  // One queue per source node
```

**Forwarding Logic:**

1. **Receiving a message** ([main.c:419-426](main.c#L419-L426)):
   - When a node receives a MSG packet, it checks the destination
   - If the destination matches its own ID → process and log as RECV
   - If destination is different → enqueue in the source node's queue

2. **Forwarding during sender's slot** ([main.c:270-285](main.c#L270-L285)):
   - While waiting for its own turn, nodes poll their queues every 10ms
   - If messages exist in the current sender's queue, **immediately forward them**
   - This happens during the original sender's time slot, not the relay's slot

3. **Multi-hop example**:
   - Node-0 sends to Node-4: `MSG|0|DST|4|Hello from node 0!`
   - Node-1 receives it, enqueues in `buffers[0]` (source node-0's queue)
   - Within node-0's slot, node-1 polls and forwards to node-2
   - Node-2 → node-3 → node-4, all within node-0's 5-second window

This per-source queueing and within-slot forwarding enables efficient multi-hop routing as described in the paper.

### Thread Model
Each node runs two threads:
1. **Listener Thread**: Receives UDP packets, handles SYNC/MSG parsing, manages queues
2. **Main Thread**: Waits for turn signal, broadcasts SYNC, sends messages, forwards queued data during other nodes time slot

## References

This implementation is based on the routing protocol described in:

**"Routing in Mobile Cyber-Physical Systems"**
Luis Oliveira & Luis Almeida

The key contribution implemented here is the within-slot message forwarding mechanism, where relay nodes forward messages during the original sender's TDMA time slot rather than waiting for their own transmission window, significantly reducing end-to-end latency in multi-hop networks.
