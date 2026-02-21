// JellyCTR v0.2 - Full Release Candidate 4
// hardware is a nightmare but the nightmare goes on anyway
// TODO: Split into smaller files
//            |_ jpeg.cpp
//            |_ network.cpp
//            |_ system.cpp
//            |_ audio.cpp
//            |_ video.cpp
//            |_ ui.cpp
//       Switch globals to compile-time
//       Add options menu
//       Clean up code
//       Optimize current code
//       Add theming options in config file
//       Add debug info
//          |_ Current ram usage
//          |_ CPU core utilization
//       Switch to JSON for config.
//  ^^^ Release 0.3 ^^^
//       Implement video support
//          |_ Add video menu
//          |_ Fetch contents
//          |_ Get media info
//          |_ Set 3ds speed for N3DS family
//          |_ Add audio decoding
//          |_ Add network indicator
//          |_ Add HW decode
//          |_ Create device profile
//          |_ Add software decode
//          |_ Add software decode profile
//          |_ Add 3d support
//          |_ Add 3d profile
//          |_ Work on remote streaming stability, buffer at least 15 seconds worth of content
//       Improve UX
//          |_ Add search funcitonality
//          |_ Add debug menu
//          |_ Optimize everything
//          |_ Redo main UI
//  ^^^ Release 0.4 ^^^
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


#include <gfx/icons.h> 

// --- Configs and other junk ---
#define CONFIG_DIR      "sdmc:/3ds/JellyCTR"
#define CONFIG_PATH     "sdmc:/3ds/JellyCTR/config.txt"
#define ICONS_PATH      "sdmc:/3ds/JellyCTR/icons.t3x"
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000 
#define MAX_JSON_SIZE   (512 * 1024)
#define AUDIO_BUF_SIZE  (32 * 1024) 
#define NUM_BUFFERS     16
#define HARDWARE_RATE   48000 

// Theme hardcoded for now
#define CLR_BLACK       C2D_Color32(0, 0, 0, 255)
#define CLR_WHITE       C2D_Color32(255, 255, 255, 255)
#define CLR_ACCENT      C2D_Color32(230, 190, 140, 255) 
#define CLR_DIM         C2D_Color32(100, 100, 100, 255)
#define CLR_CARD        C2D_Color32(40, 40, 40, 255)

enum LoopMode { LOOP_OFF, LOOP_ALL, LOOP_ONE };
enum AppState { STATE_ALBUMS, STATE_SONGS, STATE_PLAYER };

struct MusicItem { 
    std::string Name, Id, Album;
    int64_t DurationTicks;
};

// --- Globals (yeah yeah, I know, globals suck) ---
AppState current_state = STATE_ALBUMS;
std::vector<MusicItem> current_list;
std::vector<int> playback_queue;
int queue_index = 0, scroll_index = 0, repeat_timer = 0, y_hold_timer = 0;
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
pthread_t play_thread;
char access_token[256], server_url[256]; 
char* json_buffer = NULL;
size_t json_len = 0;
static u32* soc_buffer = NULL;

C3D_RenderTarget *top_target, *bottom_target;
C2D_TextBuf g_dynamicBuf;
C3D_Tex album_art_tex;
C2D_Image album_art_image;
bool has_album_art = false;
C2D_SpriteSheet sprite_sheet;

// --- Logic Helpers ---

void build_queue(int start_index) {
    playback_queue.clear();
    for (int i = 0; i < (int)current_list.size(); i++) playback_queue.push_back(i);
    if (is_shuffled) {
        std::random_device rd; std::mt19937 g(rd());
        std::shuffle(playback_queue.begin(), playback_queue.end(), g);
        for (int i = 0; i < (int)playback_queue.size(); i++) {
            if (playback_queue[i] == start_index) { std::swap(playback_queue[0], playback_queue[i]); break; }
        }
    } else {
        std::rotate(playback_queue.begin(), playback_queue.begin() + start_index, playback_queue.end());
    }
    queue_index = 0;
}

void ask_for_input(char* out, size_t buf_size, const char* hint, SwkbdType type, bool password) {
    SwkbdState swkbd;
    swkbdInit(&swkbd, type, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    if (password) swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdInputText(&swkbd, out, buf_size); 
}

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
    C3D_TexInit(&album_art_tex, 256, 256, GPU_RGBA8);
    u32* flat_pixels = (u32*)linearAlloc(256 * 256 * 4);
    if (!flat_pixels) return;
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
    for (u32 y = 0; y < 256; y++) for (u32 x = 0; x < 256; x++) tex_data[get_tiled_offset(x, y, 256)] = flat_pixels[y * 256 + x];
    GSPGPU_FlushDataCache(album_art_tex.data, 256 * 256 * 4);

    if (cinfo.output_scanline < cinfo.output_height) jpeg_abort_decompress(&cinfo);
    else jpeg_finish_decompress(&cinfo);
    
    jpeg_destroy_decompress(&cinfo);
    linearFree(flat_pixels);
    album_art_image.tex = &album_art_tex;
    static Tex3DS_SubTexture sub = { 256, 256, 0.0f, 1.0f, 1.0f, 0.0f };
    album_art_image.subtex = &sub;
    has_album_art = true;
}

size_t audio_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!thread_run) return 0;
    while (is_paused && thread_run) svcSleepThread(10000000);
    size_t total = size * nmemb; u8* data = (u8*)ptr;
    for (size_t i = 0; i < total; i++) {
        while (thread_run && waveBuf[write_node].status != NDSP_WBUF_FREE && waveBuf[write_node].status != NDSP_WBUF_DONE) svcSleepThread(500000); 
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

void stop_playback() {
    if (thread_run) { thread_run = false; is_paused = false; pthread_join(play_thread, NULL); }
    ndspChnReset(0);
}

void play_song(const MusicItem& item) {
    stop_playback();
    is_playing = true; 
    current_state = STATE_PLAYER;
    struct DLBuf { u8* b; size_t s; } d; d.b = (u8*)malloc(512 * 1024); d.s = 0;
    CURL* c = curl_easy_init();
    char url[1024]; snprintf(url, sizeof(url), "%s/Items/%s/Images/Primary?maxWidth=256&api_key=%s", server_url, item.Id.c_str(), access_token);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](void* p, size_t s, size_t n, void* u) -> size_t {
        struct DLBuf* db = (struct DLBuf*)u;
        memcpy(db->b + db->s, p, s*n); db->s += s*n; return s*n;
    });
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    if (curl_easy_perform(c) == CURLE_OK) decode_jpeg(d.b, d.s);
    curl_easy_cleanup(c); free(d.b);
    strncpy(current_song_id, item.Id.c_str(), 63); strncpy(current_song_name, item.Name.c_str(), 127); strncpy(current_album_name, item.Album.c_str(), 127);
    current_duration_seconds = (double)item.DurationTicks / 10000000.0;
    total_samples_played = 0; write_node = 0; buf_pos = 0;
    ndspChnReset(0); ndspChnInitParams(0); ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16); ndspChnSetRate(0, HARDWARE_RATE);
    for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].status = NDSP_WBUF_FREE;
    thread_run = true;
    pthread_create(&play_thread, NULL, [](void* arg) -> void* {
        CURL *curl = curl_easy_init(); char stream[2048]; 
        snprintf(stream, sizeof(stream), "%s/Audio/%s/stream?static=false&audioCodec=pcm_s16le&container=raw&audioSampleRate=48000&maxSampleRate=48000&targetSampleRate=48000&audioBitRate=1536000&api_key=%s", server_url, current_song_id, access_token); // You need to specify both a target and fallback sample rate to avoid Jellyfin crashing FFMPEG
        curl_easy_setopt(curl, CURLOPT_URL, stream); curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audio_callback); curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_perform(curl); curl_easy_cleanup(curl); thread_run = false; return NULL;
    }, NULL);
}

void play_current_queue_item() {
    if (playback_queue.empty()) return;
    play_song(current_list[playback_queue[queue_index]]);
}

void next_track() {
    if (loop_mode == LOOP_ONE) { /* we love our loops */ }
    else if (queue_index < (int)playback_queue.size() - 1) queue_index++;
    else if (loop_mode == LOOP_ALL) queue_index = 0;
    else { 
        stop_playback(); 
        is_playing = false; 
        return; 
    }
    play_current_queue_item();
}

void prev_track() {
    if (total_samples_played / 48000.0 > 3.0) play_current_queue_item();
    else if (queue_index > 0) { queue_index--; play_current_queue_item(); }
}

bool perform_login() {
    char username[128] = {0}, password[128] = {0};
    ask_for_input(server_url, 256, "Server URL (http://ip:8096)", SWKBD_TYPE_NORMAL, false);
    ask_for_input(username, 128, "Username", SWKBD_TYPE_NORMAL, false);
    ask_for_input(password, 128, "Password", SWKBD_TYPE_NORMAL, true);
    CURL *curl = curl_easy_init();
    char login_url[512]; snprintf(login_url, sizeof(login_url), "%s/Users/AuthenticateByName", server_url);
    struct json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "Username", json_object_new_string(username));
    json_object_object_add(jobj, "Pw", json_object_new_string(password));
    const char* post_data = json_object_to_json_string(jobj);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Device=\"3DS\", Version=\"0.2\"");
    json_len = 0; curl_easy_setopt(curl, CURLOPT_URL, login_url); curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t s, size_t n, void* u) -> size_t {
        size_t t = s * n; if (json_len + t < MAX_JSON_SIZE - 1) { memcpy(json_buffer + json_len, ptr, t); json_len += t; json_buffer[json_len] = '\0'; }
        return t;
    });
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    bool success = false;
    if(curl_easy_perform(curl) == CURLE_OK) {
        struct json_object *parsed = json_tokener_parse(json_buffer), *token_obj;
        if (parsed && json_object_object_get_ex(parsed, "AccessToken", &token_obj)) {
            strncpy(access_token, json_object_get_string(token_obj), 255); success = true;
        }
        if (parsed) json_object_put(parsed);
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl); json_object_put(jobj);
    return success;
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
        if (parsed) { 
            if (json_object_object_get_ex(parsed, "Items", &items)) {
                for (size_t i = 0; i < (size_t)json_object_array_length(items); i++) {
                    struct json_object *it = json_object_array_get_idx(items, i), *n, *id, *alb, *dur;
                    MusicItem mi;
                    if (json_object_object_get_ex(it, "Name", &n)) mi.Name = json_object_get_string(n);
                    if (json_object_object_get_ex(it, "Id", &id)) mi.Id = json_object_get_string(id);
                    if (json_object_object_get_ex(it, "Album", &alb)) mi.Album = json_object_get_string(alb); else mi.Album = "Unknown Album";
                    if (json_object_object_get_ex(it, "RunTimeTicks", &dur)) mi.DurationTicks = json_object_get_int64(dur);
                    current_list.push_back(mi);
                }
            }
            json_object_put(parsed); 
        }
    }
    curl_slist_free_all(h); curl_easy_cleanup(curl);
}

void draw_status_bar() {
    int wifi = (int)osGetWifiStrength();
    if (wifi > 0 && sprite_sheet) {
        C2D_Image w = C2D_SpriteSheetGetImage(sprite_sheet, icons_wifi_min_idx + (wifi - 1));
        C2D_DrawImageAt(w, 320, 5, 0.5f, NULL, 0.5f, 0.5f);
    }
    u8 bat; PTMU_GetBatteryLevel(&bat); 
    int bat_idx = (bat > 6) ? 6 : bat; 
    if (sprite_sheet) {
        C2D_Image b = C2D_SpriteSheetGetImage(sprite_sheet, icons_batt_1_idx + bat_idx);
        C2D_DrawImageAt(b, 360, 5, 0.5f, NULL, 0.5f, 0.5f);
    }
}

void draw_next_up_preview(int x, int y, int q_idx) {
    if (q_idx >= playback_queue.size()) return;
    int actual_idx = playback_queue[q_idx];
    C2D_DrawRectSolid(x, y, 0.4f, 130, 60, CLR_CARD);
    if (has_album_art) C2D_DrawImageAt(album_art_image, x + 5, y + 5, 0.5f, NULL, 0.2f, 0.2f);
    C2D_Text nextTxt; C2D_TextParse(&nextTxt, g_dynamicBuf, current_list[actual_idx].Name.c_str());
    C2D_DrawText(&nextTxt, C2D_WithColor, x + 50, y + 15, 0.5f, 0.4f, 0.4f, CLR_WHITE);
}

int main(int argc, char* argv[]) {
    gfxInitDefault(); C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT); bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_dynamicBuf = C2D_TextBufNew(4096); ndspInit(); ptmuInit();
    audio_ptr = (u8*)linearAlloc(AUDIO_BUF_SIZE * NUM_BUFFERS); 
    json_buffer = (char*)malloc(MAX_JSON_SIZE); soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(soc_buffer) socInit(soc_buffer, SOC_BUFFERSIZE);
    for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].data_vaddr = audio_ptr + (i * AUDIO_BUF_SIZE);

    sprite_sheet = C2D_SpriteSheetLoad(ICONS_PATH);

    mkdir(CONFIG_DIR, 0777);
    FILE* cfg = fopen(CONFIG_PATH, "r");
    if (!cfg) { if (perform_login()) { FILE* fw = fopen(CONFIG_PATH, "w"); fprintf(fw, "%s\n%s", server_url, access_token); fclose(fw); } }
    else { fscanf(cfg, "%255s\n%255s", server_url, access_token); fclose(cfg); }

    fetch_items(std::string(server_url) + "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName");

    while (aptMainLoop()) {
        hidScanInput(); u32 kDown = hidKeysDown(), kHeld = hidKeysHeld();
        touchPosition touch; hidTouchRead(&touch);
        if (kDown & KEY_START) break;
        if (current_state == STATE_PLAYER && is_playing && !thread_run && !is_paused && has_album_art) next_track();
        if (kDown & KEY_B) { // Should work now
            if (current_state == STATE_PLAYER) { stop_playback(); is_playing = false; current_state = STATE_SONGS; }
            else if (current_state == STATE_SONGS) { fetch_items(std::string(server_url) + "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName"); current_state = STATE_ALBUMS; scroll_index = 0; }
        }

        if (kHeld & KEY_Y) {
            y_hold_timer++;
            if (y_hold_timer >= 600) {
                y_hold_timer = 0; stop_playback();
                if (perform_login()) { FILE* fw = fopen(CONFIG_PATH, "w"); fprintf(fw, "%s\n%s", server_url, access_token); fclose(fw); fetch_items(std::string(server_url) + "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName"); current_state = STATE_ALBUMS; scroll_index = 0; }
            }
        } else y_hold_timer = 0;

        if (current_state == STATE_PLAYER) {
            if (kDown & KEY_TOUCH) {
                if (touch.px < 50 && touch.py < 50) { stop_playback(); is_playing = false; current_state = STATE_SONGS; }
                if (touch.px > 260 && touch.py < 50) loop_mode = (LoopMode)((loop_mode + 1) % 3);
                
                if (touch.px > 120 && touch.px < 200 && touch.py > 100 && touch.py < 180) { is_paused = !is_paused; ndspChnSetPaused(0, is_paused); }
                
                if (touch.py > 110 && touch.py < 170) { 
                    if (touch.px > 210) next_track();
                    if (touch.px < 110) prev_track();
                }
            }
        } else {
            bool up = (kDown & KEY_DUP), down = (kDown & KEY_DDOWN);
            if (kHeld & (KEY_DUP | KEY_DDOWN)) { repeat_timer++; if (repeat_timer >= 30 && (repeat_timer % 5 == 0)) { if (kHeld & KEY_DUP) up = true; if (kHeld & KEY_DDOWN) down = true; } } else repeat_timer = 0;
            if (up) scroll_index--; if (down) scroll_index++;
            if (!current_list.empty()) { if (scroll_index < 0) scroll_index = 0; if (scroll_index >= (int)current_list.size()) scroll_index = (int)current_list.size() - 1; }
            if (kDown & KEY_A && !current_list.empty()) {
                if (current_state == STATE_ALBUMS) { fetch_items(std::string(server_url) + "/Items?ParentId=" + current_list[scroll_index].Id); current_state = STATE_SONGS; }
                else { build_queue(scroll_index); play_current_queue_item(); }
            }
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(g_dynamicBuf);
        C2D_TargetClear(top_target, CLR_BLACK);
        C2D_SceneBegin(top_target);
        draw_status_bar();
        if (has_album_art) C2D_DrawImageAt(album_art_image, 15, 30, 0.5f, NULL, 0.65f, 0.65f);
        C2D_Text t, s, tm; C2D_TextParse(&t, g_dynamicBuf, current_song_name); C2D_TextParse(&s, g_dynamicBuf, current_album_name);
        double el = (double)total_samples_played / 48000.0; char tt_s[64]; snprintf(tt_s, sizeof(tt_s), "%d:%02d/%d:%02d", (int)el/60, (int)el%60, (int)current_duration_seconds/60, (int)current_duration_seconds%60);
        C2D_TextParse(&tm, g_dynamicBuf, tt_s);
        C2D_DrawText(&t, C2D_WithColor, 200, 40, 0.5f, 0.65f, 0.65f, CLR_WHITE); C2D_DrawText(&s, C2D_WithColor, 200, 65, 0.5f, 0.38f, 0.38f, CLR_DIM);
        C2D_DrawText(&tm, C2D_WithColor, 200, 120, 0.5f, 0.45f, 0.45f, CLR_WHITE);
        float pr = (current_duration_seconds > 0) ? (float)(el / current_duration_seconds) : 0;
        C2D_DrawRectSolid(200, 145, 0.5f, 180, 12, CLR_CARD); C2D_DrawRectSolid(200, 145, 0.5f, 180 * (pr > 1.0f ? 1.0f : pr), 12, CLR_ACCENT);

        C2D_TargetClear(bottom_target, CLR_BLACK);
        C2D_SceneBegin(bottom_target);
        if (current_state == STATE_PLAYER && sprite_sheet) {
            // New design layout
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprite_sheet, icons_back_chevron_idx), 20, 20, 0.5f, NULL, 0.8f, 0.8f);
            int li = (loop_mode == LOOP_ONE) ? icons_loop_one_idx : (loop_mode == LOOP_ALL ? icons_loop_all_idx : icons_loop_off_idx);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprite_sheet, li), 270, 20, 0.5f, NULL, 0.8f, 0.8f);
            
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprite_sheet, is_paused ? icons_paused_idx : icons_playing_idx), 160-35, 140-35, 0.5f, NULL, 0.5f, 0.5f);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprite_sheet, icons_prev_sng_idx), 80, 140-25, 0.5f, NULL, 0.5f, 0.5f);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprite_sheet, icons_next_sng_idx), 220, 140-25, 0.5f, NULL, 0.5f, 0.5f);

            C2D_Text nxtHead; C2D_TextParse(&nxtHead, g_dynamicBuf, "Next Up:");
            C2D_DrawText(&nxtHead, C2D_WithColor, 20, 180, 0.5f, 0.5f, 0.5f, CLR_WHITE);
            draw_next_up_preview(40, 200, queue_index + 1);
            draw_next_up_preview(180, 200, queue_index + 2);

        } else if (current_state != STATE_PLAYER) {
            for (int i = 0; i < 10; i++) {
                int idx = scroll_index - 4 + i;
                if (idx >= 0 && idx < (int)current_list.size()) {
                    C2D_Text it; C2D_TextParse(&it, g_dynamicBuf, current_list[idx].Name.c_str());
                    if (idx == scroll_index) C2D_DrawRectSolid(0, 15 + (i * 22), 0.4f, 320, 20, CLR_CARD);
                    C2D_DrawText(&it, C2D_WithColor, 15, 15 + (i * 22), 0.5f, 0.5f, 0.5f, (idx == scroll_index) ? CLR_ACCENT : CLR_DIM);
                }
            }
        }
        C3D_FrameEnd(0);
    }
    ptmuExit(); ndspExit(); linearFree(audio_ptr); free(json_buffer); free(soc_buffer); socExit(); gfxExit(); return 0;
}
