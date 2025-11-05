#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

int get_self_ip(char *ip_buffer) {
    strcpy(ip_buffer, "127.0.0.1");
    return 0;
}

char* get_current_time() {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char *buffer = malloc(26);
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    return (char *) buffer;
}

int main() {
    char self_ip[16];
    get_self_ip(self_ip);

    char *message = malloc(50);
    char *time = get_current_time();
    sprintf(message, "Hello from %s: %s", (char *)self_ip, time);

    printf("%s\n", message);

    printf("%lu\n", sizeof(message));
}