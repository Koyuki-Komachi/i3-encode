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

// --- 設定項目 ---
#define FRAME_SIZE 1024             // FFTのフレームサイズ (必ず2のべき乗にすること)
#define COMPRESSION_RATIO 0.7       // 圧縮率 (0.0 - 1.0): 周波数成分の下位70%をカット

#define PI 3.14159265358979323846

// 1フレームあたりのデータサイズ (16bit = 2byte)
#define FRAME_BYTES (FRAME_SIZE * sizeof(short))
// FFT後の複素数データのサイズ
#define FFT_BYTES (FRAME_SIZE * sizeof(Complex))

// 複素数を扱うための構造体
typedef struct {
    double re; // 実部 (Real part)
    double im; // 虚部 (Imaginary part)
} Complex;


// --- FFT / IFFT 自前実装 ---

// Cooley-Tukey FFTアルゴリズム (再帰版)
void fft(Complex *x, int N) {
    if (N <= 1) return;

    // 偶数番目と奇数番目の要素に分割
    Complex *even = malloc(N/2 * sizeof(Complex));
    Complex *odd  = malloc(N/2 * sizeof(Complex));
    for (int i = 0; i < N/2; i++) {
        even[i] = x[2*i];
        odd[i]  = x[2*i+1];
    }

    // 再帰的にFFTを呼び出す
    fft(even, N/2);
    fft(odd, N/2);

    // バタフライ演算で結合
    for (int k = 0; k < N/2; k++) {
        double angle = -2.0 * PI * k / N;
        Complex t = {cos(angle) * odd[k].re - sin(angle) * odd[k].im,
                     cos(angle) * odd[k].im + sin(angle) * odd[k].re};
        x[k]       = (Complex){even[k].re + t.re, even[k].im + t.im};
        x[k + N/2] = (Complex){even[k].re - t.re, even[k].im - t.im};
    }

    free(even);
    free(odd);
}

// 逆FFT (IFFT)
void ifft(Complex *x, int N) {
    // 各要素の複素共役をとる
    for (int i = 0; i < N; i++) {
        x[i].im = -x[i].im;
    }

    // 通常のFFTを実行
    fft(x, N);

    // 結果の各要素の複素共役をとり、Nで割る
    for (int i = 0; i < N; i++) {
        x[i].re = x[i].re / N;
        x[i].im = -x[i].im / N;
    }
}

// --- 電話プログラム本体 ---

int socket_fd = -1;
int server_socket = -1;
pid_t sender_pid = -1;
pid_t receiver_pid = -1;

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
    Complex fft_buffer[FRAME_SIZE];
    
    while (read(STDIN_FILENO, pcm_buffer, FRAME_BYTES) == FRAME_BYTES) {
        // PCMデータを複素数バッファに変換
        for (int i = 0; i < FRAME_SIZE; i++) {
            fft_buffer[i].re = (double)pcm_buffer[i];
            fft_buffer[i].im = 0.0;
        }

        // 自作FFT実行
        fft(fft_buffer, FRAME_SIZE);

        // --- 圧縮処理 ---
        // 簡単な実装として、スペクトルの後半部分をカットする
        // FFTの結果は対称的なので、後半をカットしても情報を大きく失わない
        // 高周波成分をカットする場合
        int cutoff_index = (int)(FRAME_SIZE / 2 * (1.0 - COMPRESSION_RATIO));
        for (int i = cutoff_index; i < FRAME_SIZE / 2; i++) {
            fft_buffer[i].re = 0;
            fft_buffer[i].im = 0;
        // 対称性を保つため対応する負の周波数もゼロ化
            fft_buffer[FRAME_SIZE - i].re = 0;
            fft_buffer[FRAME_SIZE - i].im = 0;
}
        
        // 圧縮したデータを送信
        write(sock_fd, fft_buffer, FFT_BYTES);
    }
    exit(0);
}

// 受信プロセス: ソケット → 伸張 → IFFT → 標準出力
void audio_receiver(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    Complex fft_buffer[FRAME_SIZE];
    
    while (read(sock_fd, fft_buffer, FFT_BYTES) == FFT_BYTES) {
        // 自作IFFT実行
        ifft(fft_buffer, FRAME_SIZE);

        // 複素数データをshort型PCMデータに変換
        for (int i = 0; i < FRAME_SIZE; i++) {
            // IFFT後の虚数部は誤差で微小に残る可能性があるが、ほぼ0なので実部のみ使う
            pcm_buffer[i] = (short)round(fft_buffer[i].re);
        }

        // PCMデータを標準出力へ書き出し
        write(STDOUT_FILENO, pcm_buffer, FRAME_BYTES);
    }
    exit(0);
}

// run_server, run_client, main関数は前回のコードと同じなので省略
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
    fprintf(stderr, "Client connected from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
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
        run_server(atoi(argv[1]));
    } else if (argc == 3) {
        run_client(argv[1], atoi(argv[2]));
    } else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Server: %s <port>\n", argv[0]);
        fprintf(stderr, "  Client: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    sender_pid = fork();
    if (sender_pid == 0) audio_sender(socket_fd);

    receiver_pid = fork();
    if (receiver_pid == 0) audio_receiver(socket_fd);

    wait(NULL);
    wait(NULL);
    cleanup();

    return 0;
}