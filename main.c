#include <stdio.h>      // For printf, perror, FILE, fopen, fprintf, snprintf
#include <stdlib.h>     // For getenv, exit, qsort
#include <string.h>     // For strcmp, memset, strncpy
#include <ifaddrs.h>    // For getifaddrs, freeifaddrs
#include <netdb.h>      // For getaddrinfo, freeaddrinfo, struct addrinfo
#include <unistd.h>     // For sleep
#include <arpa/inet.h>  // For inet_ntop, struct sockaddr_in
#include <sys/socket.h> // For AF_INET
#include <netinet/in.h> // For in_addr_t, INADDR_ANY, htonl, INADDR_LOOPBACK
#include <sys/time.h>   // For gettimeofday
#include <time.h>       // For formatting timestamps
#include <pthread.h>    // For pthread_create, pthread_join, mutex, cond
#include <errno.h>      // For ETIMEDOUT

// =============================================================================
// --- CONFIGURATION & CONSTANTS ---
// =============================================================================

#define PORT 8080                   // Port for all nodes to listen on
#define MAX_NODES 10 
#define LOG_FILENAME_FORMAT "/app/logs/node_%d.csv"

// --- SIMULATION SCALE FACTOR ---
// Baseline timings are 1000x slower than real-world for easier testing
// 1 = 1000x scale -> 1ms = 1000ms (1 second)
// 10 = 100x scale
// 100 = 10x scale
#define SCALE_FACTOR 100

// --- TDMA TIMING CONSTANTS ---
// Real World: 20ms slot (Standard Mesh Radio)
// Simulation: 20000ms slot (20 seconds)
#define SYNC_WINDOW_MS (20000 / SCALE_FACTOR)

// --- PACKET SIZING ---
#define PACKET_PAYLOAD_SIZE 1024

// --- SIMULATION SETTINGS (Wi-Fi) ---
// Real World: 0.5ms to 1.5ms (typical Wi-Fi latency)
// Simulation: 500ms to 1500ms
#define SIM_MIN_DELAY_MS    (500 / SCALE_FACTOR)
#define SIM_MAX_DELAY_MS    (1500 / SCALE_FACTOR)

// --- ALGORITHM SELECTION ---
// 1 = Paced (Sender waits for propagation)
// 2 = Burst (Sender dumps queue)
#define ALGORITHM_TYPE      2

// --- VIDEO SIMULATION METRICS ---
#define PACKETS_PER_FRAME   3

// =============================================================================
// --- DATA STRUCTURES ---
// =============================================================================

typedef enum {
    PACKET_TYPE_SYNC,
    PACKET_TYPE_MSG
} packet_type_t;

// Packet Structure (Replaces String Parsing)
typedef struct {
    packet_type_t type;
    int src_index;
    int dst_index;
    int slot_id;
    int msg_id;
    char payload[PACKET_PAYLOAD_SIZE];
} app_packet_t;

// Queue Node
typedef struct Node {
    app_packet_t packet;
    struct Node* next;
} node_t;

// Thread-Safe Queue Wrapper
typedef struct {
    node_t* front;
    node_t* rear;
    pthread_mutex_t lock; // Protects this specific queue
} thread_safe_queue_t;

// TDMA Synchronization State
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int ready_to_send;  // Flag (0 or 1)
    int timer_pending;  // Flag (0 or 1)
    int self_index;
    double schedule_anchor_time;   // Absolute time when the anchor node started its slot
    int schedule_anchor_index;     // Node index that owns the slot at schedule_anchor_time
    int queue_pending;  // Flag (0 or 1)
} sync_state_t;

// Arguments for the listener thread.
typedef struct {
    int sockfd;
    sync_state_t *sync_state;
} listener_args_t;

// =============================================================================
// --- GLOBALS ---
// =============================================================================

FILE *log_file = NULL;
struct sockaddr_in node_list[MAX_NODES]; // Holds all nodes (sorted)
int node_count = 0;
int self_index = -1; // Our unique, sorted ID (0 is "leader")
int cycle = 1;
thread_safe_queue_t *message_buffers[MAX_NODES]; // Array of queues (one per node)

// =============================================================================
// --- FUNCTION PROTOTYPES ---
// =============================================================================

// Queue
thread_safe_queue_t* create_queue();
void enqueue(thread_safe_queue_t* q, app_packet_t *packet);
int dequeue(thread_safe_queue_t* q, app_packet_t *out_packet);

// Helpers
int compare_nodes(const void *a, const void *b);
void log_event(const char *type, const char *message);
void log_packet_event(const char *type, app_packet_t *pkt, const char *extra);
int get_self_ip(in_addr_t *ip_numeric);
int get_node_index_for_addr(struct in_addr *sender_ip);
int get_next_node_index(int current_index);

// TDMA & Thread Sync
void wait_for_turn(int sockfd, sync_state_t *state);
void* turn_timer_thread(void* arg);
void schedule_turn_timer(sync_state_t *state);
void* listener_thread(void* arg);
void broadcast_sync_packet(int sockfd, int self_index);
void sendMessageToPeers(int sockfd, int self_index, int messageNumber);
void send_app_packet(int sockfd, app_packet_t *packet, int target_node_index);

// =============================================================================
// --- THREAD-SAFE QUEUE IMPLEMENTATION ---
// =============================================================================

// Function to create an empty queue
thread_safe_queue_t* create_queue() {
    thread_safe_queue_t* q = (thread_safe_queue_t*)malloc(sizeof(thread_safe_queue_t));
    q->front = NULL;
    q->rear = NULL;
    pthread_mutex_init(&q->lock, NULL);
    return q;
}

// Function to add an element to the queue (enqueue)
void enqueue(thread_safe_queue_t* q, app_packet_t *packet) {
    node_t* newNode = (node_t*)malloc(sizeof(node_t));
    newNode->packet = *packet; // Copy struct data
    newNode->next = NULL;

    pthread_mutex_lock(&q->lock);
    if (q->rear == NULL) { // If queue is empty
        q->front = newNode;
        q->rear = newNode;
    } else {
        q->rear->next = newNode;
        q->rear = newNode;
    }
    pthread_mutex_unlock(&q->lock);
}

// Function to remove an element from the queue (dequeue)
// Returns 1 if packet retrieved, 0 if empty
int dequeue(thread_safe_queue_t* q, app_packet_t *out_packet) {
    if (!q) return 0;   // <- safe-guard: empty/NULL queue means nothing to dequeue
    pthread_mutex_lock(&q->lock);
    if (q->front == NULL) { // If queue is empty
        // printf("Queue is empty!\n");
        pthread_mutex_unlock(&q->lock);
        return 0; // Indicate empty
    }
    
    node_t* temp = q->front;
    *out_packet = temp->packet; // Copy struct data

    q->front = q->front->next;

    if (q->front == NULL) { // If queue becomes empty after dequeue
        q->rear = NULL;
    }
    pthread_mutex_unlock(&q->lock);
    free(temp);  // Now safe to free
    return 1; // Indicate success
}

// =============================================================================
// --- GENERAL HELPER FUNCTIONS ---
// =============================================================================

/**
 * @brief Comparison function for qsort() to sort nodes by IP address.
 */
int compare_nodes(const void *a, const void *b) {
    struct sockaddr_in *node_a = (struct sockaddr_in *)a;
    struct sockaddr_in *node_b = (struct sockaddr_in *)b;
    if (node_a->sin_addr.s_addr < node_b->sin_addr.s_addr) return -1;
    if (node_a->sin_addr.s_addr > node_b->sin_addr.s_addr) return 1;
    return 0;
}

/**
 * @brief Logs an event to both the console (printf) and the CSV file.
 * @param type A short string (e.g., "INIT", "SEND", "RECV")
 * @param message The log message.
 */
void log_event(const char *type, const char *message) {
    // 1. Get high-resolution time
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double timestamp = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    timestamp = timestamp / 1000 * SCALE_FACTOR; // Scale time for simulation
    // 2. Format for console output
    printf("[Node %d] %s: %s\n", self_index, type, message);
    fflush(stdout);

    // 3. Write to CSV file with timestamp
    if (log_file) {
        fprintf(log_file, "%.6f,%d,%s,\"%s\"\n", timestamp, self_index, type, message);
        fflush(log_file);
    }
}

/**
 * @brief Logs a packet-related event with packet details.
 * @param type A short string (e.g., "SEND", "RECV", "FWD", "QUE")
 * @param pkt Pointer to the app_packet_t structure.
 * @param extra Additional message details.
 */
void log_packet_event(const char *type, app_packet_t *pkt, const char *extra) {
    char msg[512];
    snprintf(msg, sizeof(msg), "PKT %d->%d (S:%d M:%d) | %s", 
             pkt->src_index, pkt->dst_index, pkt->slot_id, pkt->msg_id, extra);
    log_event(type, msg);
}

/**
 * @brief Gets the first non-loopback IPv4 address of the host.
 * @param ip_numeric Output parameter for the numeric (in_addr_t) IP.
 * @return 0 on success, -1 on failure.
 */
int get_self_ip(in_addr_t *ip_numeric) {
    struct ifaddrs *ifaddr_list, *ifaddr_iter;
    if (getifaddrs(&ifaddr_list) == -1) {
        perror("getifaddrs"); // Can't log yet
        return -1;
    }
    for (ifaddr_iter = ifaddr_list; ifaddr_iter != NULL; ifaddr_iter = ifaddr_iter->ifa_next) {
        if (ifaddr_iter->ifa_addr == NULL) continue;
        if (ifaddr_iter->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifaddr_iter->ifa_addr;
            if (sa->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                *ip_numeric = sa->sin_addr.s_addr;
                freeifaddrs(ifaddr_list);
                return 0;
            }
        }
    }
    freeifaddrs(ifaddr_list);
    return -1;
}

/**
 * @brief Finds the sorted index (0, 1, 2...) for a given sender's IP.
 * @param sender_ip The in_addr struct of the sender.
 * @return The node's index if found, or -1.
 */
int get_node_index_for_addr(struct in_addr *sender_ip) {
    for (int i = 0; i < node_count; ++i) {
        if (node_list[i].sin_addr.s_addr == sender_ip->s_addr) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Calculates the index of the next node in the turn sequence.
 * @param current_index The index of the node that just finished.
 * @return The index of the next node.
 */
int get_next_node_index(int current_index) {
    if (node_count <= 0) return -1;
    return (current_index + 1) % node_count; // 0-based wrap-around
}

// =============================================================================
// --- TDMA & THREAD SYNCHRONIZATION ---
// =============================================================================

/**
 * @brief Blocks the main thread until its turn to send.
 */
void wait_for_turn(int sockfd, sync_state_t *state) {
    // Line Topology: N sends to N+1
    int link_node = self_index + 1;
    if (link_node >= node_count) link_node = -1; // End of line

    pthread_mutex_lock(&state->lock);
    while (1) {
        if (state->ready_to_send) {
            state->ready_to_send = 0; // Consume the "turn"
            pthread_mutex_unlock(&state->lock);
            return;
        }

        if (state->queue_pending) {
            state->queue_pending = 0;
            int anchor = state->schedule_anchor_index;
            pthread_mutex_unlock(&state->lock);

            // ALGO 1: Paced/Immediate Forwarding
            // In Algo 1, we process queues immediately while waiting for our turn (Interrupt driven)
            if (ALGORITHM_TYPE == 1) {
                if (link_node != -1 && anchor >= 0 && anchor < MAX_NODES && message_buffers[anchor]) {
                    app_packet_t packet;
                    while (dequeue(message_buffers[anchor], &packet)) {
                        log_packet_event("FWD", &packet, "Forwarding (Interrupt)");
                        send_app_packet(sockfd, &packet, link_node);
                    }
                }
            }
            pthread_mutex_lock(&state->lock);
            continue;
        }
        pthread_cond_wait(&state->cond, &state->lock);
    }
}

/**
 * @brief (Thread) Waits for SYNC_WINDOW_SECONDS, then signals the main
 * thread it's this node's turn to send.
 */
void* turn_timer_thread(void* arg) {
    sync_state_t *state = (sync_state_t *)arg;
    usleep(SYNC_WINDOW_MS * 1000);

    log_event("SYNC", "Timer expired, my turn to send.");

    pthread_mutex_lock(&state->lock);
    state->ready_to_send = 1;
    state->timer_pending = 0;
    pthread_cond_signal(&state->cond);
    pthread_mutex_unlock(&state->lock);

    return NULL;
}

/**
 * @brief Creates a detached thread to start the turn timer.
 */
void schedule_turn_timer(sync_state_t *state) {
    pthread_mutex_lock(&state->lock);
    if (state->timer_pending) {
        // Timer is already running, don't start another.
        pthread_mutex_unlock(&state->lock);
        return;
    }
    state->timer_pending = 1;
    pthread_mutex_unlock(&state->lock);

    log_event("SYNC", "Timer scheduled.");

    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, turn_timer_thread, state) != 0) {
        log_event("ERROR", "Failed to create timer thread");
        pthread_mutex_lock(&state->lock);
        state->timer_pending = 0; // Reset flag on failure
        pthread_mutex_unlock(&state->lock);
        return;
    }
    pthread_detach(timer_thread);
}

/**
 * @brief (Thread) The main receiver loop. Parses SYNC and MSG packets.
 * When a SYNC packet indicates it's this node's turn next,
 * it schedules the turn timer.
 */
void* listener_thread(void* arg) {
    listener_args_t* args = (listener_args_t*)arg;
    int sockfd = args->sockfd;
    sync_state_t *sync_state = args->sync_state;

    char recv_buffer[sizeof(app_packet_t)];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    log_event("INIT", "Listener thread started.");

    while (1) {
        int bytes_received = recvfrom(sockfd, recv_buffer, sizeof(recv_buffer), 0,
                                      (struct sockaddr*)&sender_addr, &addr_len);
        if (bytes_received < 0) {
            log_event("ERROR", "recvfrom() failed");
            continue;
        }

        // Get sender's info from sender_addr
        char sender_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip_str, sizeof(sender_ip_str));
        int sender_index = get_node_index_for_addr(&sender_addr.sin_addr);

        if (sender_index < 0) {
            // Unknown sender (not in sorted node_list). Log and ignore
            char warn_msg[128];
            snprintf(warn_msg, sizeof(warn_msg), "Received packet from unknown IP %s; ignoring.", sender_ip_str);
            log_event("WARN", warn_msg);
            continue;
        }

        // Validate packet size
        if (bytes_received != sizeof(app_packet_t)) {
            log_event("WARN", "Received packet of incorrect size. Ignoring.");
            continue;
        }

        // Parse the received packet into struct app_packet_t
        app_packet_t *packet = (app_packet_t *)recv_buffer;

        if (packet->type == PACKET_TYPE_SYNC) {
            // Listener keeps a shared TDMA timeline by decoding SYNC broadcasts
            struct timeval now;
            gettimeofday(&now, NULL);
            double now_sec = (double)now.tv_sec + (double)now.tv_usec / 1e6;

            int schedule_self = 0;

            pthread_mutex_lock(&sync_state->lock);
            // Store the anchor so downstream logic can compute any node's slot without extra parsing
            sync_state->schedule_anchor_index = packet->src_index;
            sync_state->schedule_anchor_time = now_sec;
            schedule_self = (get_next_node_index(packet->src_index) == sync_state->self_index);
            pthread_mutex_unlock(&sync_state->lock);

            if (schedule_self) {
                char log_msg[64];
                snprintf(log_msg, sizeof(log_msg), "Received SYNC from %d on cycle %d. Scheduling timer.", cycle, packet->src_index);
                log_event("SYNC", log_msg);
                schedule_turn_timer(sync_state);
            }
        } // End of packet SYNC handling
        else if (packet->type == PACKET_TYPE_MSG) {
            // Message Routing Logic
            if (packet->dst_index == self_index) {
                // It's for us!
                log_packet_event("RECV", packet, "Arrived at destination");
            } else {
                // It's for someone else. Queue it.
                if (packet->src_index >= 0 && packet->src_index < node_count) {
                    log_packet_event("QUE", packet, "Buffered");
                    if (message_buffers[packet->src_index]) {
                        enqueue(message_buffers[packet->src_index], packet);
                        // Notify main thread that there's a queued packet to forward
                        pthread_mutex_lock(&sync_state->lock);
                        sync_state->queue_pending = 1;
                        pthread_cond_signal(&sync_state->cond);
                        pthread_mutex_unlock(&sync_state->lock);
                    }
                }
            }
        } // End of packet MSG handling
    } // End of while(1)
    return NULL;
}

// =============================================================================
// --- NETWORK SENDING FUNCTIONS ---
// =============================================================================

/**
 * @brief Helper to send a struct packet to a specific node index
 */
void send_app_packet(int sockfd, app_packet_t *packet, int target_node_index) {
    if (target_node_index < 0 || target_node_index >= node_count) return;
    
    // --- SIMULATION: NETWORK DELAY (JITTER) ---
    // Calculate a random delay between MIN and MAX
    int delay_ms = SIM_MIN_DELAY_MS + (rand() % (SIM_MAX_DELAY_MS - SIM_MIN_DELAY_MS + 1));
    // Blocking sleep to simulate transmission time/propagation delay
    usleep(delay_ms * 1000);

    sendto(sockfd, packet, sizeof(app_packet_t), 0,
           (struct sockaddr *)&node_list[target_node_index], sizeof(node_list[target_node_index]));
}

/**
 * @brief Broadcasts a "SYNC" packet to all other nodes.
 */
void broadcast_sync_packet(int sockfd, int self_index) {
    log_event("SYNC", "Broadcasting SYNC packet.");

    app_packet_t packet;
    packet.type = PACKET_TYPE_SYNC;
    packet.src_index = self_index;
    packet.dst_index = -1; // Broadcast
    packet.slot_id = 0;
    packet.msg_id = 0;
    memset(packet.payload, 0, sizeof(packet.payload));

    for (int i = 0; i < node_count; ++i) {
        if (i == self_index) continue; // Don't send to self
        send_app_packet(sockfd, &packet, i);
    }
}

/**
 * @brief Sends a regular "MSG" packet to all other nodes.
 */
void sendMessageToPeers(int sockfd, int self_index, int messageNumber) {
    app_packet_t packet;
    packet.type = PACKET_TYPE_MSG;
    packet.src_index = self_index;
    packet.dst_index = node_count-1; // For simplicity, send to last node (can be changed)
    packet.slot_id = cycle; // should not have any concurrency issues
    packet.msg_id = messageNumber;
    snprintf(packet.payload, sizeof(packet.payload), "Hello from node %d! Slot #%d message #%d", self_index, cycle, messageNumber);

    if (self_index < 0 || self_index >= MAX_NODES) {
        log_event("ERROR", "Invalid self index for visibility matrix.");
        return;
    }

    int link_node = self_index+1; // Default to next node in list
    if (link_node >= node_count) {
        // No further nodes to send to
        // log_event("INFO", "No further nodes to send MSG packet to.");
        return;
    }

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "PKT %d->%d (S:%d M:%d) | Originating packet", 
                packet.src_index, packet.dst_index, packet.slot_id, packet.msg_id);
    log_event("SEND", log_msg);

    send_app_packet(sockfd, &packet, link_node);
}

int forwardMessageFromQueue(int sockfd, int self_index) {
    if (self_index < 0 || self_index >= MAX_NODES) {
        log_event("ERROR", "Invalid self index for visibility matrix.");
        return 0;
    }

    int link_node = self_index+1; // Default to next node in list
    if (link_node >= node_count) {
        // No further nodes to send to
        // log_event("INFO", "No further nodes to send MSG packet to.");
        return 0;
    }

    // Static round-robin index: persists across calls
    static int rr_index = 0;

    app_packet_t packet;
    for (int i = 0; i < node_count; i++) { // Check all queues
        if (dequeue(message_buffers[rr_index], &packet)) {
            log_packet_event("FWD", &packet, "Forwarding (Drain)");
            send_app_packet(sockfd, &packet, link_node);
            // Advance round-robin index for next call
            rr_index = (rr_index + 1) % node_count;
            return 1; // Sent one packet
        }
        // No packet in this queue, check next
        rr_index = (rr_index + 1) % node_count;
    }
    return 0; // No packets to send
}

// =============================================================================
// --- MAIN EXECUTION ---
// =============================================================================

int main() {
    // 0. Read TOTAL_ROUNDS from environment (number of send windows per node).
    // If TOTAL_ROUNDS is unset or <=0 the node will run indefinitely.
    const char *total_runs_env = getenv("TOTAL_ROUNDS");
    int TOTAL_ROUNDS = -1; // -1 => run indefinitely
    if (total_runs_env) {
        int v = atoi(total_runs_env);
        if (v > 0) TOTAL_ROUNDS = v;
    }

    // 1. Get service name
    const char* service_name = getenv("SERVICE_NAME");
    if (service_name == NULL) {
        perror("Error: SERVICE_NAME environment variable not set.");
        exit(1);
    }

    // 2. Get our own numeric IP
    in_addr_t self_ip_numeric;
    if (get_self_ip(&self_ip_numeric) != 0) {
        perror("Error: Could not find self IP address.");
        exit(1);
    }

    // Convert self IP to string for initial printing
    char self_ip_str[INET_ADDRSTRLEN];
    struct in_addr self_in_addr = {.s_addr = self_ip_numeric};
    inet_ntop(AF_INET, &self_in_addr, self_ip_str, sizeof(self_ip_str));
    
    printf("--- My IP: %s ---\n", self_ip_str);
    printf("Waiting 2 seconds for other peers to start...\n");
    fflush(stdout);
    sleep(2); 

    // 3. DNS Lookup
    struct addrinfo hints, *addrinfo_list, *addrinfo_iter;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int status = getaddrinfo(service_name, NULL, &hints, &addrinfo_list);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        exit(1);
    }

    // 4. Populate the global node_list
    for (addrinfo_iter = addrinfo_list; addrinfo_iter != NULL; addrinfo_iter = addrinfo_iter->ai_next) {
        if (node_count < MAX_NODES) {
            struct sockaddr_in *sa = (struct sockaddr_in *)addrinfo_iter->ai_addr;
            sa->sin_port = htons(PORT); 
            node_list[node_count] = *sa;
            node_count++;
        }
    }
    freeaddrinfo(addrinfo_list);

    if (node_count == 0) {
        fprintf(stderr, "Error: Could not discover any peers.\n");
        exit(1);
    }

    // 5. Sort the node list and find our index
    qsort(node_list, node_count, sizeof(struct sockaddr_in), compare_nodes);

    printf("--- Sorted Node List ---\n");
    for (int i = 0; i < node_count; i++) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(node_list[i].sin_addr), ip_str, sizeof(ip_str));
        
        if (node_list[i].sin_addr.s_addr == self_ip_numeric) {
            self_index = i;
            printf("- Node %d: %s (ME)\n", i, ip_str);
        } else {
            printf("- Node %d: %s\n", i, ip_str);
        }
        // Ensure a queue exists for every index (including self) to avoid NULL derefs
        message_buffers[i] = create_queue();
    }
    printf("---------------------------\n");
    fflush(stdout);

    if (self_index < 0) {
        fprintf(stderr, "Error: Self IP %s not found in peer list.\n", self_ip_str);
        exit(1);
    }

    // 6. Open our unique log file
    char log_filename[64];
    snprintf(log_filename, sizeof(log_filename), LOG_FILENAME_FORMAT, self_index);
    log_file = fopen(log_filename, "w");
    if (log_file == NULL) {
        perror("fopen log_file failed");
        exit(1);
    }
    fprintf(log_file, "Timestamp,NodeID,Type,Message\n");
    fflush(log_file);
    log_event("START", "Log file opened.");

    // 7. Create and Bind UDP Socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { log_event("ERROR", "socket() failed"); exit(1); }

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(PORT);
    local_addr.sin_addr.s_addr = INADDR_ANY; 

    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        log_event("ERROR", "bind() failed");
        exit(1);
    }
    log_event("INIT", "Socket bound to port 8080");

    // 8. Initialize sync state
    sync_state_t sync_state;
    pthread_mutex_init(&sync_state.lock, NULL);
    pthread_cond_init(&sync_state.cond, NULL);
    sync_state.self_index = self_index;
    sync_state.timer_pending = 0;
    sync_state.ready_to_send = (self_index == 0); // Node 0 starts
    sync_state.schedule_anchor_index = self_index;
    sync_state.schedule_anchor_time = 0.0; // Will be updated as soon as the first SYNC flows

    // 9. Start listener thread FIRST (before sending)
    pthread_t listener;
    listener_args_t args = {sockfd, &sync_state};
    if (pthread_create(&listener, NULL, listener_thread, &args) != 0) {
        log_event("ERROR", "Failed to create listener thread.");
        exit(1);
    }
    //pthread_detach(listener);
    
    // Small delay to ensure all listeners are ready
    log_event("SYNC", "Waiting for all listeners to be ready...");
    sleep(2);

    // Seed the random number generator for latency simulation
    srand(time(NULL) ^ self_ip_numeric);

    // Pre-calculate estimated traversal time for one-way message forwarding
    int forward_hop_count = node_count - 2; // additional hops beyond direct neighbor
    int estimated_forward_time_us = SIM_MAX_DELAY_MS * forward_hop_count * 1000; // in microseconds

    // 10. Main thread becomes the "Sender"
    while (1) {
        // Wait for our turn OR forward queued packets
        wait_for_turn(sockfd, &sync_state);

        // Get the time our window starts
        struct timeval start_time;
        gettimeofday(&start_time, NULL);
        double start_time_sec = (double)start_time.tv_sec + (double)start_time.tv_usec / 1e6;
        
        // Log entry into send window
        log_event("INFO", "Entering send window.");
        pthread_mutex_lock(&sync_state.lock);
        // advertise that this node is the current anchor so receivers can recalc the slot table
        sync_state.schedule_anchor_index = self_index;
        sync_state.schedule_anchor_time = start_time_sec;
        pthread_mutex_unlock(&sync_state.lock);
        
        // It's our turn, begin broadcast with SYNC
        broadcast_sync_packet(sockfd, self_index);

        int messageNumber = 1;

        // Continue sending MSGs until our window ends. This loop replaces perform_send_window()
        while(1) {
            struct timeval now;
            gettimeofday(&now, NULL);
            double now_sec = (double)now.tv_sec + (double)now.tv_usec / 1e6;

            if ((now_sec - start_time_sec) >= (double) SYNC_WINDOW_MS / 1000) break;

            if (messageNumber <= PACKETS_PER_FRAME) {
                sendMessageToPeers(sockfd, self_index, messageNumber);
                messageNumber++;
                // --- SLEEP LOGIC ---
                if (ALGORITHM_TYPE == 1) {
                    // ALGO 1: Wait for full network traversal to allow interrupts to clear path
                    usleep(estimated_forward_time_us); 
                } else {
                    // ALGO 2: Burst Mode
                    // We assume packets were buffered while waiting for our turn.
                    // We send them back-to-back (limited only by link delay in send_app_packet).
                    // usleep(1000); // Minimal inter-packet processing delay
                }
            } else {
                // Frame complete. 
                if (ALGORITHM_TYPE == 1) {
                    // Algo 1: Exit send window after sending all packets, wait_for_turn() will sync to next turn
                    break;
                }
                else if (ALGORITHM_TYPE == 2) {
                    // Algo 2: Drain Logic (Store-and-Forward)
                    if (!forwardMessageFromQueue(sockfd, self_index)) {
                        // No more messages to forward, exit send window
                        break;
                    }
                }
            }
        }
        // keep track of every time this node's slot has run
        cycle++;

        // Finished this node's send window
        log_event("INFO", "Finished send window.");

        // If TOTAL_ROUNDS is set (>0), exit after this node has completed that many windows
        if (TOTAL_ROUNDS > 0 && cycle > TOTAL_ROUNDS) {
            // Give a short grace period for background logging/network flush
            sleep(1);
            break;
        }
    }

    // Stop listener thread
    pthread_cancel(listener); // Request cancellation (recvfrom() is a cancellation point)
    pthread_join(listener, NULL);

    // log all remaining messages in queues
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (i == self_index || message_buffers[i] == NULL) continue;
        app_packet_t packet;
        // Dequeue all pending packets from the anchor's buffer
        while (dequeue(message_buffers[i], &packet)) {
            char log_msg[256];        
            snprintf(log_msg, sizeof(log_msg), "PKT %d->%d (S:%d M:%d) | Message stuck in queue after exit.", 
                            packet.src_index, packet.dst_index, packet.slot_id, packet.msg_id);
                    log_event("QUEWARN", log_msg);
        }
    }

    // Cleanup
    fclose(log_file);
    close(sockfd);
    pthread_mutex_destroy(&sync_state.lock);
    pthread_cond_destroy(&sync_state.cond);

    return 0;
}
