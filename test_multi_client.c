// udp client driver program
#include <stdio.h>
#include <strings.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>
#include<stdlib.h>

#include<pthread.h>
#include<stdint.h>
#include <string.h>


#define PORT 5000
#define MAXLINE 1000
#define RUNS 10

struct ThreadArgs {
    int thread_num;
};

void *client(void *args)
{
    struct ThreadArgs *threadArgs = (struct ThreadArgs *)args;
    char buffer[100];
    int sockfd, n;
    struct sockaddr_in servaddr;
    char *message = malloc(strlen("Hello Server ") + snprintf(NULL, 0, "%d", threadArgs->thread_num) + 1);

    char num_str[15];
    strcpy(message, "Hello Server ");
    sprintf(num_str, "%d", threadArgs->thread_num);
    strcat(message, num_str);

    // clear servaddr
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    servaddr.sin_port = htons(PORT);
    servaddr.sin_family = AF_INET;
    
    // create datagram socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // connect to server
    if(connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
    {
        printf("\n Error : Connect Failed \n");
        exit(0);
    }
    
    // request to send datagram
    // no need to specify server address in sendto
    // connect stores the peers IP and port
    sendto(sockfd, message, MAXLINE, 0, (struct sockaddr*)NULL, sizeof(servaddr));
    
    // waiting for response
    recvfrom(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr*)NULL, NULL);
    puts(buffer);
    
    // close the descriptor
    close(sockfd);
    
    pthread_exit(NULL);
}

// Driver code
int main()
{
    pthread_t threads[RUNS];
    for (int i = 0; i < RUNS; i++) {
        struct ThreadArgs args;
        args.thread_num = i;
        // printf("thread_num: %d\n", args.thread_num);
        pthread_create(&threads[i], NULL, client, (void *)&args);
    }
    
    for (int i = 0; i < RUNS; i++) {
        pthread_join(threads[i], NULL);
    }
}