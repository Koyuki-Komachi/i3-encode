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
#define SAMPLING_RATE 44100         // サンプリング周波数 (Hz)

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


// --- FFT / IFFT 自前実装 (変更なし) ---
void fft(Complex *x, int N) {
    if (N <= 1) return;
    Complex *even = malloc(N/2 * sizeof(Complex));
    Complex *odd  = malloc(N/2 * sizeof(Complex));
    for (int i = 0; i < N/2; i++) {
        even[i] = x[2*i];
        odd[i]  = x[2*i+1];
    }
    fft(even, N/2);
    fft(odd, N/2);
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
void ifft(Complex *x, int N) {
    for (int i = 0; i < N; i++) x[i].im = -x[i].im;
    fft(x, N);
    for (int i = 0; i < N; i++) {
        x[i].re = x[i].re / N;
        x[i].im = -x[i].im / N;
    }
}
// --- ここまで変更なし ---


// --- 音響心理モデル関連 ---

// 周波数(Hz)から最小可聴値(dB)を計算する簡易式
// (MP3などで使われる複雑な式を大幅に簡略化したもの)
double calculate_ath_db(double freq_hz) {
    if (freq_hz <= 0) return 96.0; // 無音時の上限
    double f_khz = freq_hz / 1000.0;
    double ath = 3.64 * pow(f_khz, -0.8) - 6.5 * exp(-0.6 * pow(f_khz - 3.3, 2)) + 0.001 * pow(f_khz, 4);
    return ath;
}

// 指定されたビット数で値を量子化・逆量子化する
double quantize_and_dequantize(double value, int bits, double max_val) {
    if (bits == 0) return 0.0;

    long long num_levels = 1LL << (bits - 1); // 符号ビットを考慮
    double step = max_val / num_levels;

    // 量子化
    long long quantized_val = round(value / step);

    // 逆量子化
    return (double)quantized_val * step;
}


// --- 電話プログラム本体 ---

int socket_fd = -1, server_socket = -1;
pid_t sender_pid = -1, receiver_pid = -1;

void cleanup() {
    if (sender_pid > 0) kill(sender_pid, SIGTERM);
    if (receiver_pid > 0) kill(receiver_pid, SIGTERM);
    if (socket_fd >= 0) close(socket_fd);
    if (server_socket >= 0) close(server_socket);
}
void signal_handler(int sig) { cleanup(); exit(0); }

// 送信プロセス: 標準入力 → FFT → 音響心理モデルで圧縮 → ソケット
void audio_sender(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    Complex fft_buffer[FRAME_SIZE];
    
    // 量子化で参照するPCMの最大値
    double max_pcm_val = 32767.0;

    while (read(STDIN_FILENO, pcm_buffer, FRAME_BYTES) == FRAME_BYTES) {
        // PCMを複素数バッファに変換
        for (int i = 0; i < FRAME_SIZE; i++) {
            fft_buffer[i].re = (double)pcm_buffer[i];
            fft_buffer[i].im = 0.0;
        }

        // FFT実行
        fft(fft_buffer, FRAME_SIZE);

        // --- 音響心理モデルに基づく圧縮処理 ---
        // FFT結果の前半半分（N/2+1）を処理すれば十分 (後半は対称)
        for (int k = 0; k < FRAME_SIZE / 2 + 1; k++) {
            // 1. 周波数と音の大きさを計算
            double freq = (double)k * SAMPLING_RATE / FRAME_SIZE;
            double magnitude = sqrt(fft_buffer[k].re * fft_buffer[k].re + fft_buffer[k].im * fft_buffer[k].im);
            double spl_db = 20 * log10(magnitude / max_pcm_val + 1e-9); // dB SPLに変換

            // 2. その周波数の最小可聴値(ATH)を計算
            double ath_db = calculate_ath_db(freq);

            // 3. 音の大きさとATHを比較してビット数を割り当て
            int bits_to_alloc = 0;
            double margin = spl_db - ath_db; // マスキングマージン

            if (margin > 12) {
                bits_to_alloc = 8; // 聞こえる音: 高精度 (8bit)
            } else if (margin > 6) {
                bits_to_alloc = 4; // やや聞こえる音: 中精度 (4bit)
            } else if (margin > 0) {
                bits_to_alloc = 2; // かろうじて聞こえる音: 低精度 (2bit)
            } else {
                bits_to_alloc = 0; // 聞こえない音: カット
            }

            // 4. 割り当てたビット数で量子化（精度を落とす）
            // このデモでは、量子化と逆量子化を送信側で行い、精度が劣化したデータを送る
            fft_buffer[k].re = quantize_and_dequantize(fft_buffer[k].re, bits_to_alloc, max_pcm_val * FRAME_SIZE / 2);
            fft_buffer[k].im = quantize_and_dequantize(fft_buffer[k].im, bits_to_alloc, max_pcm_val * FRAME_SIZE / 2);

            // 対称な後半部分も同じように処理
            if (k > 0 && k < FRAME_SIZE / 2) {
                fft_buffer[FRAME_SIZE - k].re = fft_buffer[k].re;
                fft_buffer[FRAME_SIZE - k].im = -fft_buffer[k].im; // 虚部は共役
            }
        }
        
        // 圧縮したデータを送信
        write(sock_fd, fft_buffer, FFT_BYTES);
    }
    exit(0);
}

// 受信プロセス: ソケット → IFFT → 標準出力 (この部分は変更なし)
void audio_receiver(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    Complex fft_buffer[FRAME_SIZE];
    
    while (read(sock_fd, fft_buffer, FFT_BYTES) == FFT_BYTES) {
        // 自作IFFT実行
        ifft(fft_buffer, FRAME_SIZE);

        // 複素数データをshort型PCMデータに変換
        for (int i = 0; i < FRAME_SIZE; i++) {
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
    if (argc == 2) { run_server(atoi(argv[1])); }
    else if (argc == 3) { run_client(argv[1], atoi(argv[2])); }
    else {
        fprintf(stderr, "Usage:\n  Server: %s <port>\n  Client: %s <ip> <port>\n", argv[0],argv[0]);
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