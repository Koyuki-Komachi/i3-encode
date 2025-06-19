#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUFFER_SIZE 1024

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <Port Number>\n", argv[0]);
        return 1;
    }

    // ポート番号の妥当性チェック
    char *endptr;
    long port = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number. Must be between 1 and 65535.\n");
        return 1;
    }

    // ソケット作成
    int ss = socket(PF_INET, SOCK_STREAM, 0);
    if (ss < 0) {
        perror("socket() failed");
        return 1;
    }

    /* bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(ss, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(ss);
        return 1;
    };

    /* listen */
    if (listen(ss, 10) < 0) {
        perror("listen() failed");
        close(ss);
        return 1;
    };

    printf("Server listening on port %ld...\n", port);

    /* accept */
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(struct sockaddr_in);
    int s = accept(ss, (struct sockaddr*)&client_addr, &len);
    if (s < 0) {
        perror("accept() failed");
        close(ss);
        return 1;
    }

    /* 標準入力から読んだデータの中身を送りつける */
    ssize_t bytes_read_stdin;
    ssize_t bytes_write_socket;
    unsigned char buffer[BUFFER_SIZE];

    while ((bytes_read_stdin = read(STDIN_FILENO, buffer, BUFFER_SIZE)) > 0) {
        bytes_write_socket = write(s, buffer, bytes_read_stdin);
        if (bytes_write_socket < 0) {
            perror("write() to socket failed.");
            close(s);
            close(ss);
            return 1;
        }else if (bytes_write_socket < bytes_read_stdin) {
            fprintf(stderr, "Warning: Partial Write to Socket. Send %zd of %zd bytes.\n", bytes_write_socket, bytes_read_stdin);
            fprintf(stderr, "Treating partial write as an error. Exiting\n");
            close(s);
            close(ss);
            return 1;
        }
    }
    if (bytes_read_stdin < 0) {
        perror("read() from stdin failed.");
        close(s);
        close(ss);
        return 1;
    }
    
}