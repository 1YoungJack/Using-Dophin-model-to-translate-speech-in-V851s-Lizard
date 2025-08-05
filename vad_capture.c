#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <libwebsockets.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "cJSON.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <stdatomic.h>
#include <sys/queue.h>

#define SAMPLE_RATE 16000
#define CHANNELS 1
#define AUDIO_FORMAT SND_PCM_FORMAT_S16_LE
#define WINDOW_SIZE 512

static volatile int interrupted = 0;
static volatile int audio_thread_running = 0;
static struct lws* websocket_client = NULL;
static snd_pcm_t* capture_handle = NULL;
static pthread_t audio_thread;
static pthread_mutex_t ws_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ws_cond = PTHREAD_COND_INITIALIZER;

static const SherpaOnnxVoiceActivityDetector* vad = NULL;
static SherpaOnnxVadModelConfig vadConfig;

struct per_session_data {
    int established;
};

void sig_handler(int sig) {
    printf("\n收到信号 %d，准备退出...\n", sig);
    interrupted = 1;
    if (capture_handle) {
        snd_pcm_drop(capture_handle);
    }
}

int init_audio() {
    int err;
    snd_pcm_hw_params_t* hw_params;
    const char* devices[] = { "default", "hw:0,0", "plughw:0,0", "hw:audiocodec" };
    int device_count = sizeof(devices) / sizeof(devices[0]);
    for (int i = 0; i < device_count; i++) {
        printf("尝试打开音频设备: %s\n", devices[i]);
        err = snd_pcm_open(&capture_handle, devices[i], SND_PCM_STREAM_CAPTURE, 0);
        if (err >= 0) {
            printf("成功打开音频设备: %s\n", devices[i]);
            break;
        }
        printf("无法打开音频设备 %s: %s\n", devices[i], snd_strerror(err));
    }
    if (err < 0) {
        fprintf(stderr, "所有音频设备都无法打开\n");
        return -1;
    }
    snd_pcm_hw_params_alloca(&hw_params);
    if ((err = snd_pcm_hw_params_any(capture_handle, hw_params)) < 0) {
        fprintf(stderr, "无法初始化硬件参数: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_access(capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "无法设置访问类型: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_format(capture_handle, hw_params, AUDIO_FORMAT)) < 0) {
        fprintf(stderr, "无法设置音频格式: %s\n", snd_strerror(err));
        return -1;
    }
    unsigned int rate = SAMPLE_RATE;
    int dir = 0;
    if ((err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &rate, &dir)) < 0) {
        fprintf(stderr, "无法设置采样率: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, CHANNELS)) < 0) {
        fprintf(stderr, "无法设置通道数: %s\n", snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_hw_params(capture_handle, hw_params)) < 0) {
        fprintf(stderr, "无法设置硬件参数: %s\n", snd_strerror(err));
        return -1;
    }
    printf("音频录制初始化成功\n");
    if ((err = snd_pcm_prepare(capture_handle)) < 0) {
        fprintf(stderr, "无法准备音频接口 %s\n", snd_strerror(err));
        return -1;
    }
    return 0;
}

static unsigned char* pending_audio_buf = NULL;
static size_t pending_audio_len = 0;
static pthread_mutex_t pending_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

void* audio_capture_thread(void* arg) {
    pthread_mutex_lock(&ws_mutex);
    audio_thread_running = 1;
    pthread_cond_signal(&ws_cond);
    pthread_mutex_unlock(&ws_mutex);

    int err;
    int32_t period_size = WINDOW_SIZE;
    snd_pcm_state_t state;

    state = snd_pcm_state(capture_handle);
    printf("音频设备状态: %s\n", snd_pcm_state_name(state));

    if (state != SND_PCM_STATE_PREPARED) {
        printf("设备状态不正确，重新准备...\n");
        if ((err = snd_pcm_prepare(capture_handle)) < 0) {
            fprintf(stderr, "重新准备音频接口失败: %s\n", snd_strerror(err));
            audio_thread_running = 0;
            interrupted = 1;
            return NULL;
        }
    }

    int vad_silence_frames = 0;
    bool vad_active = false;

    int16_t* frame = malloc(2 * period_size * sizeof(int16_t));
    float* float_frame = malloc(2 * period_size * sizeof(float));
    if (!frame || !float_frame) {
        fprintf(stderr, "无法分配音频缓冲区\n");
        free(frame);
        free(float_frame);
        audio_thread_running = 0;
        interrupted = 1;
        return NULL;
    }

    int retry_count = 0;
    const int max_retries = 5;

    printf("开始音频录制...\n");
    printf("按 Ctrl+C 停止录制\n\n");

    while (1) {
        // 检查中断信号
        if (interrupted) {
            printf("音频线程检测到中断信号，退出循环\n");
            break;
        }

        pthread_mutex_lock(&ws_mutex);
        struct lws* ws_client = websocket_client;
        pthread_mutex_unlock(&ws_mutex);

        if (!ws_client) {
            printf("WebSocket客户端已关闭，音频线程退出\n");
            break;
        }

        err = snd_pcm_readi(capture_handle, frame, period_size);

        if (err == -EPIPE) {
            printf("音频缓冲区溢出，重新准备...\n");
            snd_pcm_prepare(capture_handle);
            retry_count = 0;
            continue;
        }
        else if (err < 0) {
            fprintf(stderr, "读取音频数据错误: %s (错误码: %d)\n", snd_strerror(err), err);
            retry_count++;
            if (retry_count >= max_retries) {
                fprintf(stderr, "达到最大重试次数，退出\n");
                interrupted = 1;
                break;
            }
            printf("重试 %d/%d...\n", retry_count, max_retries);
            usleep(100000);
            snd_pcm_prepare(capture_handle);
            continue;
        }

        retry_count = 0;

        if (err != period_size) {
            printf("读取到 %d 帧，期望 %lu 帧\n", err, period_size);
            continue;
        }

        for (int i = 0; i < period_size; i++) {
            float_frame[i] = frame[i] / 32768.0f;
        }

        SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad, float_frame, period_size);
        while (!SherpaOnnxVoiceActivityDetectorEmpty(vad)) {
            const SherpaOnnxSpeechSegment* segment = SherpaOnnxVoiceActivityDetectorFront(vad);
            printf("检测到语音段\n");
            pthread_mutex_lock(&ws_mutex);
            if (websocket_client) {
                int16_t* int16_buf = malloc(segment->n * sizeof(int16_t));
                if (int16_buf) {
                    for (int i = 0; i < segment->n; i++) {
                        float sample = segment->samples[i];
                        if (sample > 1.0f) sample = 1.0f;
                        if (sample < -1.0f) sample = -1.0f;
                        int16_buf[i] = (int16_t)(sample * 32767.0f);
                    }
                    unsigned char* ws_buf = malloc(LWS_PRE + segment->n * sizeof(int16_t));
                    if (ws_buf) {
                        memcpy(&ws_buf[LWS_PRE], int16_buf, segment->n * sizeof(int16_t));
                        pthread_mutex_lock(&pending_audio_mutex);
                        if (pending_audio_buf) free(pending_audio_buf); // 释放旧数据
                        pending_audio_buf = ws_buf;
                        pending_audio_len = segment->n * sizeof(int16_t);
                        pthread_mutex_unlock(&pending_audio_mutex);
                        lws_callback_on_writable(websocket_client); // 通知可写
                    }
                    else {
                        fprintf(stderr, "ws_buf分配失败\n");
                        free(int16_buf);
                    }
                }
                else {
                    fprintf(stderr, "int16_buf分配失败\n");
                }
                free(int16_buf);
            }
            pthread_mutex_unlock(&ws_mutex);
            SherpaOnnxDestroySpeechSegment(segment);
            SherpaOnnxVoiceActivityDetectorPop(vad);
        }
    }
    free(frame);
    free(float_frame);
    pthread_mutex_lock(&ws_mutex);
    audio_thread_running = 0;
    pthread_cond_signal(&ws_cond);
    pthread_mutex_unlock(&ws_mutex);

    printf("音频录制线程结束\n");
    return NULL;
}

char* extract_content(const char* str) {
    // 1. 找到内容起始位置（第一个 '>' 后的空格之后）
    const char* start = strchr(str, '>'); // 找到第一个 '>'
    if (!start) return NULL;
    start = strchr(start, ' ');           // 找到后面的空格
    if (!start) return NULL;
    start++;                              // 跳过空格

    // 2. 找到内容结束位置（下一个 '<' 之前）
    const char* end = strchr(start, '<');
    if (!end) return NULL;

    // 3. 计算长度并分配内存
    size_t len = end - start;
    char* result = (char*)malloc(len + 1); // +1 为 '\0'
    if (!result) return NULL;

    // 4. 复制子字符串
    strncpy(result, start, len);
    result[len] = '\0'; // 添加终止符

    return result;
}

static int need_send_reset = 0; // 新增标志

static int callback_realtime(struct lws* wsi, enum lws_callback_reasons reason,
    void* user, void* in, size_t len) {
    struct per_session_data* pss = (struct per_session_data*)user;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf("WebSocket连接建立成功\n");
        websocket_client = wsi;
        pss->established = 1;
        need_send_reset = 1; // 标记需要发送 reset
        lws_callback_on_writable(wsi); // 通知可写
        // 启动音频线程...
        pthread_mutex_lock(&ws_mutex);
        if (pthread_create(&audio_thread, NULL, audio_capture_thread, NULL) != 0) {
            pthread_mutex_unlock(&ws_mutex);
            fprintf(stderr, "创建音频线程失败\n");
            return -1;
        }
        while (!audio_thread_running) {
            pthread_cond_wait(&ws_cond, &ws_mutex);
        }
        pthread_mutex_unlock(&ws_mutex);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (len > 0) {
            char* message = malloc(len + 1);
            memcpy(message, in, len);
            message[len] = '\0';
            cJSON* json = cJSON_Parse(message);
            if (json) {
                cJSON* transcript = cJSON_GetObjectItem(json, "transcript");
                if (transcript && cJSON_IsString(transcript)) {
                    char* content = extract_content(transcript->valuestring);
                    printf("识别结果: %s\n", content);
                    free(content);
                }
                cJSON* status = cJSON_GetObjectItem(json, "status");
                if (status && cJSON_IsString(status)) {
                    printf("状态: %s\n", status->valuestring);
                }
                cJSON* action = cJSON_GetObjectItem(json, "action");
                if (action && cJSON_IsString(action) &&
                    strcmp(action->valuestring, "stopped") == 0) {
                    printf("服务器确认停止\n");
                    interrupted = 1;
                }
                cJSON_Delete(json);
            }
            free(message);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf("WebSocket连接错误\n");
        websocket_client = NULL;
        interrupted = 1;
        break;

    case LWS_CALLBACK_CLOSED:
        printf("WebSocket连接关闭\n");
        websocket_client = NULL;
        interrupted = 1;
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        // 优先发送 reset 命令
        if (need_send_reset) {
            const char* reset_text = "{\"action\": \"reset\"}";
            size_t len = strlen(reset_text);
            unsigned char* buf = malloc(LWS_PRE + len);
            if (buf) {
                memcpy(&buf[LWS_PRE], reset_text, len);
                lws_write(wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
                free(buf);
            }
            need_send_reset = 0;
            break;
        }
        // 直接发送待发送音频数据
        pthread_mutex_lock(&pending_audio_mutex);
        if (pending_audio_buf && pending_audio_len > 0) {
            lws_write(wsi, &pending_audio_buf[LWS_PRE], pending_audio_len, LWS_WRITE_BINARY);
            free(pending_audio_buf);
            pending_audio_buf = NULL;
            pending_audio_len = 0;
        }
        pthread_mutex_unlock(&pending_audio_mutex);
        break;
    }

    default:
        break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    {
        "realtime-protocol",
        callback_realtime,
        sizeof(struct per_session_data),
        4096,
    },
    { NULL, NULL, 0, 0 }
};

void cleanup() {
    printf("正在清理资源...\n");
    interrupted = 1;
    pthread_mutex_lock(&ws_mutex);
    pthread_cond_broadcast(&ws_cond);
    pthread_mutex_unlock(&ws_mutex);
    if (audio_thread) {
        pthread_join(audio_thread, NULL);
        audio_thread = 0;
    }
    if (capture_handle) {
        snd_pcm_close(capture_handle);
        capture_handle = NULL;
    }
    if (vad) {
        SherpaOnnxDestroyVoiceActivityDetector(vad);
        vad = NULL;
    }
    printf("资源清理完成\n");
}

int main(int argc, char** argv) {
    struct lws_context_creation_info info;
    struct lws_client_connect_info connect_info;
    struct lws_context* context;
    const char* server_address = "192.168.0.202";
    int port = 9877;
    const char* path = "/ws/realtime";

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("实时语音识别客户端启动\n");
    printf("服务器: %s:%d%s\n", server_address, port, path);

    if (init_audio() < 0) {
        return -1;
    }

    memset(&vadConfig, 0, sizeof(vadConfig));
    vadConfig.silero_vad.model = "./silero_vad.onnx";
    // 默认阈值
    vadConfig.silero_vad.threshold = 0.2;
    // 解析命令行参数 -th
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-th") == 0) {
            vadConfig.silero_vad.threshold = atof(argv[i + 1]);
        }
    }
    // 检查阈值范围
    if (vadConfig.silero_vad.threshold <= 0.0 || vadConfig.silero_vad.threshold >= 1.0) {
        fprintf(stderr, "错误: 阈值必须在0到1之间 (当前值: %.2f)\n", vadConfig.silero_vad.threshold);
        return -1;
    }
    vadConfig.silero_vad.min_silence_duration = 0.5;
    vadConfig.silero_vad.min_speech_duration = 0.5;
    vadConfig.silero_vad.max_speech_duration = 10;
    vadConfig.silero_vad.window_size = WINDOW_SIZE;
    vadConfig.sample_rate = SAMPLE_RATE;
    vadConfig.num_threads = 1;
    vadConfig.debug = 0;
    printf("VAD 阈值设置为: %.2f\n", vadConfig.silero_vad.threshold);

    vad = SherpaOnnxCreateVoiceActivityDetector(&vadConfig, 30);
    if (!vad) {
        fprintf(stderr, "VAD初始化失败\n");
        cleanup();
        return -1;
    }
    printf("VAD初始化成功\n");

    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "创建WebSocket上下文失败\n");
        cleanup();
        return -1;
    }

    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    connect_info.address = server_address;
    connect_info.port = port;
    connect_info.path = path;
    connect_info.host = server_address;
    connect_info.protocol = protocols[0].name;
    connect_info.ssl_connection = LCCSCF_USE_SSL |
        LCCSCF_ALLOW_SELFSIGNED |
        LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

    struct lws* wsi = lws_client_connect_via_info(&connect_info);
    if (!wsi) {
        fprintf(stderr, "WebSocket连接失败\n");
        unsigned long ssl_err = ERR_get_error();
        if (ssl_err != 0) {
            char err_buf[256];
            ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
            fprintf(stderr, "SSL错误: %s\n", err_buf);
        }
        lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, NULL);
        lws_context_destroy(context);
        cleanup();
        return -1;
    }

    printf("等待WebSocket连接建立...\n");
    while (!interrupted) {
        lws_service(context, 50);
        pthread_mutex_lock(&ws_mutex);
        if (audio_thread != 0 && !audio_thread_running) {
            printf("检测到音频线程已结束，程序退出\n");
            interrupted = 1;
            pthread_mutex_unlock(&ws_mutex);
            break;
        }
        pthread_mutex_unlock(&ws_mutex);
    }

    printf("开始清理过程...\n");

    // 退出前发送 stop 命令
    if (websocket_client) {
        const char* stop_cmd = "{\"action\": \"stop\"}";
        size_t cmd_len = strlen(stop_cmd);
        unsigned char* buf = malloc(LWS_PRE + cmd_len);
        if (buf) {
            memcpy(&buf[LWS_PRE], stop_cmd, cmd_len);
            lws_write(websocket_client, &buf[LWS_PRE], cmd_len, LWS_WRITE_TEXT);
            free(buf);
            // 等待一会儿让 stop 命令发出去
            lws_service(context, 100);
        }
        else {
            fprintf(stderr, "分配stop命令缓冲区失败\n");
        }
    }

    cleanup();
    lws_context_destroy(context);
    printf("程序结束\n");
    return 0;
}
