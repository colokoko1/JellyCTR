#include <3ds.h>
#include <citro2d.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <sys/stat.h>

// --- Configuration ---
#define CONFIG_DIR      "sdmc:/3ds/JellyCTR"
#define CONFIG_PATH     "sdmc:/3ds/JellyCTR/config.txt"
#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000 
#define MAX_JSON_SIZE   (512 * 1024)
#define AUDIO_BUF_SIZE  (32 * 1024) 
#define NUM_BUFFERS     16
#define HARDWARE_RATE   48000 

// High-Readability OLED Theme
#define CLR_BLACK       C2D_Color32(0, 0, 0, 255)
#define CLR_WHITE       C2D_Color32(255, 255, 255, 255)
#define CLR_ACCENT      C2D_Color32(230, 190, 140, 255) // MRE Sand
#define CLR_DIM         C2D_Color32(160, 160, 160, 255)
#define CLR_PANEL       C2D_Color32(35, 35, 35, 255)

enum AppState { STATE_ALBUMS, STATE_SONGS, STATE_PLAYER };
struct MusicItem { 
    std::string Name; 
    std::string Id; 
    std::string Album;
    int64_t DurationTicks;
};

// --- Globals ---
AppState current_state = STATE_ALBUMS;
std::vector<MusicItem> current_list;
int scroll_index = 0;
u8* audio_ptr = NULL;
ndspWaveBuf waveBuf[NUM_BUFFERS];
volatile int write_node = 0; 
size_t buf_pos = 0;
volatile uint64_t total_samples_played = 0;
volatile bool thread_run = false;

char current_song_name[128] = "No Song Loaded";
char current_album_name[128] = "Unknown Album";
char current_song_id[64] = "";
double current_duration_seconds = 0;
pthread_t play_thread;

char access_token[256] = {0}, server_url[128] = {0}, user_id[64] = {0};
char* json_buffer = NULL;
size_t json_len = 0;
static u32* soc_buffer = NULL;

C3D_RenderTarget *top_target, *bottom_target;
C2D_TextBuf g_staticBuf, g_dynamicBuf;
int repeat_timer = 0;

// --- Helpers ---

void DrawRoundedRect(float x, float y, float w, float h, float r, u32 color) {
    C2D_DrawRectSolid(x + r, y, 0, w - (2 * r), h, color);
    C2D_DrawRectSolid(x, y + r, 0, r, h - (2 * r), color);
    C2D_DrawRectSolid(x + w - r, y + r, 0, r, h - (2 * r), color);
    C2D_DrawCircleSolid(x + r, y + r, 0, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, 0, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, 0, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0, r, color);
}

// --- Networking Callbacks ---

size_t json_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    if (json_len + total < MAX_JSON_SIZE - 1) {
        memcpy(json_buffer + json_len, ptr, total);
        json_len += total;
        json_buffer[json_len] = '\0';
    }
    return total;
}

size_t audio_stream_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!thread_run) return 0;
    size_t total = size * nmemb;
    u8* data = (u8*)ptr;
    for (size_t i = 0; i < total; i++) {
        while (thread_run && waveBuf[write_node].status != NDSP_WBUF_FREE && waveBuf[write_node].status != NDSP_WBUF_DONE) {
            svcSleepThread(500000); 
        }
        if (!thread_run) return 0;
        audio_ptr[write_node * AUDIO_BUF_SIZE + buf_pos] = data[i];
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
    return total;
}

// --- Auth & Input ---

void get_input(const char* hint, char* out, size_t max_len, bool password) {
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, hint);
    if (password) swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdInputText(&swkbd, out, max_len);
}

bool verify_token() {
    if (strlen(access_token) < 5) return false;
    json_len = 0;
    CURL *curl = curl_easy_init();
    if(!curl) return false;
    char verify_url[256];
    snprintf(verify_url, sizeof(verify_url), "%s/System/Info", server_url);
    struct curl_slist *headers = NULL;
    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Token=\"%s\", Device=\"3DS\", DeviceId=\"16038\"", access_token);
    headers = curl_slist_append(headers, auth_hdr);
    curl_easy_setopt(curl, CURLOPT_URL, verify_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, json_write_callback);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

bool perform_login() {
    if (strlen(server_url) < 5) get_input("Server URL", server_url, 128, false);
    char user[64] = {0}, pass[64] = {0};
    get_input("Username", user, 64, false);
    get_input("Password", pass, 64, true);
    json_len = 0;
    CURL *curl = curl_easy_init();
    if(!curl) return false;
    struct json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "Username", json_object_new_string(user));
    json_object_object_add(jobj, "Pw", json_object_new_string(pass));
    const char* post_data = json_object_to_json_string(jobj);
    char auth_url[256];
    snprintf(auth_url, sizeof(auth_url), "%s/Users/authenticatebyname", server_url);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Device=\"3DS\", DeviceId=\"16038\", Version=\"0.1\"");
    curl_easy_setopt(curl, CURLOPT_URL, auth_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, json_write_callback);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    CURLcode res = curl_easy_perform(curl);
    bool success = false;
    if(res == CURLE_OK) {
        struct json_object *parsed_json = json_tokener_parse(json_buffer);
        struct json_object *token_obj, *user_obj, *id_obj;
        if (parsed_json && json_object_object_get_ex(parsed_json, "AccessToken", &token_obj)) {
            strncpy(access_token, json_object_get_string(token_obj), 255);
            if (json_object_object_get_ex(parsed_json, "User", &user_obj) && json_object_object_get_ex(user_obj, "Id", &id_obj)) {
                strncpy(user_id, json_object_get_string(id_obj), 63);
            }
            mkdir(CONFIG_DIR, 0777);
            FILE* f = fopen(CONFIG_PATH, "w");
            if (f) { fprintf(f, "%s\n%s\n%s", server_url, access_token, user_id); fclose(f); }
            success = true;
        }
        if (parsed_json) json_object_put(parsed_json);
    }
    json_object_put(jobj);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}

// --- Audio Playback ---

void* play_thread_func(void* arg) {
    CURL *curl = curl_easy_init();
    if(curl) {
        char stream_url[2048];
        snprintf(stream_url, sizeof(stream_url), "%s/Audio/%s/stream?static=false&audioCodec=pcm_s16le&container=raw&audioSampleRate=48000&audioBitRate=1536000&audioChannels=2&api_key=%s", server_url, current_song_id, access_token);
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Device=\"3DS\", DeviceId=\"16038\"");
        curl_easy_setopt(curl, CURLOPT_URL, stream_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, audio_stream_callback);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    thread_run = false;
    return NULL;
}

void stop_playback() {
    if (thread_run) { thread_run = false; pthread_join(play_thread, NULL); }
    ndspChnReset(0);
}

void play_song(const MusicItem& item) {
    stop_playback();
    strncpy(current_song_id, item.Id.c_str(), 63);
    strncpy(current_song_name, item.Name.c_str(), 127);
    strncpy(current_album_name, item.Album.c_str(), 127);
    current_duration_seconds = (double)item.DurationTicks / 10000000.0;
    
    buf_pos = 0; write_node = 0; total_samples_played = 0;
    ndspChnReset(0);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(0, HARDWARE_RATE);
    float mix[12] = {1.0f, 1.0f}; ndspChnSetMix(0, mix);
    
    for(int i=0; i<NUM_BUFFERS; i++) waveBuf[i].status = NDSP_WBUF_FREE;
    thread_run = true;
    pthread_create(&play_thread, NULL, play_thread_func, NULL);
    current_state = STATE_PLAYER;
}

void fetch_items(const std::string& query_url) {
    json_len = 0; current_list.clear(); scroll_index = 0;
    CURL *curl = curl_easy_init();
    if(!curl) return;
    struct curl_slist *headers = NULL;
    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "X-Emby-Authorization: MediaBrowser Client=\"JellyCTR\", Token=\"%s\"", access_token);
    headers = curl_slist_append(headers, auth_hdr);
    curl_easy_setopt(curl, CURLOPT_URL, query_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, json_write_callback);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    if(curl_easy_perform(curl) == CURLE_OK) {
        struct json_object *parsed_json = json_tokener_parse(json_buffer);
        struct json_object *items_array;
        if (parsed_json && json_object_object_get_ex(parsed_json, "Items", &items_array)) {
            size_t array_len = (size_t)json_object_array_length(items_array);
            for (size_t i = 0; i < array_len; i++) {
                struct json_object *item = json_object_array_get_idx(items_array, i);
                struct json_object *n_obj, *id_obj, *alb_obj, *dur_obj;
                MusicItem mi;
                if (json_object_object_get_ex(item, "Name", &n_obj)) mi.Name = json_object_get_string(n_obj);
                if (json_object_object_get_ex(item, "Id", &id_obj)) mi.Id = json_object_get_string(id_obj);
                if (json_object_object_get_ex(item, "Album", &alb_obj)) mi.Album = json_object_get_string(alb_obj);
                else mi.Album = "Unknown Album";
                if (json_object_object_get_ex(item, "RunTimeTicks", &dur_obj)) mi.DurationTicks = json_object_get_int64(dur_obj);
                else mi.DurationTicks = 0;
                current_list.push_back(mi);
            }
            json_object_put(parsed_json);
        }
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
}

// --- Main ---

int main(int argc, char* argv[]) {
    gfxInitDefault(); C3D_Init(C3D_DEFAULT_CMDBUF_SIZE); C2D_Init(C2D_DEFAULT_MAX_OBJECTS); C2D_Prepare();
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    g_staticBuf = C2D_TextBufNew(1024); g_dynamicBuf = C2D_TextBufNew(4096);
    ndspInit();

    audio_ptr = (u8*)linearAlloc(AUDIO_BUF_SIZE * NUM_BUFFERS); 
    json_buffer = (char*)malloc(MAX_JSON_SIZE);
    memset(waveBuf, 0, sizeof(waveBuf));
    for(int i=0; i<NUM_BUFFERS; i++) {
        waveBuf[i].data_vaddr = audio_ptr + (i * AUDIO_BUF_SIZE);
        waveBuf[i].nsamples = AUDIO_BUF_SIZE / 4;
    }
    soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(soc_buffer) socInit(soc_buffer, SOC_BUFFERSIZE);

    FILE* cfg_f = fopen(CONFIG_PATH, "r");
    if (cfg_f) { if(fscanf(cfg_f, "%127s\n%255s\n%63s", server_url, access_token, user_id) != 3) { /* handle error */ } fclose(cfg_f); }

    hidScanInput();
    bool auth_ok = false;
    if (strlen(access_token) > 5 && !(hidKeysHeld() & KEY_Y)) if (verify_token()) auth_ok = true;
    if (!auth_ok && !perform_login()) return 0;

    fetch_items(std::string(server_url) + "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName");

    while (aptMainLoop()) {
        hidScanInput(); u32 kDown = hidKeysDown(); u32 kHeld = hidKeysHeld();
        if (kDown & KEY_START) break;

        if (current_state != STATE_PLAYER) {
            bool move_up = (kDown & KEY_DUP); 
            bool move_down = (kDown & KEY_DDOWN);

            if (kHeld & (KEY_DUP | KEY_DDOWN)) {
                repeat_timer++;
                if (repeat_timer >= 30 && (repeat_timer % 5 == 0)) {
                    if (kHeld & KEY_DUP) move_up = true;
                    if (kHeld & KEY_DDOWN) move_down = true;
                }
            } else repeat_timer = 0;

            if (move_up) scroll_index--; 
            if (move_down) scroll_index++;

            if (!current_list.empty()) {
                if (scroll_index < 0) scroll_index = 0;
                if (scroll_index >= (int)current_list.size()) scroll_index = (int)current_list.size() - 1;
            }
            if (kDown & KEY_A && !current_list.empty()) {
                if (current_state == STATE_ALBUMS) {
                    fetch_items(std::string(server_url) + "/Items?ParentId=" + current_list[scroll_index].Id + "&SortBy=IndexNumber");
                    current_state = STATE_SONGS;
                } else play_song(current_list[scroll_index]);
            }
        }
        if (kDown & KEY_B) {
            if (current_state == STATE_PLAYER) { stop_playback(); current_state = STATE_SONGS; }
            else if (current_state == STATE_SONGS) {
                fetch_items(std::string(server_url) + "/Items?IncludeItemTypes=MusicAlbum&Recursive=true&SortBy=SortName");
                current_state = STATE_ALBUMS;
            }
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top_target, CLR_BLACK);
        C2D_SceneBegin(top_target);

        DrawRoundedRect(20, 45, 170, 170, 12, CLR_PANEL);
        C2D_TextBufClear(g_dynamicBuf);
        C2D_Text title, sub, timeTxt;
        C2D_TextParse(&title, g_dynamicBuf, current_song_name);

        char from_str[160]; // Increased to avoid truncation warnings
        snprintf(from_str, sizeof(from_str), "from %s", current_album_name);
        C2D_TextParse(&sub, g_dynamicBuf, from_str);

        double elapsed = (double)total_samples_played / (double)HARDWARE_RATE;
        int e_min = (int)elapsed / 60; int e_sec = (int)elapsed % 60;
        int d_min = (int)current_duration_seconds / 60; int d_sec = (int)current_duration_seconds % 60;
        char time_str[64]; snprintf(time_str, sizeof(time_str), "%d:%02d / %d:%02d", e_min, e_sec, d_min, d_sec);
        C2D_TextParse(&timeTxt, g_dynamicBuf, time_str);

        C2D_DrawText(&title, C2D_WithColor, 210, 80, 0, 0.7f, 0.7f, CLR_WHITE);
        C2D_DrawText(&sub, C2D_WithColor, 210, 105, 0, 0.45f, 0.45f, CLR_DIM);
        C2D_DrawText(&timeTxt, C2D_WithColor, 210, 170, 0, 0.5f, 0.5f, CLR_WHITE);

        float progress = (current_duration_seconds > 0) ? (float)(elapsed / current_duration_seconds) : 0;
        if (progress > 1.0f) progress = 1.0f;
        DrawRoundedRect(210, 190, 170, 10, 5, C2D_Color32(60, 60, 60, 255));
        DrawRoundedRect(210, 190, 170 * progress, 10, 5, CLR_ACCENT);
        
        C2D_DrawRectSolid(0, 0, 0, 400, 25, C2D_Color32(50, 50, 50, 255));

        C2D_TargetClear(bottom_target, CLR_BLACK);
        C2D_SceneBegin(bottom_target);
        if (current_state != STATE_PLAYER) {
            for (int i = 0; i < 10; i++) {
                int idx = scroll_index - 4 + i;
                if (idx >= 0 && idx < (int)current_list.size()) {
                    C2D_Text item; C2D_TextParse(&item, g_dynamicBuf, current_list[idx].Name.c_str());
                    u32 col = (idx == scroll_index) ? CLR_ACCENT : CLR_DIM;
                    if (idx == scroll_index) C2D_DrawRectSolid(0, 15 + (i * 22), 0, 320, 20, C2D_Color32(40, 40, 40, 255));
                    C2D_DrawText(&item, C2D_WithColor, 15, 15 + (i * 22), 0, 0.5f, 0.5f, col);
                }
            }
        }
        C3D_FrameEnd(0);
    }
    stop_playback(); ndspExit(); linearFree(audio_ptr); free(json_buffer); free(soc_buffer); socExit(); gfxExit(); return 0;
}
