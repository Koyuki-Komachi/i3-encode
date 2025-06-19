// rec -t raw -b 16 -c 1 -e s -r 44100 - | ./serv_send2 50000
// ./client_recv <IP Address> 50000 | play -t raw -b 16 -c 1 -e s -r 44100

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

#define BUFFER_SIZE 1024

// グローバル変数でクリーンアップ用のファイルポインタを保持
FILE *rec_pipe = NULL;
int client_socket = -1;
int server_socket = -1;

// シグナルハンドラ（Ctrl+Cなどでの終了時のクリーンアップ）
void cleanup_handler(int sig) {
    printf("\nReceived signal %d. Cleaning up...\n", sig);
    
    if (rec_pipe != NULL) {
        pclose(rec_pipe);
        rec_pipe = NULL;
    }
    
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
    
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    
    exit(0);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <Port Number>\n", argv[0]);
        return 1;
    }

    // シグナルハンドラの設定
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);

    // ポート番号の妥当性チェック
    char *endptr;
    long port = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "Error: Invalid port number. Must be 1-65535.\n");
        return 1;
    }

    // ソケット作成
    server_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket() failed");
        return 1;
    }

    // SO_REUSEADDRオプションを設定
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt() failed");
        close(server_socket);
        return 1;
    }

    // bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(server_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(server_socket);
        return 1;
    }

    // listen
    if (listen(server_socket, 10) < 0) {
        perror("listen() failed");
        close(server_socket);
        return 1;
    }

    printf("Server listening on port %ld...\n", port);

    // accept
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(client_addr));
    socklen_t len = sizeof(struct sockaddr_in);
    client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &len);
    if (client_socket < 0) {
        perror("accept() failed");
        close(server_socket);
        return 1;
    }

    printf("Client connected from %s:%d\n", 
           inet_ntoa(client_addr.sin_addr), 
           ntohs(client_addr.sin_port));

    // クライアント接続後にrecコマンドを起動
    printf("Starting audio recording...\n");
    
    // recコマンドで音声録音を開始（WAV形式で標準出力に出力）
    // -t wav: WAV形式で出力
    // -c 1: モノラル
    // -r 44100: サンプリングレート44.1kHz
    // -b 16: 16bit
    // -: 標準出力に出力
    rec_pipe = popen("rec -t raw -b 16 -c 1 -e s -r 44100 -", "r");
    if (rec_pipe == NULL) {
        perror("popen() failed - rec command not found or failed to start");
        fprintf(stderr, "Make sure 'rec' command (from sox package) is installed.\n");
        fprintf(stderr, "On Ubuntu/Debian: sudo apt-get install sox\n");
        fprintf(stderr, "On CentOS/RHEL: sudo yum install sox\n");
        close(client_socket);
        close(server_socket);
        return 1;
    }

    // 録音データをクライアントに送信
    unsigned char buffer[BUFFER_SIZE];
    size_t bytes_read;
    ssize_t bytes_write_socket;

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, rec_pipe)) > 0) {
        ssize_t total_written = 0;
        
        // 部分書き込みに対応
        while (total_written < (ssize_t)bytes_read) {
            bytes_write_socket = write(client_socket, buffer + total_written, 
                                     bytes_read - total_written);
            if (bytes_write_socket < 0) {
                if (errno == EPIPE) {
                    printf("Client disconnected (broken pipe).\n");
                } else {
                    perror("write() to socket failed");
                }
                goto cleanup;
            }
            total_written += bytes_write_socket;
        }
    }

    // fread()の終了理由をチェック
    if (ferror(rec_pipe)) {
        perror("fread() from rec pipe failed");
    } else {
        printf("Audio recording ended (EOF from rec command).\n");
    }

cleanup:
    // クリーンアップ
    if (rec_pipe != NULL) {
        int pclose_status = pclose(rec_pipe);
        if (pclose_status == -1) {
            perror("pclose() failed");
        } else {
            printf("rec command terminated with status: %d\n", pclose_status);
        }
        rec_pipe = NULL;
    }
    
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
    
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }

    printf("Server terminated.\n");
    return 0;
}