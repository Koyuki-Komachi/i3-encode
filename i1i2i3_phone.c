// インターネット電話プログラム（パイプライン対応版）
// サーバー: rec ... | ./i1i2i3_phone 50000 | play ...
// クライアント: rec ... | ./i1i2i3_phone <ip> 50000 | play ...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#define BUFFER_SIZE 1024

int socket_fd = -1;
int server_socket = -1;
pid_t sender_pid = -1;
pid_t receiver_pid = -1;

// クリーンアップ
void cleanup() {
    if (sender_pid > 0) kill(sender_pid, SIGTERM);
    if (receiver_pid > 0) kill(receiver_pid, SIGTERM);
    if (socket_fd >= 0) close(socket_fd);
    if (server_socket >= 0) close(server_socket);
}

void signal_handler(int sig) {
    cleanup();
    exit(0);
}

// 送信プロセス: 標準入力 → ソケット
void audio_sender(int sock_fd) {
    unsigned char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE)) > 0) {
        write(sock_fd, buffer, bytes_read);
    }
    exit(0);
}

// 受信プロセス: ソケット → 標準出力
void audio_receiver(int sock_fd) {
    unsigned char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = read(sock_fd, buffer, BUFFER_SIZE)) > 0) {
        write(STDOUT_FILENO, buffer, bytes_read);
    }
    exit(0);
}

int run_server(int port) {
    struct sockaddr_in addr;

    server_socket = socket(PF_INET, SOCK_STREAM, 0);

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_socket, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_socket, 1);

    fprintf(stderr, "Server listening on port %d...\n", port);

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    socket_fd = accept(server_socket, (struct sockaddr*)&client_addr, &len);

    fprintf(stderr, "Client connected from %s:%d\n",
            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    return 0;
}

int run_client(const char *ip_str, int port) {
    struct sockaddr_in serv_addr;

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_aton(ip_str, &serv_addr.sin_addr);

    fprintf(stderr, "Connecting to %s:%d...\n", ip_str, port);
    connect(socket_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    fprintf(stderr, "Connected!\n");

    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (argc == 2) {
        int port = atoi(argv[1]);
        run_server(port);
    }else if (argc == 3) {
        const char *ip_str = argv[1];
        int port = atoi(argv[2]);
        run_client(ip_str, port);
    }else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Server: %s <port>\n", argv[0]);
        fprintf(stderr, "  Client: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    sender_pid = fork();
    if (sender_pid == 0) {
        audio_sender(socket_fd);
    }

    receiver_pid = fork();
    if (receiver_pid == 0) {
        audio_receiver(socket_fd);
    }

    wait(NULL);
    cleanup();

    return 0;
}