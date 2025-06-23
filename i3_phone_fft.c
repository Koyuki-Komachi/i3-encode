#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <math.h>
#include <fftw3.h>

// --- 設定項目 ---
#define FRAME_SIZE 1024             // FFTを行う音声データのフレームサイズ
#define COMPRESSION_RATIO 0.7       // 圧縮率 (0.0 - 1.0): 振幅が小さい下位70%をカット

// 1フレームあたりのデータサイズ (16bit = 2byte)
#define FRAME_BYTES (FRAME_SIZE * sizeof(short))
// FFT後の複素数データのサイズ
#define FFT_COMPLEX_SIZE (FRAME_SIZE / 2 + 1)
#define FFT_BYTES (FFT_COMPLEX_SIZE * sizeof(fftw_complex))
// ---

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

// 送信プロセス: 標準入力 → FFT → 圧縮 → ソケット
void audio_sender(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    double *fft_in;
    fftw_complex *fft_out;
    fftw_plan plan_forward;

    // FFTWのためのメモリ確保
    fft_in = (double*) fftw_malloc(sizeof(double) * FRAME_SIZE);
    fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_COMPLEX_SIZE);

    // FFTプランの作成
    plan_forward = fftw_plan_dft_r2c_1d(FRAME_SIZE, fft_in, fft_out, FFTW_ESTIMATE);

    while (read(STDIN_FILENO, pcm_buffer, FRAME_BYTES) == FRAME_BYTES) {
        // short型PCMデータをdouble型に変換
        for (int i = 0; i < FRAME_SIZE; i++) {
            fft_in[i] = (double)pcm_buffer[i];
        }

        // FFT実行
        fftw_execute(plan_forward);

        // --- 圧縮処理 ---
        // 振幅が小さい高周波成分をカットする
        // 簡単な実装として、周波数成分の上位(1-COMPRESSION_RATIO)%をゼロにする
        int compression_index = (int)(FFT_COMPLEX_SIZE * (1.0 - COMPRESSION_RATIO));
        for (int i = compression_index; i < FFT_COMPLEX_SIZE; i++) {
            fft_out[i][0] = 0; // 実部
            fft_out[i][1] = 0; // 虚部
        }
        
        // 圧縮したデータを送信
        write(sock_fd, fft_out, FFT_BYTES);
    }

    // 後片付け
    fftw_destroy_plan(plan_forward);
    fftw_free(fft_in);
    fftw_free(fft_out);
    exit(0);
}

// 受信プロセス: ソケット → 伸張 → IFFT → 標準出力
void audio_receiver(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    fftw_complex *fft_in;
    double *fft_out;
    fftw_plan plan_backward;

    // FFTWのためのメモリ確保
    fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_COMPLEX_SIZE);
    fft_out = (double*) fftw_malloc(sizeof(double) * FRAME_SIZE);

    // 逆FFTプランの作成
    plan_backward = fftw_plan_dft_c2r_1d(FRAME_SIZE, fft_in, fft_out, FFTW_ESTIMATE);

    while (read(sock_fd, fft_in, FFT_BYTES) == FFT_BYTES) {
        // 逆FFT実行
        fftw_execute(plan_backward);

        // double型をshort型PCMデータに変換
        // IFFTの結果はスケールされるため、FRAME_SIZEで割って正規化する
        for (int i = 0; i < FRAME_SIZE; i++) {
            pcm_buffer[i] = (short)(fft_out[i] / FRAME_SIZE);
        }

        // PCMデータを標準出力へ書き出し
        write(STDOUT_FILENO, pcm_buffer, FRAME_BYTES);
    }

    // 後片付け
    fftw_destroy_plan(plan_backward);
    fftw_free(fft_in);
    fftw_free(fft_out);
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
    } else if (argc == 3) {
        const char *ip_str = argv[1];
        int port = atoi(argv[2]);
        run_client(ip_str, port);
    } else {
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
    wait(NULL); // 2つの子プロセスを待つ
    cleanup();

    return 0;
}