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
#define SAMPLE_RATE 16000           // サンプリングレート (Hz)
#define NUM_BANDS 32                // 周波数帯域の分割数

// 電話帯域の設定
#define PHONE_BAND_LOW_HZ 300      // 電話帯域の下限 (Hz)
#define PHONE_BAND_HIGH_HZ 3400    // 電話帯域の上限 (Hz)

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

// 心理音響圧縮用の構造体
typedef struct {
    float magnitude;
    float phase;
    unsigned char quantized_mag;  // 量子化された振幅
    unsigned char quantized_phase; // 量子化された位相
} PsychoData;

// 各周波数帯域の圧縮設定
typedef struct {
    int start_bin;      // 開始周波数ビン
    int end_bin;        // 終了周波数ビン
    int mag_bits;       // 振幅の量子化ビット数
    int phase_bits;     // 位相の量子化ビット数
    float threshold_db; // 最小可聴値 (dB)
} BandConfig;

// 圧縮方法の選択
typedef enum {
    COMPRESS_PSYCHOACOUSTIC = 1,  // 心理音響圧縮
    COMPRESS_PHONE_BAND = 2       // 電話帯域制限
} CompressionMethod;

// グローバル変数
CompressionMethod g_compression_method = COMPRESS_PSYCHOACOUSTIC;
int g_phone_band_low_bin, g_phone_band_high_bin;  // 電話帯域のビン番号

// --- 電話帯域制限機能 ---

// 電話帯域のビン番号を計算
void init_phone_band_bins() {
    g_phone_band_low_bin = (int)((float)PHONE_BAND_LOW_HZ * FRAME_SIZE / SAMPLE_RATE);
    g_phone_band_high_bin = (int)((float)PHONE_BAND_HIGH_HZ * FRAME_SIZE / SAMPLE_RATE);
    
    // 範囲チェック
    if (g_phone_band_low_bin < 0) g_phone_band_low_bin = 0;
    if (g_phone_band_high_bin >= FRAME_SIZE/2) g_phone_band_high_bin = FRAME_SIZE/2 - 1;
    
    fprintf(stderr, "Phone band filtering: %d Hz - %d Hz (bins %d - %d)\n",
            PHONE_BAND_LOW_HZ, PHONE_BAND_HIGH_HZ, 
            g_phone_band_low_bin, g_phone_band_high_bin);
}

// 電話帯域制限を適用
void apply_phone_band_filter(Complex *fft_data) {
    // 低周波成分を0にする
    for (int i = 0; i < g_phone_band_low_bin; i++) {
        fft_data[i].re = 0.0;
        fft_data[i].im = 0.0;
        // 対称成分も0にする
        fft_data[FRAME_SIZE - 1 - i].re = 0.0;
        fft_data[FRAME_SIZE - 1 - i].im = 0.0;
    }
    
    // 高周波成分を0にする
    for (int i = g_phone_band_high_bin + 1; i < FRAME_SIZE/2; i++) {
        fft_data[i].re = 0.0;
        fft_data[i].im = 0.0;
        // 対称成分も0にする
        fft_data[FRAME_SIZE - i].re = 0.0;
        fft_data[FRAME_SIZE - i].im = 0.0;
    }
}

// 電話帯域データの圧縮（有効な帯域のみ送信）
void phone_band_compress(Complex *fft_data, unsigned char *compressed_data, int *compressed_size) {
    int write_pos = 0;
    
    // 有効な帯域のみを圧縮データに格納
    for (int i = g_phone_band_low_bin; i <= g_phone_band_high_bin; i++) {
        // 実部と虚部を float として格納
        float real_part = (float)fft_data[i].re;
        float imag_part = (float)fft_data[i].im;
        
        memcpy(&compressed_data[write_pos], &real_part, sizeof(float));
        write_pos += sizeof(float);
        memcpy(&compressed_data[write_pos], &imag_part, sizeof(float));
        write_pos += sizeof(float);
    }
    
    *compressed_size = write_pos;
}

// 電話帯域データの展開
void phone_band_decompress(unsigned char *compressed_data, Complex *fft_data, int compressed_size) {
    // FFTバッファを初期化
    memset(fft_data, 0, FRAME_SIZE * sizeof(Complex));
    
    int read_pos = 0;
    
    // 有効な帯域のデータを復元
    for (int i = g_phone_band_low_bin; i <= g_phone_band_high_bin && read_pos < compressed_size; i++) {
        if (read_pos + sizeof(float) * 2 > compressed_size) break;
        
        float real_part, imag_part;
        memcpy(&real_part, &compressed_data[read_pos], sizeof(float));
        read_pos += sizeof(float);
        memcpy(&imag_part, &compressed_data[read_pos], sizeof(float));
        read_pos += sizeof(float);
        
        fft_data[i].re = (double)real_part;
        fft_data[i].im = (double)imag_part;
        
        // 対称性を保つ（実数信号のため）
        if (i > 0 && i < FRAME_SIZE/2) {
            fft_data[FRAME_SIZE - i].re = fft_data[i].re;
            fft_data[FRAME_SIZE - i].im = -fft_data[i].im;
        }
    }
}

// --- 心理音響モデル ---

// 絶対聴覚閾値の近似式 (Bark scale based)
float absolute_threshold_db(float freq_hz) {
    if (freq_hz < 20) return 80.0f;
    if (freq_hz > 16000) return 60.0f;
    
    // 簡略化された絶対聴覚閾値曲線
    float bark = 13.0f * atan(0.00076f * freq_hz) + 3.5f * atan(pow(freq_hz / 7500.0f, 2));
    float threshold = 3.64f * pow(freq_hz / 1000.0f, -0.8f) 
                     - 6.5f * exp(-0.6f * pow(freq_hz / 1000.0f - 3.3f, 2)) 
                     + 0.001f * pow(freq_hz / 1000.0f, 4);
    
    // 低周波と高周波でのペナルティ
    if (freq_hz < 500) threshold += 20.0f;
    if (freq_hz > 8000) threshold += 10.0f;
    
    return threshold;
}

// 周波数帯域の設定を初期化
void init_band_config(BandConfig bands[NUM_BANDS]) {
    int bins_per_band = (FRAME_SIZE / 2) / NUM_BANDS;
    
    for (int i = 0; i < NUM_BANDS; i++) {
        bands[i].start_bin = i * bins_per_band;
        bands[i].end_bin = (i + 1) * bins_per_band - 1;
        if (i == NUM_BANDS - 1) bands[i].end_bin = FRAME_SIZE / 2 - 1;
        
        // 中心周波数を計算
        float center_freq = ((float)(bands[i].start_bin + bands[i].end_bin) / 2.0f) 
                           * SAMPLE_RATE / FRAME_SIZE;
        
        // 絶対聴覚閾値を取得
        bands[i].threshold_db = absolute_threshold_db(center_freq);
        
        // 閾値に基づいてビット数を決定
        if (bands[i].threshold_db > 40.0f) {
            // 聞こえにくい帯域: 低ビット
            bands[i].mag_bits = 3;
            bands[i].phase_bits = 2;
        } else if (bands[i].threshold_db > 20.0f) {
            // 中程度の帯域: 中ビット
            bands[i].mag_bits = 5;
            bands[i].phase_bits = 3;
        } else {
            // 聞こえやすい帯域: 高ビット
            bands[i].mag_bits = 7;
            bands[i].phase_bits = 4;
        }
        
        fprintf(stderr, "Band %d: %.1f-%.1f Hz, Threshold: %.1f dB, Bits: %d/%d\n",
                i, 
                bands[i].start_bin * (float)SAMPLE_RATE / FRAME_SIZE,
                bands[i].end_bin * (float)SAMPLE_RATE / FRAME_SIZE,
                bands[i].threshold_db,
                bands[i].mag_bits, bands[i].phase_bits);
    }
}

// 量子化関数
unsigned char quantize_value(float value, int bits, float min_val, float max_val) {
    int levels = (1 << bits) - 1;  // 2^bits - 1
    float normalized = (value - min_val) / (max_val - min_val);
    normalized = fmaxf(0.0f, fminf(1.0f, normalized));  // クランプ
    return (unsigned char)(normalized * levels);
}

// 逆量子化関数
float dequantize_value(unsigned char quantized, int bits, float min_val, float max_val) {
    int levels = (1 << bits) - 1;  // 2^bits - 1
    float normalized = (float)quantized / levels;
    return min_val + normalized * (max_val - min_val);
}

// 心理音響圧縮
void psychoacoustic_compress(Complex *fft_data, unsigned char *compressed_data, 
                           BandConfig bands[NUM_BANDS], int *compressed_size) {
    int write_pos = 0;
    
    for (int band = 0; band < NUM_BANDS; band++) {
        for (int bin = bands[band].start_bin; bin <= bands[band].end_bin && bin < FRAME_SIZE/2; bin++) {
            // 振幅と位相を計算
            float magnitude = sqrt(fft_data[bin].re * fft_data[bin].re + 
                                 fft_data[bin].im * fft_data[bin].im);
            float phase = atan2(fft_data[bin].im, fft_data[bin].re);
            
            // 振幅を dB に変換
            float magnitude_db = 20.0f * log10f(fmaxf(magnitude, 1e-10f));
            
            // 閾値以下の成分は大幅に減衰
            if (magnitude_db < bands[band].threshold_db) {
                magnitude_db = bands[band].threshold_db - 20.0f;  // さらに20dB減衰
            }
            
            // 量子化範囲を設定 (適応的)
            float mag_min = bands[band].threshold_db - 30.0f;
            float mag_max = mag_min + 60.0f;  // 60dBの範囲
            
            // 量子化
            unsigned char q_mag = quantize_value(magnitude_db, bands[band].mag_bits, mag_min, mag_max);
            unsigned char q_phase = quantize_value(phase + PI, bands[band].phase_bits, 0.0f, 2.0f * PI);
            
            // 圧縮データに書き込み
            compressed_data[write_pos++] = q_mag;
            compressed_data[write_pos++] = q_phase;
        }
    }
    
    *compressed_size = write_pos;
}

// 心理音響展開
void psychoacoustic_decompress(unsigned char *compressed_data, Complex *fft_data, 
                             BandConfig bands[NUM_BANDS], int compressed_size) {
    // FFTバッファを初期化
    memset(fft_data, 0, FRAME_SIZE * sizeof(Complex));
    
    int read_pos = 0;
    
    for (int band = 0; band < NUM_BANDS && read_pos < compressed_size; band++) {
        for (int bin = bands[band].start_bin; bin <= bands[band].end_bin && bin < FRAME_SIZE/2; bin++) {
            if (read_pos + 1 >= compressed_size) break;
            
            // 圧縮データから読み取り
            unsigned char q_mag = compressed_data[read_pos++];
            unsigned char q_phase = compressed_data[read_pos++];
            
            // 逆量子化
            float mag_min = bands[band].threshold_db - 30.0f;
            float mag_max = mag_min + 60.0f;
            
            float magnitude_db = dequantize_value(q_mag, bands[band].mag_bits, mag_min, mag_max);
            float phase = dequantize_value(q_phase, bands[band].phase_bits, 0.0f, 2.0f * PI) - PI;
            
            // dBから線形振幅に変換
            float magnitude = pow(10.0f, magnitude_db / 20.0f);
            
            // 複素数に変換
            fft_data[bin].re = magnitude * cos(phase);
            fft_data[bin].im = magnitude * sin(phase);
            
            // 対称性を保つ（実数信号のため）
            if (bin > 0 && bin < FRAME_SIZE/2) {
                fft_data[FRAME_SIZE - bin].re = fft_data[bin].re;
                fft_data[FRAME_SIZE - bin].im = -fft_data[bin].im;
            }
        }
    }
}

// --- FFT / IFFT 実装 ---

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
    for (int i = 0; i < N; i++) {
        x[i].im = -x[i].im;
    }

    fft(x, N);

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
BandConfig g_bands[NUM_BANDS];  // グローバル帯域設定

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

// 送信プロセス
void audio_sender(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    Complex fft_buffer[FRAME_SIZE];
    unsigned char compressed_data[FRAME_SIZE * 2];  // 最大サイズ
    
    while (read(STDIN_FILENO, pcm_buffer, FRAME_BYTES) == FRAME_BYTES) {
        // PCMデータを複素数バッファに変換
        for (int i = 0; i < FRAME_SIZE; i++) {
            fft_buffer[i].re = (double)pcm_buffer[i];
            fft_buffer[i].im = 0.0;
        }

        // FFT実行
        fft(fft_buffer, FRAME_SIZE);

        int compressed_size;
        
        // 圧縮方法に応じて処理
        if (g_compression_method == COMPRESS_PHONE_BAND) {
            // 電話帯域制限を適用
            apply_phone_band_filter(fft_buffer);
            // 電話帯域圧縮
            phone_band_compress(fft_buffer, compressed_data, &compressed_size);
        } else {
            // 心理音響圧縮
            psychoacoustic_compress(fft_buffer, compressed_data, g_bands, &compressed_size);
        }
        
        // 圧縮サイズを先に送信
        write(sock_fd, &compressed_size, sizeof(int));
        // 圧縮データを送信
        write(sock_fd, compressed_data, compressed_size);
        
        // 圧縮率を表示
        static int frame_count = 0;
        if (++frame_count % 100 == 0) {
            int original_size = (g_compression_method == COMPRESS_PHONE_BAND) ? 
                               ((g_phone_band_high_bin - g_phone_band_low_bin + 1) * 2 * sizeof(double)) : 
                               FFT_BYTES;
            float compression_ratio = (float)compressed_size / original_size;
            const char* method_name = (g_compression_method == COMPRESS_PHONE_BAND) ? 
                                    "Phone Band" : "Psychoacoustic";
            fprintf(stderr, "%s compression ratio: %.2f%% (Frame %d)\n", 
                   method_name, compression_ratio * 100, frame_count);
        }
    }
    exit(0);
}

// 受信プロセス
void audio_receiver(int sock_fd) {
    short pcm_buffer[FRAME_SIZE];
    Complex fft_buffer[FRAME_SIZE];
    unsigned char compressed_data[FRAME_SIZE * 2];
    
    while (1) {
        int compressed_size;
        // 圧縮サイズを受信
        if (read(sock_fd, &compressed_size, sizeof(int)) != sizeof(int)) break;
        if (compressed_size <= 0 || compressed_size > FRAME_SIZE * 2) break;
        
        // 圧縮データを受信
        if (read(sock_fd, compressed_data, compressed_size) != compressed_size) break;
        
        // 圧縮方法に応じて展開
        if (g_compression_method == COMPRESS_PHONE_BAND) {
            // 電話帯域展開
            phone_band_decompress(compressed_data, fft_buffer, compressed_size);
        } else {
            // 心理音響展開
            psychoacoustic_decompress(compressed_data, fft_buffer, g_bands, compressed_size);
        }

        // IFFT実行
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

    // コマンドライン引数の解析
    int compression_method = 1;  // デフォルトは心理音響圧縮
    int arg_start = 1;
    
    if (argc >= 2 && (strcmp(argv[1], "-p") == 0 || strcmp(argv[1], "--psychoacoustic") == 0)) {
        compression_method = 1;
        arg_start = 2;
    } else if (argc >= 2 && (strcmp(argv[1], "-b") == 0 || strcmp(argv[1], "--phone-band") == 0)) {
        compression_method = 2;
        arg_start = 2;
    }
    
    g_compression_method = (CompressionMethod)compression_method;
    
    if (g_compression_method == COMPRESS_PSYCHOACOUSTIC) {
        fprintf(stderr, "Using psychoacoustic compression\n");
        init_band_config(g_bands);
    } else {
        fprintf(stderr, "Using phone band compression (300-3400 Hz)\n");
        init_phone_band_bins();
    }

    // ネットワーク設定
    if (argc - arg_start == 1) {
        run_server(atoi(argv[arg_start]));
    } else if (argc - arg_start == 2) {
        run_client(argv[arg_start], atoi(argv[arg_start + 1]));
    } else {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  Options:\n");
        fprintf(stderr, "    -p, --psychoacoustic  Use psychoacoustic compression (default)\n");
        fprintf(stderr, "    -b, --phone-band      Use phone band compression (300-3400 Hz)\n");
        fprintf(stderr, "  Server: %s [options] <port>\n", argv[0]);
        fprintf(stderr, "  Client: %s [options] <ip> <port>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s -p 12345                    # Psychoacoustic compression server\n", argv[0]);
        fprintf(stderr, "  %s -b 127.0.0.1 12345         # Phone band compression client\n", argv[0]);
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