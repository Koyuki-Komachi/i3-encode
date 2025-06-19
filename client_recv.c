#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define BUFFER_SIZE 1024

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP Address> <Port Number>\n", argv[0]);
        return 1;
    }

    const char *ip_str = argv[1];
    const char *port_str = argv[2];
    int sockfd;
    struct sockaddr_in serv_addr;
    unsigned char buffer[BUFFER_SIZE];
    ssize_t n_read;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket() failed");
        return 1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;

    if (inet_aton(ip_str, &serv_addr.sin_addr) == 0) {
        fprintf(stderr, "Error: Invalid IP address format: %s\n", ip_str);
        close(sockfd);
        return 1;
    }

    long port = atoi(port_str);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number '%s'. Must be between 1 nad 65535.\n", port_str);
        close(sockfd);
        return 1;
    }
    serv_addr.sin_port = htons((unsigned short)port);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect() failed");
        close(sockfd);
        return 1;
    }

    while ((n_read = read(sockfd, buffer, BUFFER_SIZE)) > 0) {
        if (write(STDOUT_FILENO, buffer, n_read) != n_read) {
            perror("write() to stdout failed");
            close(sockfd);
            return 1;
        }
    }

    if (n_read < 0) {
        perror("read() from socket failed");
    }

    if (close(sockfd) < 0) {
        perror("close() socket failed");
        return 1;
    }
}

