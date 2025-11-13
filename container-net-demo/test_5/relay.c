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

// =============================================================================
// --- CONFIGURATION & GLOBALS ---
// =============================================================================

#define PORT 8080                   // Port for all nodes to listen on
#define MAX_NODES 10 
#define BUFFER_SIZE 1024
#define LOG_FILENAME_FORMAT "/app/logs/node_%d.csv"
#define SYNC_WINDOW_SECONDS 10      // Time (in sec) for each node's "turn"
#define SYNC_PREFIX "SYNC|"
#define MSG_PREFIX  "MSG|"

// --- Globals ---
FILE *log_file = NULL;
struct sockaddr_in node_list[MAX_NODES]; // Holds all nodes (sorted)
int node_count = 0;
int self_index = -1; // Our unique, sorted ID (0 is "leader")

/**
 * @brief Thread synchronization state for TDMA.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int ready_to_send;  // Flag (0 or 1)
    int timer_pending;  // Flag (0 or 1)
    int self_index;
} sync_state_t;

/**
 * @brief Arguments for the listener thread.
 */
typedef struct {
    int sockfd;
    sync_state_t *sync_state;
} listener_args_t;

// --- Function Prototypes ---
int compare_nodes(const void *a, const void *b);
void log_event(const char *type, const char *message);
int get_self_ip(in_addr_t *ip_numeric);
int get_node_index_for_addr(struct in_addr *sender_ip);
int get_next_node_index(int current_index);
void wait_for_turn(sync_state_t *state);
void* turn_timer_thread(void* arg);
void schedule_turn_timer(sync_state_t *state);
void* listener_thread(void* arg);
void broadcast_sync_packet(int sockfd, int self_index);
void sendMessageToPeers(int sockfd, int self_index);
void perform_send_window(int sockfd, int self_index);

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
void wait_for_turn(sync_state_t *state) {
    pthread_mutex_lock(&state->lock);
    while (!state->ready_to_send) {
        pthread_cond_wait(&state->cond, &state->lock);
    }
    state->ready_to_send = 0; // Consume the "turn"
    pthread_mutex_unlock(&state->lock);
}

/**
 * @brief (Thread) Waits for SYNC_WINDOW_SECONDS, then signals the main
 * thread it's this node's turn to send.
 */
void* turn_timer_thread(void* arg) {
    sync_state_t *state = (sync_state_t *)arg;
    sleep(SYNC_WINDOW_SECONDS);

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

    char recv_buffer[BUFFER_SIZE];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    log_event("INIT", "Listener thread started.");

    while (1) {
        int bytes_received = recvfrom(sockfd, recv_buffer, sizeof(recv_buffer) - 1, 0,
                                      (struct sockaddr*)&sender_addr, &addr_len);

        if (bytes_received < 0) {
            log_event("ERROR", "recvfrom() failed");
            continue;
        }
        
        recv_buffer[bytes_received] = '\0'; // Null-terminate

        // Get sender's info
        char sender_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip_str, sizeof(sender_ip_str));
        int sender_index = get_node_index_for_addr(&sender_addr.sin_addr);

        char log_msg[BUFFER_SIZE];

        if (strncmp(recv_buffer, SYNC_PREFIX, strlen(SYNC_PREFIX)) == 0) {
            int sync_sender_index = atoi(recv_buffer + strlen(SYNC_PREFIX));
            snprintf(log_msg, sizeof(log_msg), "Received SYNC from Node %d (%s)",
                     sync_sender_index, sender_ip_str);
            log_event("RECV", log_msg);

            int next_node_index = get_next_node_index(sync_sender_index);
            if (next_node_index == sync_state->self_index) {
                schedule_turn_timer(sync_state);
            }
        } else {
             snprintf(log_msg, sizeof(log_msg), "Received from Node %d (%s): \"%s\"",
                     sender_index, sender_ip_str, recv_buffer);
             log_event("RECV", log_msg);
        }
    }
    return NULL;
}

// =============================================================================
// --- NETWORK SENDING FUNCTIONS ---
// =============================================================================

/**
 * @brief Broadcasts a "SYNC" packet to all other nodes.
 */
void broadcast_sync_packet(int sockfd, int self_index) {
    log_event("SEND", "Broadcasting SYNC packet.");

    char message[64];
    snprintf(message, sizeof(message), SYNC_PREFIX "%d", self_index);

    for (int i = 0; i < node_count; ++i) {
        if (i == self_index) continue; // Don't send to self

        sendto(sockfd, message, strlen(message), 0, 
               (struct sockaddr *)&node_list[i], sizeof(node_list[i]));
    }
}

/**
 * @brief Sends a regular "MSG" packet to all other nodes.
 */
void sendMessageToPeers(int sockfd, int self_index) {
    log_event("SEND", "Broadcasting MSG packet.");
    
    char message[128];
    snprintf(message, sizeof(message), MSG_PREFIX "Hello from node %d", self_index);

    for (int i = 0; i < node_count; ++i) {
        if (i == self_index) continue; // Don't send to self
        sendto(sockfd, message, strlen(message), 0, 
               (struct sockaddr *)&node_list[i], sizeof(node_list[i]));
    }
}

/**
 * @brief The main action for a node's "turn": broadcast SYNC, then MSG.
 */
void perform_send_window(int sockfd, int self_index) {
    broadcast_sync_packet(sockfd, self_index);
    sendMessageToPeers(sockfd, self_index);
}

// =============================================================================
// --- MAIN EXECUTION ---
// =============================================================================

int main() {
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
    printf("Waiting 5 seconds for other peers to start...\n");
    fflush(stdout);
    sleep(5); 

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

    // 9. Start listener thread FIRST (before sending)
    pthread_t listener;
    listener_args_t args = {sockfd, &sync_state};
    if (pthread_create(&listener, NULL, listener_thread, &args) != 0) {
        log_event("ERROR", "Failed to create listener thread.");
        exit(1);
    }
    
    // Small delay to ensure all listeners are ready
    log_event("SYNC", "Waiting 10s for all nodes to be ready...");
    sleep(10);
    
    log_event("INIT", "Entering synchronized send loop.");

    // 10. Main thread becomes the "Sender"
    while (1) {
        wait_for_turn(&sync_state);
        perform_send_window(sockfd, self_index);
    }

    return 0;
}