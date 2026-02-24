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
#include <inttypes.h>

#include "gfx/images/icons.h" 

// --- Constants ---
#define CONFIG_DIR      "sdmc:/3ds/JellyCTR"
#define CONFIG_JSON     "sdmc:/3ds/JellyCTR/config.json"
#define ICONS_PATH      "sdmc:/3ds/JellyCTR/icons.t3x"
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000 
#define MAX_JSON_SIZE   (512 * 1024)
#define AUDIO_BUF_SIZE  (32 * 1024) 
#define NUM_BUFFERS     24 
#define HARDWARE_RATE   48000 
#define TS_PACKET_SIZE  188

enum LoopMode { LOOP_OFF, LOOP_ALL, LOOP_ONE };
enum AppState { STATE_LIBRARIES, STATE_ALBUMS, STATE_SONGS, STATE_PLAYER, STATE_VIDEO };

struct MusicItem { 
    std::string Name, Id, Album;
    int64_t DurationTicks;
};

// --- Globals ---
AppState current_state = STATE_LIBRARIES;
std::vector<MusicItem> current_list;
std::vector<int> playback_queue;
int queue_index = 0, scroll_index = 0;
bool is_shuffled = false, is_playing = false, is_n3ds = false;
LoopMode loop_mode = LOOP_OFF;

// Video/MVD Globals
bool mvd_enabled = false;
C3D_Tex video_tex;
C2D_Image video_image;
u32* video_linear_buf = NULL;
u16 video_pid = 0; 
u16 pmt_pid = 0; 

std::vector<std::string> debug_logs;
void log_msg(std::string m) {
    debug_logs.push_back(m);
    if(debug_logs.size() > 22) debug_logs.erase(debug_logs.begin());
}

// Audio State
u8* audio_ptr = NULL;
ndspWaveBuf waveBuf[NUM_BUFFERS];
volatile int write_node = 0; 
size_t buf_pos = 0;
volatile uint64_t total_samples_played = 0;
volatile bool thread_run = false, is_paused = false;

// Network & UI State
char current_song_name[128], current_album_name[128], current_song_id[64];
double current_duration_seconds = 0;
pthread_t play_thread;
char access_token[256] = {0}, server_url[256] = {0}; 
char* json_buffer = NULL;
size_t json_len = 0;
static u32* soc_buffer = NULL;

C3D_RenderTarget *top_target, *bottom_target;
C2D_TextBuf g_dynamicBuf;
C3D_Tex album_art_tex;
C2D_Image album_art_image;
bool has_album_art = false;
C2D_SpriteSheet sprite_sheet;

u32 clr_bg = C2D_Color32(0, 0, 0, 255);
u32 clr_accent = C2D_Color32(230, 190, 140, 255);
u32 clr_card = C2D_Color32(40, 40, 40, 255);
u32 clr_white = C2D_Color32(255, 255, 255, 255);
u32 clr_dim = C2D_Color32(100, 100, 100, 255);

// --- Video/Demux Logic ---

void init_video_decoder() {
    // RGB565 is the native output for MVD and most efficient for 3DS rendering
    if (R_SUCCEEDED(mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565, MVD_DEFAULT_WORKBUF_SIZE, NULL))) {
        mvd_enabled = true;
        log_msg("SYS: MVD Initialized.");
    }
    C3D_TexInit(&video_tex, 512, 256, GPU_RGB565);
    video_image.tex = &video_tex;
    // UV Mapping: 320/512 = 0.625, 240/256 = 0.9375
    static Tex3DS_SubTexture sub = { 512, 256, 0.0f, 0.9375f, 0.625f, 0.0f };
    video_image.subtex = &sub;
    video_linear_buf = (u32*)linearAlloc(320 * 240 * 2); 
}

void demux_ts_packet(u8* packet) {
    if (packet[0] != 0x47) return; 
    u16 pid = ((packet[1] & 0x1f) << 8) | packet[2];
    u8 adaptation_field = (packet[3] & 0x30) >> 4;
    u8 pointer = (adaptation_field >= 2) ? (packet[4] + 1) : 0;
    u8* payload = packet + 4 + pointer;
    size_t payload_size = TS_PACKET_SIZE - (4 + pointer);

    if (pid == 0 && pmt_pid == 0) {
        pmt_pid = ((payload[10] & 0x1f) << 8) | payload[11];
    } else if (pid == pmt_pid && video_pid == 0) {
        int section_len = ((payload[1] & 0x0F) << 8) | payload[2];
        u8* stream_data = payload + 12; 
        while (stream_data < payload + section_len) {
            if (stream_data[0] == 0x1B) { // H.264
                video_pid = ((stream_data[1] & 0x1F) << 8) | stream_data[2];
                log_msg("TS: Found Video PID " + std::to_string(video_pid));
                break;
            }
            stream_data += 5 + (((stream_data[3] & 0x0F) << 8) | stream_data[4]);
        }
    } else if (pid == video_pid && mvd_enabled && current_state == STATE_VIDEO) {
        if (video_linear_buf) {
            MVDSTD_ProcessNALUnitOut out_metadata; 
            memset(&out_metadata, 0, sizeof(out_metadata));
            mvdstdProcessVideoFrame(payload, payload_size, 0, &out_metadata);
            C3D_TexLoadImage(&video_tex, video_linear_buf, GPU_TEXFACE_2D, 0);
        }
    }
}

// --- JPEG & Graphics ---

uint32_t get_tiled_offset(uint32_t x, uint32_t y, uint32_t w) {
    return ((((y >> 3) * (w >> 3) + (x >> 3)) << 6) + ((x & 1) << 0) + ((y & 1) << 1) + ((x & 2) << 1) + ((y & 2) << 2) + ((x & 4) << 2) + ((y & 4) << 3));
}

void decode_jpeg(u8* src_data, size_t src_size) {
    if (src_size < 100) return;
    if (has_album_art) { C3D_TexDelete(&album_art_tex); has_album_art = false; }
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src_data, src_size);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&cinfo); return; }
    cinfo.out_color_space = JCS_RGB; 
    jpeg_start_decompress(&cinfo);  
    u32* flat_pixels = (u32*)linearAlloc(256 * 256 * 4);
    if (!flat_pixels) { jpeg_destroy_decompress(&cinfo); return; }
    memset(flat_pixels, 0, 256 * 256 * 4);
    u8* row_ptr = (u8*)malloc(cinfo.output_width * 3);
    while (cinfo.output_scanline < cinfo.output_height && cinfo.output_scanline < 256) {
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
        for(u32 x = 0; x < cinfo.output_width && x < 256; x++) {
            u8 r = row_ptr[x * 3], g = row_ptr[x * 3 + 1], b = row_ptr[x * 3 + 2];
            flat_pixels[(cinfo.output_scanline - 1) * 256 + x] = (0xFF) | (b << 8) | (g << 16) | (r << 24);
        }
    }
    free(row_ptr);
    u32* tex_data = (u32*)album_art_tex.data;
    C3D_TexInit(&album_art_tex, 256, 256, GPU_RGBA8);
    for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++) 
        ((u32*)album_art_tex.data)[get_tiled_offset(x, y, 256)] = flat_pixels[y * 256 + x];
    GSPGPU_FlushDataCache(album_art_tex.data, 256 * 256 * 4);
    jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo);
    linearFree(flat_pixels);
    album_art_image.tex = &album_art_tex;
    static Tex3DS_SubTexture sub = { 256, 256, 0.0f, 1.0f, 1.0f, 0.0f };
    album_art_image.subtex = &sub; has_album_art = true;
}

// --- Helpers ---

void load_config() {
    FILE* f = fopen(CONFIG_JSON, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return; }
    char* buf = (char*)malloc(size + 1);
    size_t read_bytes = fread(buf, 1, size, f);
    buf[read_bytes] = '\0'; fclose(f);
    struct json_object *root = json_tokener_parse(buf), *val;
    if (root) {
        if (json_object_object_get_ex(root, "server", &val)) strncpy(server_url, json_object_get_string(val), 255);
        if (json_object_object_get_ex(root, "token", &val)) strncpy(access_token, json_object_get_string(val), 255);
        json_object_put(root);
    }
    free(buf);
}

void fetch_items(const std::string& query_url) {
    json_len = 0; current_list.clear();
    CURL *curl = curl_easy_init(); struct curl_slist *h = NULL;
    char auth[512]; snprintf(auth, sizeof(auth), "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Token=\"%s\"", access_token);
    h = curl_slist_append(h, auth); curl_easy_setopt(curl, CURLOPT_URL, query_url.c_str()); curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t s, size_t n, void* u) -> size_t {
        size_t t = s * n; if (json_len + t < MAX_JSON_SIZE - 1) { memcpy(json_buffer + json_len, ptr, t); json_len += t; json_buffer[json_len] = '\0'; }
        return t;
    });
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    if(curl_easy_perform(curl) == CURLE_OK) {
        struct json_object *parsed = json_tokener_parse(json_buffer), *items;
        if (parsed && json_object_object_get_ex(parsed, "Items", &items)) {
            int items_found = json_object_array_length(items);
            for (int i = 0; i < items_found; i++) {
                struct json_object *it = json_object_array_get_idx(items, i), *n, *id, *alb, *dur;
                MusicItem mi;
                if (json_object_object_get_ex(it, "Name", &n)) mi.Name = json_object_get_string(n);
                if (json_object_object_get_ex(it, "Id", &id)) mi.Id = json_object_get_string(id);
                if (json_object_object_get_ex(it, "Album", &alb)) mi.Album = json_object_get_string(alb); else mi.Album = "Unknown Album";
                if (json_object_object_get_ex(it, "RunTimeTicks", &dur)) mi.DurationTicks = json_object_get_int64(dur);
                current_list.push_back(mi);
            }
            json_object_put(parsed); 
        }
    }
    curl_slist_free_all(h); curl_easy_cleanup(curl);
}

void fetch_libraries() {
    std::string url = std::string(server_url) + "/Library/SelectableMediaFolders?api_key=" + access_token;
    fetch_items(url);
    current_state = STATE_LIBRARIES;
    scroll_index = 0;
}

size_t audio_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!thread_run) return 0;
    while (is_paused && thread_run) svcSleepThread(10000000);
    size_t total = size * nmemb; u8* data = (u8*)ptr;
    if (current_state == STATE_VIDEO) {
        for (size_t i = 0; i < total; i += TS_PACKET_SIZE) if (i + TS_PACKET_SIZE <= total) demux_ts_packet(data + i);
    }
    for (size_t i = 0; i < total; i++) {
        while (thread_run && waveBuf[write_node].status != NDSP_WBUF_FREE && waveBuf[write_node].status != NDSP_WBUF_DONE) svcSleepThread(100000); 
        if (!thread_run) return 0;
        audio_ptr[write_node * AUDIO_BUF_SIZE + buf_pos] = data[i];
        buf_pos++;
        if (buf_pos >= AUDIO_BUF_SIZE) {
            DSP_FlushDataCache(audio_ptr + (write_node * AUDIO_BUF_SIZE), AUDIO_BUF_SIZE);
            waveBuf[write_node].nsamples = AUDIO_BUF_SIZE / 4; 
            ndspChnWaveBufAdd(0, &waveBuf[write_node]);
            total_samples_played += (AUDIO_BUF_SIZE / 4);
            write_node = (write_node + 1) % NUM_BUFFERS; buf_pos = 0;
        }
    }
    return total;
}

void* play_thread_func(void* arg) {
    std::string item_id = current_song_id;
    CURL *curl = curl_easy_init(); char stream[2048]; 
    snprintf(stream, sizeof(stream), "%s/Videos/%s/stream?static=false&VideoCodec=h264&VideoProfile=baseline&MaxWidth=320&AudioCodec=pcm_s16le&api_key=%s", server_url, item_id.c_str(), access_token);
    curl_easy_setopt(curl, CURLOPT_URL, stream); 
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audio_callback); 
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_perform(curl); curl_easy_cleanup(curl); thread_run = false; return NULL;
}

void play_song(const MusicItem& item) {
    if (thread_run) { thread_run = false; pthread_join(play_thread, NULL); }
    ndspChnReset(0); is_playing = true; current_state = STATE_PLAYER;
    strncpy(current_song_id, item.Id.c_str(), 63); strncpy(current_song_name, item.Name.c_str(), 127);
    current_duration_seconds = (double)item.DurationTicks / 10000000.0;
    total_samples_played = 0; write_node = 0; buf_pos = 0;
    video_pid = 0; pmt_pid = 0; thread_run = true;
    ndspChnInitParams(0); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0, HARDWARE_RATE);
    pthread_create(&play_thread, NULL, play_thread_func, NULL);
}

// --- Main ---

int main(int argc, char* argv[]) {
    gfxInitDefault(); 
    APT_CheckNew3DS(&is_n3ds); if (is_n3ds) osSetSpeedupEnable(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT); bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_dynamicBuf = C2D_TextBufNew(8192); ndspInit(); ptmuInit();
    audio_ptr = (u8*)linearAlloc(AUDIO_BUF_SIZE * NUM_BUFFERS); 
    json_buffer = (char*)malloc(MAX_JSON_SIZE); soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(soc_buffer) socInit(soc_buffer, SOC_BUFFERSIZE);
    
    init_video_decoder();
    sprite_sheet = C2D_SpriteSheetLoad(ICONS_PATH);
    load_config();
    fetch_libraries();

    while (aptMainLoop()) {
        hidScanInput(); 
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break;

        if (current_state == STATE_PLAYER) {
            if (kDown & KEY_B) current_state = STATE_SONGS;
            if (kDown & KEY_X && mvd_enabled) current_state = STATE_VIDEO;
            if (kDown & KEY_A) { is_paused = !is_paused; ndspChnSetPaused(0, is_paused); }
        } else if (current_state == STATE_VIDEO) {
            if (kDown & KEY_B) current_state = STATE_PLAYER;
        } else {
            // Scroll logic
            if (kDown & KEY_DDOWN) scroll_index++;
            if (kDown & KEY_DUP) scroll_index--;
            if (!current_list.empty()) {
                if (scroll_index < 0) scroll_index = 0;
                if (scroll_index >= (int)current_list.size()) scroll_index = (int)current_list.size() - 1;
            }

            // ACTION: Selection logic
            if (kDown & KEY_A && !current_list.empty()) {
                if (current_state == STATE_LIBRARIES) {
                    // Dive into the selected library folder
                    std::string url = std::string(server_url) + "/Items?ParentId=" + current_list[scroll_index].Id + "&Recursive=true&IncludeItemTypes=MusicAlbum,Video&api_key=" + access_token;
                    fetch_items(url);
                    current_state = STATE_ALBUMS;
                    scroll_index = 0;
                } else if (current_state == STATE_ALBUMS) {
                    if (current_list[scroll_index].DurationTicks > 0) play_song(current_list[scroll_index]);
                    else {
                        fetch_items(std::string(server_url) + "/Items?ParentId=" + current_list[scroll_index].Id + "&api_key=" + access_token);
                        current_state = STATE_SONGS;
                        scroll_index = 0;
                    }
                } else if (current_state == STATE_SONGS) {
                    play_song(current_list[scroll_index]);
                }
            }
            if (kDown & KEY_B) {
                if (current_state == STATE_SONGS) current_state = STATE_ALBUMS;
                else if (current_state == STATE_ALBUMS) fetch_libraries();
            }
        }

        // --- RENDER ---
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top_target, clr_bg);
        C2D_SceneBegin(top_target);

        if (current_state == STATE_PLAYER) {
            if (has_album_art) C2D_DrawImageAt(album_art_image, 15, 30, 0.5f, NULL, 0.65f, 0.65f);
        } else if (current_state == STATE_VIDEO) {
            C2D_DrawImageAt(video_image, 40, 0, 0.5f, NULL, 1.0f, 1.0f);
        }

        C2D_TargetClear(bottom_target, clr_bg);
        C2D_SceneBegin(bottom_target);

        // If list is empty, draw status
        if (current_list.empty()) {
            C2D_Text loadT; C2D_TextParse(&loadT, g_dynamicBuf, "Loading Libraries...");
            C2D_DrawText(&loadT, C2D_WithColor, 80, 110, 0.5f, 0.5f, 0.5f, clr_white);
        } else if (current_state != STATE_PLAYER && current_state != STATE_VIDEO) {
            // ACTUALLY DRAW THE LIST
            for (int i = 0; i < 10; i++) {
                int idx = scroll_index - 4 + i;
                if (idx >= 0 && idx < (int)current_list.size()) {
                    C2D_Text it; C2D_TextParse(&it, g_dynamicBuf, current_list[idx].Name.c_str());
                    if (idx == scroll_index) C2D_DrawRectSolid(0, 15 + (i * 22), 0.4f, 320, 20, clr_card);
                    C2D_DrawText(&it, C2D_WithColor, 15, 15 + (i * 22), 0.5f, 0.5f, 0.5f, (idx == scroll_index) ? clr_accent : clr_dim);
                }
            }
        }
        C3D_FrameEnd(0);
        C2D_TextBufClear(g_dynamicBuf);
    }
    if (mvd_enabled) mvdstdExit();
    ndspExit(); socExit(); gfxExit(); return 0;
}