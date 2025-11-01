#include <stdio.h>      // For printf, perror
#include <stdlib.h>     // For getenv, exit
#include <string.h>     // For strcmp, memset
#include <ifaddrs.h>    // For getifaddrs, freeifaddrs
#include <netdb.h>      // For getaddrinfo, freeaddrinfo, struct addrinfo
#include <unistd.h>     // For sleep
#include <arpa/inet.h>  // For inet_ntop, struct sockaddr_in
#include <sys/socket.h> // For AF_INET

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
    struct addrinfo hints, *addrinfo_list, *addrinfo_iter;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // Only want IPv4
    hints.ai_socktype = SOCK_DGRAM;  // Specify socket type (for UDP)

    // 4. Perform the DNS lookup on the service name
    int status = getaddrinfo(service_name, NULL, &hints, &addrinfo_list);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        exit(1);
    }

    // 5. Loop through the linked list of results and print neighbors
    printf("Neighbors:\n");
    for (addrinfo_iter = addrinfo_list; addrinfo_iter != NULL; addrinfo_iter = addrinfo_iter->ai_next) {
        // Get the sockaddr structure
        struct sockaddr_in *sa = (struct sockaddr_in *)addrinfo_iter->ai_addr;
        
        // Convert the IP to a string
        char peer_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa->sin_addr), peer_ip, sizeof(peer_ip));

        // Print it *only* if it's not our own IP
        if (strcmp(self_ip, peer_ip) != 0) {
            printf("- %s\n", peer_ip);
        }
    }
    printf("---------------------------\n");
    fflush(stdout);

    // 6. Free the memory used by the list
    freeaddrinfo(addrinfo_list);

    // 7. Keep the container running so we can see it in "docker ps"
    // and so other containers can discover it.
    printf("Discovery complete. Idling...\n");
    fflush(stdout);
    sleep(60); // Sleep for 1 minute (adjust as needed)

    return 0;
}