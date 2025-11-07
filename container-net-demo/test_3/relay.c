#include <stdio.h>      // For printf, perror
#include <stdlib.h>     // For getenv, exit, malloc
#include <string.h>     // For strcmp, memset, strftime
#include <ifaddrs.h>    // For getifaddrs, freeifaddrs
#include <netdb.h>      // For getaddrinfo, freeaddrinfo, struct addrinfo
#include <unistd.h>     // For sleep
#include <arpa/inet.h>  // For inet_ntop, struct sockaddr_in
#include <sys/socket.h> // For AF_INET
#include <time.h>       // For time, localtime
#include <pthread.h>    // For pthread_create, pthread_join
#include <sys/time.h>   // For gettimeofday
#include <stdint.h>     // For uint32_t

#define SYNC_PREFIX "SYNC|"
#define MSG_PREFIX  "MSG|"
#define SYNC_WINDOW_SECONDS 10

// Struct for node information
typedef struct {
    char ip[INET_ADDRSTRLEN];
    uint32_t addr_host_order;
    uint32_t addr_network_order;
    int node_number;
} peer_info_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int ready_to_send;
    int timer_pending;
    int self_node_number;
} sync_state_t;

// Helper function to get this container's own IP address
// Returns 0 on success, -1 on failure
int get_self_ip(char *ip_buffer, size_t buffer_size) {
    struct ifaddrs *ifaddr_list, *ifaddr_iter;

    // Get the list of all network interfaces
    if (getifaddrs(&ifaddr_list) == -1) {
        perror("getifaddrs");
        return -1;
    }

    // Walk through the linked list of interfaces
    for (ifaddr_iter = ifaddr_list; ifaddr_iter != NULL; ifaddr_iter = ifaddr_iter->ifa_next) {
        if (ifaddr_iter->ifa_addr == NULL) {
            continue;
        }

        // We only care about IPv4 addresses
        if (ifaddr_iter->ifa_addr->sa_family == AF_INET) {
            // Convert the address to a string
            struct sockaddr_in *sa = (struct sockaddr_in *)ifaddr_iter->ifa_addr;
            inet_ntop(AF_INET, &(sa->sin_addr), ip_buffer, buffer_size);
            
            // Ignore the loopback "127.0.0.1" address
            if (strcmp("127.0.0.1", ip_buffer) != 0) {
                freeifaddrs(ifaddr_list); // Clean up memory
                return 0; // Success! IP is in ip_buffer
            }
        }
    }

    freeifaddrs(ifaddr_list); // Clean up memory
    return -1; // Failed to find a non-loopback IP
}

char* getCurrentTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);  // Get current time (seconds + microseconds)

    struct tm *tm_info = localtime(&tv.tv_sec);

    char *buffer = malloc(32);
    if (!buffer) {
        fprintf(stderr, "Error: Could not allocate time string.\n");
        exit(1);
    }

    // Format base time
    strftime(buffer, 20, "%Y-%m-%d %H:%M:%S", tm_info);

    // Append microseconds (6 digits)
    sprintf(buffer + 19, ".%06ld", (long)tv.tv_usec);

    return buffer;
}

int compare_peer_info(const void *a, const void *b) {
    const peer_info_t *pa = (const peer_info_t *)a;
    const peer_info_t *pb = (const peer_info_t *)b;
    if (pa->addr_host_order < pb->addr_host_order) return -1;
    if (pa->addr_host_order > pb->addr_host_order) return 1;
    return 0;
}

peer_info_t* build_peer_map(struct addrinfo *addrinfo_list, int *peer_count) {

    // build a peer list with initial capacity of 4
    int capacity = 4;
    peer_info_t *peers = malloc(capacity * sizeof(peer_info_t));
    if (!peers) {
        perror("malloc");
        exit(1);
    }

    int count = 0;
    struct addrinfo *addrinfo_iter;
    for (addrinfo_iter = addrinfo_list; addrinfo_iter != NULL; addrinfo_iter = addrinfo_iter->ai_next) {
        
        // extract ip address
        struct sockaddr_in *sa = (struct sockaddr_in *)addrinfo_iter->ai_addr;
        uint32_t host_addr = ntohl(sa->sin_addr.s_addr);

        // avoid duplicate peers
        int exists = 0;
        for (int i = 0; i < count; ++i) {
            if (peers[i].addr_host_order == host_addr) {
                exists = 1;
                break;
            }
        }
        if (exists) {
            continue;
        }

        // resize peers array if capacity is reached
        if (count == capacity) {
            capacity *= 2;
            peer_info_t *tmp = realloc(peers, capacity * sizeof(peer_info_t));
            if (!tmp) {
                free(peers);
                perror("realloc");
                exit(1);
            }
            peers = tmp;
        }

        // add peer to peers array
        peers[count].addr_host_order = host_addr;
        peers[count].addr_network_order = htonl(host_addr);
        inet_ntop(AF_INET, &(sa->sin_addr), peers[count].ip, sizeof(peers[count].ip));
        peers[count].node_number = 0; // Filled in after sorting
        count++;
    }

    // no peers
    if (count == 0) {
        free(peers);
        *peer_count = 0;
        return NULL;
    }

    // sort by ip addresss so every node has the same ordering
    qsort(peers, count, sizeof(peer_info_t), compare_peer_info);
    for (int i = 0; i < count; ++i) {
        peers[i].node_number = i + 1;
    }
    *peer_count = count;
    return peers;
}

int get_node_number_for_ip(const char* ip, const peer_info_t *peers, int peer_count) {
    for (int i = 0; i < peer_count; ++i) {
        if (strcmp(peers[i].ip, ip) == 0) {
            return peers[i].node_number;
        }
    }
    return -1;
}

int get_next_node_number(int current_node, int total_nodes) {
    if (total_nodes <= 0) {
        return -1;
    }
    return (current_node % total_nodes) + 1;
}

void wait_for_turn(sync_state_t *state) {
    pthread_mutex_lock(&state->lock);
    while (!state->ready_to_send) {
        pthread_cond_wait(&state->cond, &state->lock);
    }
    state->ready_to_send = 0;
    pthread_mutex_unlock(&state->lock);
}

void* turn_timer_thread(void* arg) {
    sync_state_t *state = (sync_state_t *)arg;
    sleep(SYNC_WINDOW_SECONDS);

    pthread_mutex_lock(&state->lock);
    state->ready_to_send = 1;
    state->timer_pending = 0;
    pthread_cond_signal(&state->cond);
    pthread_mutex_unlock(&state->lock);

    return NULL;
}

void schedule_turn_timer(sync_state_t *state) {
    pthread_mutex_lock(&state->lock);
    if (state->timer_pending) {
        pthread_mutex_unlock(&state->lock);
        return;
    }
    state->timer_pending = 1;
    pthread_mutex_unlock(&state->lock);

    pthread_t timer_thread;
    if (pthread_create(&timer_thread, NULL, turn_timer_thread, state) != 0) {
        perror("Failed to create timer thread");
        pthread_mutex_lock(&state->lock);
        state->timer_pending = 0;
        pthread_mutex_unlock(&state->lock);
        return;
    }
    pthread_detach(timer_thread);
}

void broadcast_sync_packet(
    int sockfd,
    const peer_info_t *peer_map,
    int peer_count,
    const char* self_ip,
    int self_node_number) 
{
    for (int i = 0; i < peer_count; ++i) {
        if (strcmp(peer_map[i].ip, self_ip) == 0) {
            continue;
        }

        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(8080);
        dest_addr.sin_addr.s_addr = peer_map[i].addr_network_order;

        char message[64];
        snprintf(message, sizeof(message), SYNC_PREFIX "%d", self_node_number);
        sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    }

    printf("Node %d broadcasted synchronization packet.\n", self_node_number);
    fflush(stdout);
}

void sendMessageToPeers(
    int sockfd,
    const peer_info_t *peer_map,
    int peer_count,
    const char* self_ip,
    int self_node_number) 
{
    for (int i = 0; i < peer_count; ++i) {
        if (strcmp(peer_map[i].ip, self_ip) == 0) {
            continue;
        }

        struct sockaddr_in dest_addr;
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(8080);
        dest_addr.sin_addr.s_addr = peer_map[i].addr_network_order;

        char message[128];
        char *time = getCurrentTime();
        sprintf(message, MSG_PREFIX "Hello from node %d (%s): %s", self_node_number, self_ip, time);
        sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

        fflush(stdout);
        free(time);
    }
}

void perform_send_window(
    int sockfd,
    const peer_info_t *peer_map,
    int peer_count,
    const char* self_ip,
    int self_node_number) 
{
    broadcast_sync_packet(sockfd, peer_map, peer_count, self_ip, self_node_number);

    sendMessageToPeers(sockfd, peer_map, peer_count, self_ip, self_node_number);
    sleep(SYNC_WINDOW_SECONDS);
}

void receiveMessageFromPeers(
    int sockfd,
    const peer_info_t *peer_map,
    int peer_count,
    sync_state_t *sync_state) 
{
    char buffer[1024];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    printf("Listening for messages...\n");
    fflush(stdout);

    // Receive messages from each peer
    while (1) {
        char buffer[1024];
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                                      (struct sockaddr*)&sender_addr, &addr_len);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';  // Null-terminate

            // Get sender's IP
            char sender_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, sizeof(sender_ip));

            char* time = getCurrentTime();
            int node_number = get_node_number_for_ip(sender_ip, peer_map, peer_count);

            if (strncmp(buffer, SYNC_PREFIX, strlen(SYNC_PREFIX)) == 0) {
                int sender_node_number = atoi(buffer + strlen(SYNC_PREFIX));
                if (sender_node_number <= 0) {
                    sender_node_number = node_number;
                }

                printf("Received sync from node %d (%s) at %s\n",
                       sender_node_number, sender_ip, time);
                fflush(stdout);

                int next_node = get_next_node_number(sender_node_number, peer_count);
                if (next_node == sync_state->self_node_number) {
                    schedule_turn_timer(sync_state);
                }
            } else {
                if (node_number > 0) {
                    printf("Received from node %d (%s): \"%s\" at time %s\n",
                           node_number, sender_ip, buffer, time);
                } else {
                    printf("Received from %s: \"%s\" at time %s\n", sender_ip, buffer, time);
                }
                fflush(stdout);
            }
            free(time);
        }
    }
}

// Structure to pass arguments to the listener thread
typedef struct {
    int sockfd;
    int num_peers;
    const peer_info_t *peer_map;
    sync_state_t *sync_state;
} listener_args_t;

// Thread function that listens for messages
void* listener_thread(void* arg) {
    listener_args_t* args = (listener_args_t*)arg;
    receiveMessageFromPeers(args->sockfd, args->peer_map, args->num_peers, args->sync_state);
    return NULL;
}

int main() {
    // 1. Get the service name from the environment
    const char* service_name = getenv("SERVICE_NAME");
    if (service_name == NULL) {
        fprintf(stderr, "Error: SERVICE_NAME environment variable not set.\n");
        exit(1);
    }

    // 2. Get our own IP address
    char self_ip[INET_ADDRSTRLEN];
    if (get_self_ip(self_ip, sizeof(self_ip)) != 0) {
        fprintf(stderr, "Error: Could not find self IP address.\n");
        exit(1);
    }
    
    printf("--- My IP: %s ---\n", self_ip);
    printf("Waiting 5 seconds for other peers to start...\n");
    fflush(stdout); // Flush output so Docker Compose logs it immediately

    sleep(5); // Wait for other containers to start

    // 3. Set up hints for the DNS lookup
    struct addrinfo hints, *addrinfo_list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // Only want IPv4
    hints.ai_socktype = SOCK_DGRAM;  // Specify socket type (for UDP)

    // 4. Perform the DNS lookup on the service name
    int status = getaddrinfo(service_name, NULL, &hints, &addrinfo_list);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        exit(1);
    }

    // 5. Build peer map
    int peer_count = 0;
    peer_info_t *peer_map = build_peer_map(addrinfo_list, &peer_count);
    if (peer_count == 0) {
        fprintf(stderr, "Error: Could not discover any peers.\n");
        exit(1);
    }
    printf("Discovered %d node(s):\n", peer_count);
    for (int i = 0; i < peer_count; ++i) {
        printf("  Node #%d -> %s\n", peer_map[i].node_number, peer_map[i].ip);
    }
    int self_node_number = get_node_number_for_ip(self_ip, peer_map, peer_count);
    if (self_node_number < 0) {
        fprintf(stderr, "Error: Self IP %s not found in peer list.\n", self_ip);
        exit(1);
    }
    fflush(stdout);

    freeaddrinfo(addrinfo_list);

    // 6. Bind to a socket for bidirectional UDP communication
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    if (sockfd < 0) {
        fprintf(stderr, "Could not create socket.\n");
        exit(1);
    }

    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;    // listen on all interfaces
    local_addr.sin_port = htons(8080);          // Port 8080
    if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        fprintf(stderr, "Could not bind socket.\n");
        exit(1);
    }

    sync_state_t sync_state;
    pthread_mutex_init(&sync_state.lock, NULL);
    pthread_cond_init(&sync_state.cond, NULL);
    sync_state.self_node_number = self_node_number;
    sync_state.timer_pending = 0;
    sync_state.ready_to_send = (self_node_number == 1);

    // 7. Start listener thread FIRST (before sending)
    pthread_t listener;
    listener_args_t args = {sockfd, peer_count, peer_map, &sync_state};
    if (pthread_create(&listener, NULL, listener_thread, &args) != 0) {
        fprintf(stderr, "Failed to create listener thread.\n");
        exit(1);
    }

    // Small delay to ensure listener thread is ready
    printf("Waiting 10 seconds for other peers to listen...\n");
    fflush(stdout); // Flush output so Docker Compose logs it immediately
    sleep(10);

    printf("Entering synchronized send loop.\n");
    fflush(stdout);

    while (1) {
        wait_for_turn(&sync_state);
        perform_send_window(sockfd, peer_map, peer_count, self_ip, self_node_number);
    }

    return 0;
}
