#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h> // For waitpid()
#include <errno.h>    // For errno

#define BUFFER_SIZE 4096 // データ送受信用バッファサイズ

void error_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

void transfer_data(int fd_from, int fd_to, const char* direction_name, int socket_to_shutdown_for_sender) {
    
    char buffer[BUFFER_SIZE];
    ssize_t n_read, n_written_total, n_written_single;
    int read_error_occurred = 0;
    int write_error_occurred = 0;

    fprintf(stderr, "[%s PID: %d] データ転送開始 (from fd %d to fd %d).\n",
            direction_name, getpid(), fd_from, fd_to);

    while ((n_read = read(fd_from, buffer, BUFFER_SIZE)) > 0) {
        n_written_total = 0;
        while (n_written_total < n_read) {
            n_written_single = write(fd_to, buffer + n_written_total, n_read - n_written_total);
            if (n_written_single < 0) {
                if (errno == EINTR) continue; // シグナルによる中断なら再試行
                
                if (errno == EPIPE) {
                    fprintf(stderr, "[%s PID: %d] 書込みエラー: Broken pipe (相手が接続を切断したか、パイプがクローズされました).\n",
                            direction_name, getpid());
                } else {
                    fprintf(stderr, "[%s PID: %d] 書込みエラー: ", direction_name, getpid());
                    perror("");
                }
                write_error_occurred = 1;
                goto end_loop; 
            }
            n_written_total += n_written_single;
        }
    }

end_loop:
    if (n_read < 0) {
        fprintf(stderr, "[%s PID: %d] 読込みエラー: ", direction_name, getpid());
        perror("");
        read_error_occurred = 1;
    }

    if (socket_to_shutdown_for_sender != -1) { // 送信担当の場合 (STDIN -> socket)
        if (write_error_occurred) {
             fprintf(stderr, "[%s PID: %d] 書込みエラーのため、送信を異常終了します。\n", direction_name, getpid());
        } else if (read_error_occurred) {
             fprintf(stderr, "[%s PID: %d] 標準入力の読込みエラーのため、送信を停止します。\n", direction_name, getpid());
        } else { // n_read == 0 (EOF from stdin)
             fprintf(stderr, "[%s PID: %d] 標準入力からの読み込みが正常に終了しました。送信を停止します。\n", direction_name, getpid());
        }
        // 送信パスからの送信終了を通知
        if (shutdown(socket_to_shutdown_for_sender, SHUT_WR) < 0) {
            if (errno != ENOTCONN && errno != EPIPE) { // 既に切断されている場合はエラーとしない
                 fprintf(stderr, "[%s PID: %d] shutdown(SHUT_WR) エラー: ", direction_name, getpid());
                 perror("");
            }
        }
    } else { // 受信担当の場合 (socket -> STDOUT)
        if (write_error_occurred) {
            fprintf(stderr, "[%s PID: %d] 標準出力への書込みエラーのため、受信を終了します。\n", direction_name, getpid());
        } else if (read_error_occurred) {
            fprintf(stderr, "[%s PID: %d] ソケットの読込みエラーのため、受信を終了します。\n", direction_name, getpid());
        } else { // n_read == 0 (EOF from socket)
            fprintf(stderr, "[%s PID: %d] ソケットからの読み込みが正常に終了しました (相手が送信を停止)。\n", direction_name, getpid());
        }
    }
    fprintf(stderr, "[%s PID: %d] データ転送終了。\n", direction_name, getpid());
}


int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "使用法:\n");
        fprintf(stderr, "  サーバーモード: %s server <ポート番号>\n", argv[0]);
        fprintf(stderr, "  クライアントモード: %s client <IPアドレス> <ポート番号>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port;
    int conn_fd = -1, listen_fd = -1;
    struct sockaddr_in serv_addr, client_addr;
    socklen_t client_len;
    pid_t pid;

    int is_server_mode = (strcmp(argv[1], "server") == 0);

    if (is_server_mode) { // サーバーモード
        if (argc != 3) {
            fprintf(stderr, "サーバー使用法: %s server <ポート番号>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        port = atoi(argv[2]);

        if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) error_exit("socket 作成エラー (サーバー)");
        
        int optval = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
            close(listen_fd); // setsockopt失敗時はlisten_fdを閉じる
            error_exit("setsockopt(SO_REUSEADDR) エラー");
        }

        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(port);

        if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            close(listen_fd);
            error_exit("bind エラー");
        }
        if (listen(listen_fd, 1) < 0) { // この電話アプリでは1対1通信のみ想定
            close(listen_fd);
            error_exit("listen エラー");
        }
        fprintf(stderr, "[メインプロセス PID: %d] サーバー: ポート %d で接続待機中...\n", getpid(), port);
        client_len = sizeof(client_addr);
        if ((conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len)) < 0) {
            close(listen_fd);
            error_exit("accept エラー");
        }
        char client_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_str, sizeof(client_ip_str));
        fprintf(stderr, "[メインプロセス PID: %d] サーバー: クライアント %s:%d が接続しました。\n", getpid(), client_ip_str, ntohs(client_addr.sin_port));
        close(listen_fd); // 1対1接続なのでリスニングソケットは閉じる
        listen_fd = -1; 
    } else { // クライアントモード
        if (strcmp(argv[1], "client") != 0 || argc != 4) {
            fprintf(stderr, "クライアント使用法: %s client <IPアドレス> <ポート番号>\n", argv[0]);
            exit(EXIT_FAILURE);
        }
        char *server_ip = argv[2];
        port = atoi(argv[3]);

        if ((conn_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) error_exit("socket 作成エラー (クライアント)");
        
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0) {
            close(conn_fd);
            error_exit("inet_pton 無効なIPアドレスまたは変換エラー");
        }

        fprintf(stderr, "[メインプロセス PID: %d] クライアント: サーバー %s:%d に接続中...\n", getpid(), server_ip, port);
        if (connect(conn_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            close(conn_fd);
            error_exit("connect エラー");
        }
        fprintf(stderr, "[メインプロセス PID: %d] クライアント: サーバーに接続しました。\n", getpid());
    }

    // この時点で conn_fd はサーバー・クライアント双方で確立済み
    
    pid = fork();
    if (pid < 0) {
        close(conn_fd);
        error_exit("fork エラー");
    }

    if (pid == 0) { // 子プロセス: 受信担当 (ソケット -> 標準出力)
        transfer_data(conn_fd, STDOUT_FILENO, "受信担当", -1);
        fprintf(stderr, "[受信担当 PID: %d] 終了します。\n", getpid());
        close(conn_fd); // 子プロセス側のconn_fdをクローズ
        exit(0); 
    } else { // 親プロセス: 送信担当 (標準入力 -> ソケット)
        transfer_data(STDIN_FILENO, conn_fd, "送信担当", conn_fd);
        
        fprintf(stderr, "[送信担当 PID: %d] 子プロセス (受信担当) の終了を待機中...\n", getpid());
        int status;
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid エラー");
        } else {
            if (WIFEXITED(status)) {
                fprintf(stderr, "[送信担当 PID: %d] 子プロセスはステータス %d で正常終了しました。\n", getpid(), WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "[送信担当 PID: %d] 子プロセスはシグナル %d で異常終了しました。\n", getpid(), WTERMSIG(status));
            } else {
                 fprintf(stderr, "[送信担当 PID: %d] 子プロセスの終了ステータスは不明です: %d\n", getpid(), status);
            }
        }
        
        close(conn_fd); // 親プロセス側のconn_fdをクローズ
        fprintf(stderr, "[送信担当 PID: %d] 全ての処理を終了しました。\n", getpid());
    }

    return 0;
}