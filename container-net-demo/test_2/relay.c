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
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char *buffer = malloc(26);
    if (!buffer) {
        fprintf(stderr, "Error: Could not allocate time string.\n");
        exit(1);
    }
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    return (char *) buffer;
}


int sendMessageToPeers(int sockfd, struct addrinfo *addrinfo_list, char* self_ip) {
    struct addrinfo *addrinfo_iter;
    int numPeers = 0;
    for (addrinfo_iter = addrinfo_list; addrinfo_iter != NULL; addrinfo_iter = addrinfo_iter->ai_next)
    {
        // Get the sockaddr structure
        struct sockaddr_in *sa = (struct sockaddr_in *)addrinfo_iter->ai_addr;

        // Convert the IP to a string
        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa->sin_addr), peer_ip, sizeof(peer_ip));

        // Print it *only* if it's not our own IP
        if (strcmp(self_ip, peer_ip) != 0)
        {
            struct sockaddr_in dest_addr;
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(8080);
            dest_addr.sin_addr = sa->sin_addr;

            char message[100];
            char *time = getCurrentTime();
            sprintf(message, "Hello from %s: %s\n", self_ip, time);
            sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

            printf("Sent message to %s.\n", peer_ip);
            fflush(stdout);

            free(time); // Clean up
            numPeers++;
        }
    }
    return numPeers;
}

void receiveMessageFromPeers(int sockfd, int num_peers) {
    char buffer[1024];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    printf("Listening for %d messages...\n", num_peers);
    fflush(stdout);

    // Receive messages from each peer
    for (int i = 0; i < num_peers; i++) {
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                                      (struct sockaddr*)&sender_addr, &addr_len);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';  // Null-terminate

            // Get sender's IP
            char sender_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, sizeof(sender_ip));

            printf("Received from %s: %s", sender_ip, buffer);
            fflush(stdout);
        }
    }
}

// Structure to pass arguments to the listener thread
typedef struct {
    int sockfd;
    int num_peers;
} listener_args_t;

// Thread function that listens for messages
void* listener_thread(void* arg) {
    listener_args_t* args = (listener_args_t*)arg;
    receiveMessageFromPeers(args->sockfd, args->num_peers);
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

    // 5. Bind to a socket for bidirectional UDP communication
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

    // 6. Count peers first
    int numPeers = 0;
    struct addrinfo *temp;
    for (temp = addrinfo_list; temp != NULL; temp = temp->ai_next) {
        struct sockaddr_in *sa = (struct sockaddr_in *)temp->ai_addr;
        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa->sin_addr), peer_ip, sizeof(peer_ip));
        if (strcmp(self_ip, peer_ip) != 0) {
            numPeers++;
        }
    }

    printf("Found %d peers. Starting listener thread...\n", numPeers);
    fflush(stdout);

    // 7. Start listener thread FIRST (before sending)
    pthread_t listener;
    listener_args_t args = {sockfd, numPeers};
    if (pthread_create(&listener, NULL, listener_thread, &args) != 0) {
        fprintf(stderr, "Failed to create listener thread.\n");
        exit(1);
    }

    // Small delay to ensure listener thread is ready
    sleep(1);

    // 8. Now send messages to all peers
    printf("Sending messages to peers...\n");
    fflush(stdout);
    sendMessageToPeers(sockfd, addrinfo_list, self_ip);

    // 9. Wait for listener thread to finish receiving all messages
    printf("Waiting for listener thread to complete...\n");
    fflush(stdout);
    pthread_join(listener, NULL);

    printf("---------------------------\n");
    fflush(stdout);

    // 10. Free the memory used by the list
    freeaddrinfo(addrinfo_list);

    // 11. Keep the container running so we can see it in "docker ps"
    // and so other containers can discover it.
    printf("Idling...\n");
    fflush(stdout);
    sleep(60); // Sleep for 1 minute (adjust as needed)

    return 0;
}