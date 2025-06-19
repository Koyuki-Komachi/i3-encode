// インターネット電話プログラム（圧縮機能付き）
//
// [非圧縮モード]
// サーバー: rec ... | ./phone 50000 | play ...
// クライアント: rec ... | ./phone <ip> 50000 | play ...
//
// [圧縮モード (例: 1/2に間引く)]
// サーバー: rec ... | ./phone 50000 2 | play ...
// クライアント: rec ... | ./phone <ip> 50000 2 | play ...

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h> // int16_t を使うために追加

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

// 送信プロセス: 標準入力 → (圧縮) → ソケット
// rate > 1 の場合に、音声データを間引いて圧縮する
void audio_sender(int sock_fd, int rate) {
    // 圧縮しない場合 (rateが1以下) は、バッファ単位で高速に転送
    if (rate <= 1) {
        unsigned char buffer[BUFFER_SIZE];
        ssize_t bytes_read;
        while ((bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE)) > 0) {
            if (write(sock_fd, buffer, bytes_read) < 0) {
                perror("write");
                break;
            }
        }
    } else {
        // 圧縮する場合 (rate > 1) は、サンプル単位で処理
        int16_t sample;
        int count = 0;
        // freadで標準入力から1サンプル(16bit = 2bytes)ずつ読み込む
        while (fread(&sample, sizeof(int16_t), 1, stdin) == 1) {
            // countがrateの倍数の時だけサンプルを送信
            if (count % rate == 0) {
                if (write(sock_fd, &sample, sizeof(int16_t)) < 0) {
                    perror("write");
                    break;
                }
            }
            count++;
        }
    }
    exit(0);
}

// 受信プロセス: ソケット → 標準出力
void audio_receiver(int sock_fd) {
    unsigned char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = read(sock_fd, buffer, BUFFER_SIZE)) > 0) {
        if (write(STDOUT_FILENO, buffer, bytes_read) < 0) {
            perror("write to stdout");
            break;
        }
    }
    exit(0);
}

// サーバーモード
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

// クライアントモード
int run_client(const char *ip_str, int port) {
    struct sockaddr_in serv_addr;
    
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_aton(ip_str, &serv_addr.sin_addr);
    
    fprintf(stderr, "Connecting to %s:%d...\n", ip_str, port);
    connect(socket_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    fprintf(stderr, "Connected!\n");
    
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int port;
    const char *ip_str;
    int rate = 1; // デフォルトの圧縮率は1 (圧縮なし)

    if (argc == 2) { // サーバーモード (圧縮なし)
        port = atoi(argv[1]);
        run_server(port);
    } else if (argc == 3) {
        // サーバーモード (圧縮あり) or クライアント (圧縮なし)
        if (strchr(argv[1], '.')) { // 第1引数がIPアドレスならクライアント
            ip_str = argv[1];
            port = atoi(argv[2]);
            run_client(ip_str, port);
        } else { // それ以外はサーバー
            port = atoi(argv[1]);
            rate = atoi(argv[2]);
            run_server(port);
        }
    } else if (argc == 4) { // クライアントモード (圧縮あり)
        ip_str = argv[1];
        port = atoi(argv[2]);
        rate = atoi(argv[3]);
        run_client(ip_str, port);
    } else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Server: %s <port> [rate]\n", argv[0]);
        fprintf(stderr, "  Client: %s <ip> <port> [rate]\n", argv[0]);
        return 1;
    }

    if (rate <= 0) {
        fprintf(stderr, "Rate must be a positive integer.\n");
        rate = 1;
    }
    if (rate > 1) {
        fprintf(stderr, "Compression mode enabled: 1/%d sampling.\n", rate);
    }
    
    // 送信プロセス: 標準入力 → ソケット
    sender_pid = fork();
    if (sender_pid == 0) {
        // 圧縮率を渡す
        audio_sender(socket_fd, rate);
    }
    
    // 受信プロセス: ソケット → 標準出力
    receiver_pid = fork();
    if (receiver_pid == 0) {
        audio_receiver(socket_fd);
    }
    
    // 親プロセス: 子プロセスの終了を待機
    wait(NULL);
    cleanup();
    
    return 0;
}
