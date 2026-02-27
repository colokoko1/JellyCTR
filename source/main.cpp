#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <sys/stat.h>
#include <jpeglib.h>

#include "gfx/images/icons.h"

// --- Constants & Config ---
#define CONFIG_DIR      "sdmc:/3ds/JellyCTR"
#define CONFIG_PATH     "sdmc:/3ds/JellyCTR/config.json"
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000 
#define MAX_JSON_SIZE   (1024 * 1024) 
#define AUDIO_BUF_SIZE  (32 * 1024) 
#define NUM_BUFFERS     16
#define HARDWARE_RATE   48000 
#define TS_PACKET_SIZE  188
#define MVD_BUFFER_SIZE (1024 * 1024 * 4) // Expanded to 4MB

#define CLR_BLACK       C2D_Color32(0, 0, 0, 255)
#define CLR_WHITE       C2D_Color32(255, 255, 255, 255)
#define CLR_ACCENT      C2D_Color32(230, 190, 140, 255) 
#define CLR_DIM         C2D_Color32(100, 100, 100, 255)
#define CLR_CARD        C2D_Color32(40, 40, 40, 255)

enum LoopMode { LOOP_OFF, LOOP_ALL, LOOP_ONE };
enum AppState { STATE_LIBRARIES, STATE_ALBUMS, STATE_SONGS, STATE_PLAYER, STATE_VIDEO_PLAYER };

struct MediaItem { 
    std::string Name, Id, Album, Artist, Type, CollectionType;
    int64_t DurationTicks;
    bool IsVideo, IsFolder;
};

struct ReportPayload {
    std::string action, item_id, server_url, access_token;
    int64_t ticks;
};

// --- Globals ---
AppState current_state = STATE_LIBRARIES;
std::vector<MediaItem> current_libraries;
std::vector<MediaItem> current_list;
std::vector<int> playback_queue;
int queue_index = 0, scroll_index = 0, repeat_timer = 0;
bool is_shuffled = false;
bool is_playing = false; 
LoopMode loop_mode = LOOP_OFF;

u8* audio_ptr = NULL;
ndspWaveBuf waveBuf[NUM_BUFFERS];
volatile int write_node = 0; 
size_t buf_pos = 0;
volatile uint64_t total_samples_played = 0;
volatile bool thread_run = false, is_paused = false;

char current_song_name[128], current_album_name[128], current_song_id[64];
double current_duration_seconds = 0;
pthread_t play_thread = 0, video_thread = 0;
int last_report_second = 0;

char access_token[256] = {0}, server_url[256] = {0}, user_id[128] = {0}; 
char* json_buffer = NULL;
size_t json_len = 0;
static u32* soc_buffer = NULL;

bool mvd_available = false;
u8* nal_buffer = NULL; 
size_t nal_buffer_pos = 0;
u8  ts_fragment[TS_PACKET_SIZE];
size_t ts_fragment_pos = 0;
int video_pid = -1;
int audio_pid = -1;
int frames_decoded = 0;

u32* mvd_out_buffer = NULL;
MVDSTD_Config mvd_config;

// Sync & Texture Globals
double start_video_pts = -1.0;
double current_video_pts = 0.0;
u16* video_tex_buffer = NULL;
C3D_Tex video_tex;
C2D_Image video_image;

C3D_RenderTarget *top_target, *bottom_target;
C2D_TextBuf g_dynamicBuf;
C3D_Tex album_art_tex;
C2D_Image album_art_image;
bool has_album_art = false;
C2D_SpriteSheet sprite_sheet;

// --- Helper UI ---
void draw_loading() {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW); 
    C2D_TextBufClear(g_dynamicBuf);
    C2D_TargetClear(bottom_target, CLR_BLACK); 
    C2D_SceneBegin(bottom_target);
    C2D_Text ld; C2D_TextParse(&ld, g_dynamicBuf, "Loading..."); 
    C2D_DrawText(&ld, C2D_WithColor, 120, 110, 0.5f, 0.8f, 0.8f, CLR_WHITE); 
    C3D_FrameEnd(0);
}

// --- Playback Reporting ---
void* report_thread_func(void* arg) {
    ReportPayload* rp = (ReportPayload*)arg;
    CURL *curl = curl_easy_init();
    char url[512]; snprintf(url, 512, "%s/Sessions/%s", rp->server_url.c_str(), rp->action.c_str());
    json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "ItemId", json_object_new_string(rp->item_id.c_str()));
    json_object_object_add(jobj, "PositionTicks", json_object_new_int64(rp->ticks));
    json_object_object_add(jobj, "PlayMethod", json_object_new_string("Transcode"));
    json_object_object_add(jobj, "PlaySessionId", json_object_new_string("JCTR-SESSION-01"));
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    char auth[512]; snprintf(auth, sizeof(auth), "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Token=\"%s\"", rp->access_token.c_str());
    h = curl_slist_append(h, auth);
    curl_easy_setopt(curl, CURLOPT_URL, url); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_object_to_json_string(jobj));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* p, size_t s, size_t n, void* u) -> size_t { return s * n; });
    curl_easy_perform(curl); curl_slist_free_all(h); json_object_put(jobj); curl_easy_cleanup(curl);
    delete rp; return NULL;
}

void report_playback(const std::string& action, const std::string& item_id, int64_t ticks) {
    if (strlen(access_token) == 0) return;
    ReportPayload* rp = new ReportPayload{action, item_id, server_url, access_token, ticks};
    pthread_t r_thread; if (pthread_create(&r_thread, NULL, report_thread_func, rp) == 0) pthread_detach(r_thread); else delete rp;
}

uint32_t get_tiled_offset(uint32_t x, uint32_t y, uint32_t w) {
    return ((((y >> 3) * (w >> 3) + (x >> 3)) << 6) + ((x & 1) << 0) + ((y & 1) << 1) + ((x & 2) << 1) + ((y & 2) << 2) + ((x & 4) << 2) + ((y & 4) << 3));
}

// --- Hardware Video Logic ---
inline u16 YUV_to_RGB565(int y, int u, int v) {
    y -= 16; u -= 128; v -= 128;
    int r = (298 * y + 409 * v + 128) >> 8;
    int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
    int b = (298 * y + 516 * u + 128) >> 8;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void convert_and_tile_yuyv(u32* yuyv_data, u16* tiled_out) {
    u8* in = (u8*)yuyv_data;
    for (u32 y = 0; y < 240; y++) {
        for (u32 x = 0; x < 400; x += 2) {
            int y0 = in[0]; int u  = in[1];
            int y1 = in[2]; int v  = in[3];
            in += 4;
            tiled_out[get_tiled_offset(x, y, 512)] = YUV_to_RGB565(y0, u, v);
            tiled_out[get_tiled_offset(x + 1, y, 512)] = YUV_to_RGB565(y1, u, v);
        }
    }
    GSPGPU_FlushDataCache(tiled_out, 512 * 256 * 2);
}

void process_nal_unit(u8* data, size_t size) {
    if (!mvd_available || size < 4) return;
    Result res = mvdstdProcessVideoFrame(data, size, 0, NULL);
    if (R_SUCCEEDED(res)) {
        // A/V Sync Logic
        if (start_video_pts >= 0.0) {
            double video_time = current_video_pts - start_video_pts;
            double audio_time = (double)total_samples_played / HARDWARE_RATE;
            
            // If video is rendering too fast, wait for audio
            while (thread_run && video_time > audio_time + 0.03) {
                svcSleepThread(5000000); // 5ms sleep
                audio_time = (double)total_samples_played / HARDWARE_RATE;
            }
        }

        mvdstdRenderVideoFrame(&mvd_config, true); 
        
        if (video_tex_buffer && mvd_out_buffer) {
            // Tell the CPU to dump its cached "green" zeros and read the actual RAM
            GSPGPU_InvalidateDataCache(mvd_out_buffer, 400 * 240 * 2);
            
            convert_and_tile_yuyv(mvd_out_buffer, video_tex_buffer);
            frames_decoded++;
        }
    }
}
size_t video_ts_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!thread_run) return 0;
    size_t total = size * nmemb; u8* data = (u8*)ptr;
    for (size_t i = 0; i < total; i++) {
        ts_fragment[ts_fragment_pos++] = data[i];
        if (ts_fragment_pos == TS_PACKET_SIZE) {
            if (ts_fragment[0] == 0x47) {
                int pid = ((ts_fragment[1] & 0x1F) << 8) | ts_fragment[2];
                int pusi = (ts_fragment[1] & 0x40) >> 6; 
                int afc = (ts_fragment[3] & 0x30) >> 4;
                
                int payload_offset = 4; 
                if (afc == 2 || afc == 3) payload_offset += 1 + ts_fragment[4];

                if ((afc == 1 || afc == 3) && payload_offset < TS_PACKET_SIZE) {
                    if (pusi && payload_offset + 3 < TS_PACKET_SIZE) {
                        if (ts_fragment[payload_offset] == 0x00 && ts_fragment[payload_offset+1] == 0x00 && ts_fragment[payload_offset+2] == 0x01) {
                            u8 stream_id = ts_fragment[payload_offset+3];
                            
                            if ((stream_id & 0xF0) == 0xE0) {
                                video_pid = pid; 
                                if (ts_fragment[payload_offset+7] & 0x80) { 
                                    uint64_t pts = ((uint64_t)(ts_fragment[payload_offset+9] & 0x0E) << 29) | ((uint64_t)(ts_fragment[payload_offset+10]) << 22) | ((uint64_t)(ts_fragment[payload_offset+11] & 0xFE) << 14) | ((uint64_t)(ts_fragment[payload_offset+12]) << 7) | ((uint64_t)(ts_fragment[payload_offset+13] & 0xFE) >> 1);
                                    current_video_pts = (double)pts / 90000.0;
                                    if (start_video_pts < 0.0) start_video_pts = current_video_pts;
                                }
                                payload_offset += 9 + ts_fragment[payload_offset+8];
                            } 
                            else if ((stream_id & 0xE0) == 0xC0 || stream_id == 0xBD) {
                                audio_pid = pid;
                                payload_offset += 9 + ts_fragment[payload_offset+8];
                            }
                        }
                    }

                    if (video_pid == pid && payload_offset < TS_PACKET_SIZE) {
                        size_t ps = TS_PACKET_SIZE - payload_offset;
                        if (nal_buffer_pos + ps < MVD_BUFFER_SIZE) {
                            memcpy(nal_buffer + nal_buffer_pos, ts_fragment + payload_offset, ps); nal_buffer_pos += ps;
                            if (nal_buffer_pos >= 6) {
                                int s_idx = -1; for (size_t j = 0; j <= nal_buffer_pos - 3; j++) { if (nal_buffer[j] == 0x00 && nal_buffer[j+1] == 0x00 && nal_buffer[j+2] == 0x01) { s_idx = j; break; } }
                                if (s_idx > 0) { memmove(nal_buffer, nal_buffer + s_idx, nal_buffer_pos - s_idx); nal_buffer_pos -= s_idx; s_idx = 0; }
                                if (s_idx == 0) {
                                    int n_idx = -1; for (size_t j = 3; j <= nal_buffer_pos - 3; j++) { if (nal_buffer[j] == 0x00 && nal_buffer[j+1] == 0x00 && nal_buffer[j+2] == 0x01) { n_idx = j; if (j > 0 && nal_buffer[j-1] == 0x00) n_idx = j - 1; break; } }
                                    if (n_idx > 0) { process_nal_unit(nal_buffer, n_idx); memmove(nal_buffer, nal_buffer + n_idx, nal_buffer_pos - n_idx); nal_buffer_pos -= n_idx; }
                                }
                            }
                        } else nal_buffer_pos = 0; 
                    }
                    else if (audio_pid == pid && payload_offset < TS_PACKET_SIZE) {
                        size_t ps = TS_PACKET_SIZE - payload_offset;
                        for (size_t j = 0; j < ps; j++) {
                            while (thread_run && waveBuf[write_node].status != NDSP_WBUF_FREE && waveBuf[write_node].status != NDSP_WBUF_DONE) svcSleepThread(100000);
                            if (!thread_run) break;
                            
                            audio_ptr[write_node * AUDIO_BUF_SIZE + buf_pos] = ts_fragment[payload_offset + j];
                            buf_pos++;
                            
                            if (buf_pos >= AUDIO_BUF_SIZE) {
                                DSP_FlushDataCache(audio_ptr + (write_node * AUDIO_BUF_SIZE), AUDIO_BUF_SIZE);
                                waveBuf[write_node].nsamples = AUDIO_BUF_SIZE / 4; 
                                ndspChnWaveBufAdd(0, &waveBuf[write_node]);
                                total_samples_played += (AUDIO_BUF_SIZE / 4); 
                                write_node = (write_node + 1) % NUM_BUFFERS; 
                                buf_pos = 0;
                            }
                        }
                    }
                }
            }
            ts_fragment_pos = 0; 
        }
    }
    
    int current_sec = total_samples_played / HARDWARE_RATE; 
    if (current_sec - last_report_second >= 10) {
        report_playback("Playing/Progress", current_song_id, current_sec * 10000000LL);
        last_report_second = current_sec;
    }

    return total;
}

// --- Queue & Graphics ---
void build_queue(int start_index) {
    playback_queue.clear();
    for (int i = 0; i < (int)current_list.size(); i++) playback_queue.push_back(i);
    if (is_shuffled) {
        std::random_device rd; std::mt19937 g(rd()); std::shuffle(playback_queue.begin(), playback_queue.end(), g);
        for (int i = 0; i < (int)playback_queue.size(); i++) { if (playback_queue[i] == start_index) { std::swap(playback_queue[0], playback_queue[i]); break; } }
    } else {
        std::rotate(playback_queue.begin(), playback_queue.begin() + start_index, playback_queue.end());
    }
    queue_index = 0;
}

void decode_jpeg(u8* src_data, size_t src_size) {
    if (src_size < 100 || src_data[0] != 0xFF || src_data[1] != 0xD8) return;
    if (has_album_art) { C3D_TexDelete(&album_art_tex); has_album_art = false; }
    struct jpeg_decompress_struct cinfo; struct jpeg_error_mgr jerr; cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo); jpeg_mem_src(&cinfo, src_data, src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return; }
    cinfo.out_color_space = JCS_RGB; jpeg_start_decompress(&cinfo);  
    C3D_TexInit(&album_art_tex, 256, 256, GPU_RGBA8); u32* flat = (u32*)linearAlloc(256 * 256 * 4);
    if (!flat) { jpeg_destroy_decompress(&cinfo); return; }
    memset(flat, 0, 256 * 256 * 4); u8* r_ptr = (u8*)malloc(cinfo.output_width * 3);
    while (cinfo.output_scanline < cinfo.output_height && cinfo.output_scanline < 256) {
        jpeg_read_scanlines(&cinfo, &r_ptr, 1);
        for(u32 x = 0; x < (u32)cinfo.output_width && x < 256; x++) { u8 r = r_ptr[x * 3], g = r_ptr[x * 3 + 1], b = r_ptr[x * 3 + 2]; flat[(cinfo.output_scanline - 1) * 256 + x] = (0xFF) | (b << 8) | (g << 16) | (r << 24); }
    }
    free(r_ptr); u32* t_data = (u32*)album_art_tex.data;
    for (u32 y = 0; y < 256; y++) { for (u32 x = 0; x < 256; x++) { t_data[get_tiled_offset(x, y, 256)] = flat[y * 256 + x]; } }
    GSPGPU_FlushDataCache(album_art_tex.data, 256 * 256 * 4); jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); linearFree(flat);
    album_art_image.tex = &album_art_tex; static Tex3DS_SubTexture sub = { 256, 256, 0.0f, 1.0f, 1.0f, 0.0f }; album_art_image.subtex = &sub; has_album_art = true;
}

size_t audio_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!thread_run) return 0;
    size_t total = size * nmemb; u8* data = (u8*)ptr;
    for (size_t i = 0; i < total; i++) {
        while (thread_run && waveBuf[write_node].status != NDSP_WBUF_FREE && waveBuf[write_node].status != NDSP_WBUF_DONE) svcSleepThread(100000);
        if (!thread_run) break;
        audio_ptr[write_node * AUDIO_BUF_SIZE + buf_pos] = data[i]; buf_pos++;
        if (buf_pos >= AUDIO_BUF_SIZE) {
            DSP_FlushDataCache(audio_ptr + (write_node * AUDIO_BUF_SIZE), AUDIO_BUF_SIZE);
            waveBuf[write_node].nsamples = AUDIO_BUF_SIZE / 4; ndspChnWaveBufAdd(0, &waveBuf[write_node]);
            total_samples_played += (AUDIO_BUF_SIZE / 4); write_node = (write_node + 1) % NUM_BUFFERS; buf_pos = 0;
        }
    }
    return total;
}

// --- Data Fetching ---
void fetch_items(const std::string& query_url) {
    if (strlen(access_token) == 0) return;
    draw_loading(); json_len = 0; current_list.clear();
    CURL *curl = curl_easy_init();
    char auth[512]; snprintf(auth, sizeof(auth), "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Token=\"%s\"", access_token);
    struct curl_slist *h = curl_slist_append(NULL, auth);
    curl_easy_setopt(curl, CURLOPT_URL, query_url.c_str()); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t s, size_t n, void* u) -> size_t { size_t t = s * n; if (json_len + t < MAX_JSON_SIZE - 1) { memcpy(json_buffer + json_len, ptr, t); json_len += t; json_buffer[json_len] = '\0'; } return t; });
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    if(curl_easy_perform(curl) == CURLE_OK) {
        struct json_object *parsed = json_tokener_parse(json_buffer), *items;
        if (parsed && json_object_object_get_ex(parsed, "Items", &items) && items) {
            for (size_t i = 0; i < (size_t)json_object_array_length(items); i++) {
                struct json_object *it = json_object_array_get_idx(items, i), *n, *id, *type, *folder, *ct; MediaItem mi;
                if (json_object_object_get_ex(it, "Name", &n) && n) mi.Name = json_object_get_string(n);
                if (json_object_object_get_ex(it, "Id", &id) && id) mi.Id = json_object_get_string(id);
                if (json_object_object_get_ex(it, "CollectionType", &ct) && ct) mi.CollectionType = json_object_get_string(ct);
                mi.IsVideo = false; mi.IsFolder = false;
                if (json_object_object_get_ex(it, "IsFolder", &folder) && folder) mi.IsFolder = json_object_get_boolean(folder);
                if (json_object_object_get_ex(it, "Type", &type) && type) {
                    const char* t = json_object_get_string(type);
                    if (strcmp(t, "Episode") == 0 || strcmp(t, "Movie") == 0) mi.IsVideo = true;
                    if (strcmp(t, "Series") == 0 || strcmp(t, "Season") == 0) mi.IsFolder = true;
                }
                current_list.push_back(mi);
            }
        }
        if (parsed) json_object_put(parsed);
    }
    curl_slist_free_all(h); curl_easy_cleanup(curl);
}

void fetch_libraries() {
    std::string url = std::string(server_url) + "/Users/" + user_id + "/Views";
    fetch_items(url); current_libraries.clear();
    for(auto& mi : current_list) current_libraries.push_back(mi);
    current_list.clear();
}

// --- Playback Logic ---
void stop_playback() {
    thread_run = false; is_paused = false;
    if (is_playing) report_playback("Playing/Stopped", current_song_id, total_samples_played / 48000 * 10000000LL);
    if (play_thread) { pthread_join(play_thread, NULL); play_thread = 0; }
    if (video_thread) { pthread_join(video_thread, NULL); video_thread = 0; }
    
    if (mvd_out_buffer) { linearFree(mvd_out_buffer); mvd_out_buffer = NULL; }
    if (video_tex_buffer) { linearFree(video_tex_buffer); video_tex_buffer = NULL; C3D_TexDelete(&video_tex); }
    
    ndspChnReset(0);
}

void play_current_queue_item() {
    stop_playback(); if (queue_index >= (int)playback_queue.size()) return;
    MediaItem item = current_list[playback_queue[queue_index]];
    strncpy(current_song_id, item.Id.c_str(), 63); current_song_id[63] = '\0';
    strncpy(current_song_name, item.Name.c_str(), 127); current_song_name[127] = '\0';
    is_playing = true; report_playback("Playing", current_song_id, 0);

    if (item.IsVideo) {
        current_state = STATE_VIDEO_PLAYER; 
        nal_buffer_pos = 0; ts_fragment_pos = 0; video_pid = -1; audio_pid = -1; frames_decoded = 0;
        start_video_pts = -1.0; current_video_pts = 0.0;
        
        mvd_out_buffer = (u32*)linearAlloc(400 * 240 * 2); 
        mvdstdGenerateDefaultConfig(&mvd_config, 400, 240, 400, 240, NULL, mvd_out_buffer, NULL);
        MVDSTD_SetConfig(&mvd_config); 
        
        video_tex_buffer = (u16*)linearAlloc(512 * 256 * 2);
        C3D_TexInit(&video_tex, 512, 256, GPU_RGB565);
        video_tex.data = video_tex_buffer; 
        
        static Tex3DS_SubTexture video_subtex = { 400, 240, 0.0f, 400.0f/512.0f, 240.0f/256.0f, 0.0f };
        video_image.tex = &video_tex;
        video_image.subtex = &video_subtex;

        total_samples_played = 0; write_node = 0; buf_pos = 0;
        ndspChnInitParams(0); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0, HARDWARE_RATE);
        for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].status = NDSP_WBUF_FREE;

        thread_run = true;
        pthread_create(&video_thread, NULL, [](void* arg) -> void* {
            CURL *curl = curl_easy_init(); char s[2048]; 
            snprintf(s, 2048, "%s/Videos/%s/stream?static=false&videoCodec=h264&profile=baseline&width=400&height=240&videoBitRate=800000&container=ts&audioCodec=pcm_s16le&api_key=%s", server_url, current_song_id, access_token);
            curl_easy_setopt(curl, CURLOPT_URL, s); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, video_ts_callback); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_perform(curl); curl_easy_cleanup(curl); thread_run = false; return NULL;
        }, NULL);
        return;
    }
    
    current_state = STATE_PLAYER; draw_loading(); 
    struct DLBuf { u8* b; size_t s; } d; d.b = (u8*)malloc(512 * 1024); d.s = 0;
    CURL* img_c = curl_easy_init(); char img_url[1024]; snprintf(img_url, 1024, "%s/Items/%s/Images/Primary?maxWidth=256&api_key=%s", server_url, current_song_id, access_token);
    curl_easy_setopt(img_c, CURLOPT_URL, img_url); curl_easy_setopt(img_c, CURLOPT_WRITEFUNCTION, +[](void* p, size_t s, size_t n, void* u) -> size_t { struct DLBuf* db = (struct DLBuf*)u; size_t bytes = s * n; if (db->s + bytes > 512 * 1024) return bytes; memcpy(db->b + db->s, p, bytes); db->s += bytes; return bytes; });
    curl_easy_setopt(img_c, CURLOPT_WRITEDATA, &d); curl_easy_setopt(img_c, CURLOPT_SSL_VERIFYPEER, 0L);
    if (curl_easy_perform(img_c) == CURLE_OK) decode_jpeg(d.b, d.s);
    curl_easy_cleanup(img_c); free(d.b); 
    
    total_samples_played = 0; write_node = 0; buf_pos = 0;
    ndspChnInitParams(0); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0, HARDWARE_RATE);
    for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].status = NDSP_WBUF_FREE;
    
    thread_run = true; pthread_create(&play_thread, NULL, [](void* arg) -> void* {
        CURL *curl = curl_easy_init(); char s[2048]; snprintf(s, 2048, "%s/Audio/%s/stream?static=false&container=wav&audioCodec=pcm_s16le&audioSampleRate=48000&audioBitRate=1536000&audioChannels=2&maxAudioChannels=2&api_key=%s", server_url, current_song_id, access_token);
        curl_easy_setopt(curl, CURLOPT_URL, s); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audio_callback); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); curl_easy_perform(curl); curl_easy_cleanup(curl); thread_run = false; return NULL;
    }, NULL);
}

// --- Main ---
int main(int argc, char* argv[]) {
    gfxInitDefault(); C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT); bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_dynamicBuf = C2D_TextBufNew(4096); ndspInit(); ptmuInit();
    if (R_SUCCEEDED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_YUYV422, MVD_DEFAULT_WORKBUF_SIZE, NULL))) mvd_available = true;
    audio_ptr = (u8*)linearAlloc(AUDIO_BUF_SIZE * NUM_BUFFERS); nal_buffer = (u8*)linearAlloc(MVD_BUFFER_SIZE); 
    json_buffer = (char*)malloc(MAX_JSON_SIZE); soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE); if(soc_buffer) socInit(soc_buffer, SOC_BUFFERSIZE);
    for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].data_vaddr = audio_ptr + (i * AUDIO_BUF_SIZE);
    sprite_sheet = C2D_SpriteSheetLoad("sdmc:/3ds/JellyCTR/icons.t3x"); mkdir(CONFIG_DIR, 0777);
    
    FILE* fr = fopen(CONFIG_PATH, "r");
    if (fr) {
        fseek(fr, 0, SEEK_END); long sz = ftell(fr); fseek(fr, 0, SEEK_SET); 
        char* t = (char*)malloc(sz + 1); fread(t, 1, sz, fr); t[sz] = '\0'; fclose(fr);
        json_object *p = json_tokener_parse(t), *u, *tok, *uid;
        if (json_object_object_get_ex(p, "server_url", &u) && u) strncpy(server_url, json_object_get_string(u), 255);
        if (json_object_object_get_ex(p, "access_token", &tok) && tok) strncpy(access_token, json_object_get_string(tok), 255);
        if (json_object_object_get_ex(p, "user_id", &uid) && uid) strncpy(user_id, json_object_get_string(uid), 127);
        json_object_put(p); free(t);
    }
    fetch_libraries();

    while (aptMainLoop()) {
        hidScanInput(); u32 kD = hidKeysDown();
        if (kD & KEY_START) break;
        if (kD & KEY_B) {
            if (current_state == STATE_PLAYER || current_state == STATE_VIDEO_PLAYER) { stop_playback(); is_playing = false; current_state = STATE_SONGS; }
            else if (current_state == STATE_SONGS) { current_state = STATE_ALBUMS; scroll_index = 0; }
            else if (current_state == STATE_ALBUMS) { current_state = STATE_LIBRARIES; scroll_index = 0; }
        }
        if (current_state != STATE_PLAYER && current_state != STATE_VIDEO_PLAYER) {
            if (kD & KEY_DUP) scroll_index--; if (kD & KEY_DDOWN) scroll_index++;
            size_t max = (current_state == STATE_LIBRARIES) ? current_libraries.size() : current_list.size();
            if (max > 0) { if (scroll_index < 0) scroll_index = 0; if (scroll_index >= (int)max) scroll_index = (int)max - 1; }
            if (kD & KEY_A && max > 0) {
                MediaItem selected = (current_state == STATE_LIBRARIES) ? current_libraries[scroll_index] : current_list[scroll_index];
                if (selected.IsFolder) {
                    std::string url = std::string(server_url) + "/Items?ParentId=" + selected.Id;
                    if (selected.CollectionType == "music") url += "&IncludeItemTypes=MusicAlbum&Recursive=true";
                    url += "&SortBy=SortName"; fetch_items(url);
                    if (current_state == STATE_LIBRARIES) current_state = STATE_ALBUMS; else current_state = STATE_SONGS;
                    scroll_index = 0;
                } else {
                    build_queue(scroll_index); play_current_queue_item();
                }
            }
        }
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW); C2D_TextBufClear(g_dynamicBuf);
        C2D_TargetClear(top_target, CLR_BLACK); C2D_SceneBegin(top_target);
        if (current_state == STATE_VIDEO_PLAYER) { 
            if (frames_decoded > 0) {
                C2D_DrawImageAt(video_image, 0, 0, 0.5f, NULL, 1.0f, 1.0f);
            } else {
                char s[64]; snprintf(s, 64, "Buffering... Frames: %d", frames_decoded); 
                C2D_Text tv; C2D_TextParse(&tv, g_dynamicBuf, s); 
                C2D_DrawText(&tv, C2D_WithColor, 15, 100, 0.5f, 0.65f, 0.65f, CLR_WHITE); 
            }
        }
        else { 
            if (has_album_art) C2D_DrawImageAt(album_art_image, 15, 30, 0.5f, NULL, 0.65f, 0.65f);
            C2D_Text t1; C2D_TextParse(&t1, g_dynamicBuf, is_playing ? current_song_name : "JellyCTR Ready"); 
            C2D_DrawText(&t1, C2D_WithColor, 200, 40, 0.5f, 0.65f, 0.65f, CLR_WHITE); 
        }
        C2D_TargetClear(bottom_target, CLR_BLACK); C2D_SceneBegin(bottom_target);
        if (current_state == STATE_LIBRARIES) { for(int i=0; i<10; i++) { int idx = scroll_index-4+i; if(idx>=0 && idx<(int)current_libraries.size()) { C2D_Text t; C2D_TextParse(&t, g_dynamicBuf, current_libraries[idx].Name.c_str()); C2D_DrawText(&t, C2D_WithColor, 15, 15+(i*22), 0.5f, 0.5f, 0.5f, (idx==scroll_index)?CLR_ACCENT:CLR_DIM); } } }
        else { for(int i=0; i<10; i++) { int idx = scroll_index-4+i; if(idx>=0 && idx<(int)current_list.size()) { C2D_Text t; C2D_TextParse(&t, g_dynamicBuf, current_list[idx].Name.c_str()); C2D_DrawText(&t, C2D_WithColor, 15, 15+(i*22), 0.5f, 0.5f, 0.5f, (idx==scroll_index)?CLR_ACCENT:CLR_DIM); } } }
        C3D_FrameEnd(0);
    }
    stop_playback(); linearFree(audio_ptr); linearFree(nal_buffer); socExit(); ndspExit(); C2D_Fini(); C3D_Fini(); gfxExit();
    return 0;
}