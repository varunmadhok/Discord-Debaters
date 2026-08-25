// bot_deferred_logging.c - Updated REST Polling Discord Bot with MicroVM Local Command Interception
//
// Build command:
// gcc -o bot_deferred_logging bot_deferred_logging.c -lcurl -lcjson -lpthread

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <curl/curl.h>
#include "cjson/cJSON.h"

#define DISCORD_API_BASE     "https://discord.com/api/v10"
#define GROQ_URL             "https://api.groq.com/openai/v1/chat/completions"
#define GROQ_MODEL           "openai/gpt-oss-120b"
#define GROQ_API_KEY         "1. INSERT GROQ API KEY HERE"

// Must be set to your bot's token and channel ID
#define DISCORD_BOT_TOKEN    "2. INSERT ALPHA BOT TOKEN HERE"
#define DISCORD_CHANNEL_ID   "3. INSERT YOUR DISCORD CHANNEL ID HERE"
// 4. (Optional) There is another VM_IDENTITY variable in the code that you may want to update

#define USER_AGENT           "BareMetal-Discord-Bot (1.0)"
#define POLL_INTERVAL_SECS   3
#define MAX_MSG_OBJS         10
#define MAX_CONSECUTIVE_FAIL 5

typedef struct {
    char *memory;
    size_t size;
} MemoryBuffer;

typedef struct {
    char *channel_id;
    char *message_id;
    char *user_prompt;
    char *groq_api_key;
    char *bot_token;
    long request_id;
} AsyncJobContext;

// In-memory global store (resides purely in guest RAM, no disk I/O)
static char g_heap_storage[512] = "Empty (No data stored in heap yet)";

// Helper to fetch guest system uptime from read-only /proc/uptime
static void get_guest_uptime(char *buf, size_t max_len) {
    FILE *f = fopen("/proc/uptime", "r");
    if (!f) {
        snprintf(buf, max_len, "Uptime: Unavailable");
        return;
    }
    double uptime_secs;
    if (fscanf(f, "%lf", &uptime_secs) == 1) {
        snprintf(buf, max_len, "Guest Uptime: %.2f seconds", uptime_secs);
    } else {
        snprintf(buf, max_len, "Uptime: Parse Error");
    }
    fclose(f);
}

static long g_request_counter = 0;

// Helper to log with microsecond timestamps
static void log_timestamp(const char *level, long req_id, const char *fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    char time_buf[64];
    struct tm *tm_info = localtime(&tv.tv_sec);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

    printf("[%s.%03d] [%s] [Req #%ld] ", time_buf, (int)(tv.tv_usec / 1000), level, req_id);

    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

// MicroVM Diagnostic & File System Helper Functions
static void get_guest_ip(char *buf, size_t max_len) {
    char hostname[128];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct hostent *he = gethostbyname(hostname);
        if (he && he->h_addr_list[0]) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[0], sizeof(struct in_addr));
            snprintf(buf, max_len, "Host: %s | IP: %s", hostname, inet_ntoa(addr));
            return;
        }
    }
    snprintf(buf, max_len, "IP: 127.0.0.1 (Loopback)");
}

static int write_guest_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static void read_guest_file(const char *path, char *buf, size_t max_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, max_len, "Error: File '%s' does not exist in this microVM sandbox.", path);
        return;
    }
    size_t n = fread(buf, 1, max_len - 1, f);
    buf[n] = '\0';
    fclose(f);
}

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    char *ptr = realloc(mem->memory, mem->size + total_size + 1);
    if (!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, total_size);
    mem->size += total_size;
    mem->memory[mem->size] = 0;

    return total_size;
}

static void curl_common_opts(CURL *h, int force_fresh) {
    curl_easy_setopt(h, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    if (force_fresh)
        curl_easy_setopt(h, CURLOPT_FRESH_CONNECT, 1L);
}

static long http_get(CURL *h, const char *url, const char *auth_header, MemoryBuffer *mb, int force_fresh) {
    mb->size = 0;
    if (mb->memory) mb->memory[0] = '\0';

    curl_easy_reset(h);
    curl_common_opts(h, force_fresh);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, (void *)mb);

    struct curl_slist *headers = curl_slist_append(NULL, auth_header);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        log_timestamp("ERROR", 0, "GET %s failed: %s", url, curl_easy_strerror(res));
        return -1;
    }
    return status;
}

static long http_post(CURL *h, const char *url, const char *auth_header, const char *payload, MemoryBuffer *mb, int force_fresh) {
    mb->size = 0;
    if (mb->memory) mb->memory[0] = '\0';

    curl_easy_reset(h);
    curl_common_opts(h, force_fresh);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, (void *)mb);

    struct curl_slist *headers = curl_slist_append(NULL, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        log_timestamp("ERROR", 0, "POST %s failed: %s", url, curl_easy_strerror(res));
        return -1;
    }
    return status;
}

static char *call_groq_api(long req_id, const char *user_prompt, const char *groq_api_key) {
    log_timestamp("DEBUG", req_id, "Dispatching prompt to Groq API (%s)...", GROQ_MODEL);
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    MemoryBuffer chunk = { .memory = malloc(1), .size = 0 };

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", GROQ_MODEL);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "You are an AI microVM agent running on bare-metal assembly infrastructure.");
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *usr_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(usr_msg, "role", "user");
    cJSON_AddStringToObject(usr_msg, "content", user_prompt);
    cJSON_AddItemToArray(messages, usr_msg);

    char *json_payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    struct curl_slist *headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", groq_api_key);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_URL, GROQ_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    
    free(json_payload);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log_timestamp("ERROR", req_id, "Groq libcurl request failed: %s", curl_easy_strerror(res));
        free(chunk.memory);
        return NULL;
    }

    log_timestamp("DEBUG", req_id, "Groq HTTP request returned successfully.");

    char *completion_text = NULL;
    cJSON *resp_json = cJSON_Parse(chunk.memory);
    if (resp_json) {
        cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
            cJSON *message = cJSON_GetObjectItem(first_choice, "message");
            cJSON *content = cJSON_GetObjectItem(message, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                completion_text = strdup(content->valuestring);
            }
        }
        cJSON_Delete(resp_json);
    } else {
        log_timestamp("ERROR", req_id, "Failed to parse JSON response from Groq.");
    }

    free(chunk.memory);
    return completion_text;
}

static void send_discord_reply(long req_id, const char *channel_id, const char *message_id, const char *token, const char *content) {
    log_timestamp("DEBUG", req_id, "Sending POST reply to Discord REST channel endpoint...");
    CURL *curl = curl_easy_init();
    if (!curl) return;

    char safe_content[2048];
    if (strlen(content) > 1990) {
        log_timestamp("WARN", req_id, "Response exceeded 2000 chars (%zu chars). Truncating payload.", strlen(content));
        snprintf(safe_content, sizeof(safe_content), "%.1900s\n\n*[Output truncated due to Discord 2,000 character limit]*", content);
    } else {
        snprintf(safe_content, sizeof(safe_content), "%s", content);
    }

    char post_url[256];
    snprintf(post_url, sizeof(post_url), "%s/channels/%s/messages", DISCORD_API_BASE, channel_id);

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", token);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "content", safe_content);
    
    cJSON *msg_ref = cJSON_CreateObject();
    cJSON_AddStringToObject(msg_ref, "message_id", message_id);
    cJSON_AddItemToObject(root, "message_reference", msg_ref);

    char *json_payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    MemoryBuffer post_mb = { .memory = malloc(1), .size = 0 };
    long status = http_post(curl, post_url, auth_header, json_payload, &post_mb, 0);

    if (status >= 200 && status < 300) {
        log_timestamp("INFO", req_id, "Discord reply posted successfully (HTTP %ld).", status);
    } else {
        log_timestamp("ERROR", req_id, "Discord reply failed (HTTP %ld): %s", status, post_mb.memory ? post_mb.memory : "");
    }

    free(json_payload);
    free(post_mb.memory);
    curl_easy_cleanup(curl);
}

static void *async_groq_worker(void *arg) {
    AsyncJobContext *job = (AsyncJobContext *)arg;
    long req_id = job->request_id;

    log_timestamp("INFO", req_id, "Worker thread started for prompt: \"%s\"", job->user_prompt);

    char *ai_reply = call_groq_api(req_id, job->user_prompt, job->groq_api_key);
    if (!ai_reply) {
        ai_reply = strdup("Error: Failed to fetch completion from Groq LLM endpoint.");
    }

    send_discord_reply(req_id, job->channel_id, job->message_id, job->bot_token, ai_reply);

    free(job->channel_id);
    free(job->message_id);
    free(job->user_prompt);
    free(job->groq_api_key);
    free(job->bot_token);
    free(ai_reply);
    free(job);

    log_timestamp("INFO", req_id, "Worker thread finished and freed memory.");
    pthread_exit(NULL);
}

static int snowflake_gt(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return la > lb;
    return strcmp(a, b) > 0;
}

static int mentions_bot(const char *content, const char *bot_id) {
    char pat[40], pat_nick[40];
    snprintf(pat, sizeof(pat), "<@%s>", bot_id);
    snprintf(pat_nick, sizeof(pat_nick), "<@!%s>", bot_id);
    return strstr(content, pat) != NULL || strstr(content, pat_nick) != NULL;
}

int main(void) {
    setbuf(stdout, NULL);
    curl_global_init(CURL_GLOBAL_ALL);

    const char *bot_token = DISCORD_BOT_TOKEN[0] ? DISCORD_BOT_TOKEN : getenv("DISCORD_BOT_TOKEN");
    const char *channel_id = DISCORD_CHANNEL_ID[0] ? DISCORD_CHANNEL_ID : getenv("DISCORD_CHANNEL_ID");
    const char *groq_api_key = GROQ_API_KEY[0] ? GROQ_API_KEY : getenv("GROQ_API_KEY");

    if (!bot_token || !channel_id || !groq_api_key) {
        fprintf(stderr, "[FATAL] Missing DISCORD_BOT_TOKEN, DISCORD_CHANNEL_ID, or GROQ_API_KEY.\n");
        return 1;
    }

    CURL *h = curl_easy_init();
    if (!h) {
        fprintf(stderr, "[FATAL] curl_easy_init() failed\n");
        return 1;
    }

    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bot %s", bot_token);

    char get_url[256];
    snprintf(get_url, sizeof(get_url), "%s/channels/%s/messages", DISCORD_API_BASE, channel_id);

    MemoryBuffer msg_mb = { .memory = malloc(1), .size = 0 };

    char bot_id[32] = "";
    {
        char me_url[128];
        snprintf(me_url, sizeof(me_url), "%s/users/@me", DISCORD_API_BASE);
        long status = http_get(h, me_url, auth_header, &msg_mb, 0);
        if (status == 200 && msg_mb.memory) {
            cJSON *me_json = cJSON_Parse(msg_mb.memory);
            if (me_json) {
                cJSON *id_obj = cJSON_GetObjectItem(me_json, "id");
                if (cJSON_IsString(id_obj) && id_obj->valuestring) {
                    strncpy(bot_id, id_obj->valuestring, sizeof(bot_id) - 1);
                }
                cJSON_Delete(me_json);
            }
        }
        if (bot_id[0] == '\0') {
            log_timestamp("FATAL", 0, "Could not fetch Bot ID from /users/@me (HTTP %ld). Check DISCORD_BOT_TOKEN.", status);
            free(msg_mb.memory);
            curl_easy_cleanup(h);
            return 1;
        }
    }

    char last_id[32] = "0";

    {
        char url[300];
        snprintf(url, sizeof(url), "%s?limit=1", get_url);
        long status = http_get(h, url, auth_header, &msg_mb, 0);
        if (status == 200 && msg_mb.memory) {
            cJSON *arr = cJSON_Parse(msg_mb.memory);
            if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
                cJSON *msg0 = cJSON_GetArrayItem(arr, 0);
                cJSON *id_obj = cJSON_GetObjectItem(msg0, "id");
                if (cJSON_IsString(id_obj) && id_obj->valuestring) {
                    strncpy(last_id, id_obj->valuestring, sizeof(last_id) - 1);
                }
            }
            if (arr) cJSON_Delete(arr);
        }
    }

    printf("=========================================================\n");
    printf("  Discord REST Polling Bot Watching Channel %s\n", channel_id);
    printf("  Bot User ID: %s | Starting After Msg ID: %s\n", bot_id, last_id);
    printf("=========================================================\n");

    int consecutive_failures = 0;

    for (;;) {
        char url[300];
        snprintf(url, sizeof(url), "%s?limit=%d&after=%s", get_url, MAX_MSG_OBJS, last_id);

        long status = http_get(h, url, auth_header, &msg_mb, consecutive_failures > 0);

        if (status == 429) {
            consecutive_failures = 0;
            log_timestamp("WARN", 0, "Rate limited by Discord. Sleeping 5s.");
            sleep(5);
            continue;
        }
        if (status != 200) {
            consecutive_failures++;
            log_timestamp("ERROR", 0, "GET messages failed (HTTP %ld), retry in %ds (%d consecutive)",
                          status, POLL_INTERVAL_SECS, consecutive_failures);

            if (consecutive_failures >= MAX_CONSECUTIVE_FAIL) {
                log_timestamp("WARN", 0, "Reinitializing curl handle after consecutive failures.");
                curl_easy_cleanup(h);
                h = curl_easy_init();
                consecutive_failures = 0;
            }
            sleep(POLL_INTERVAL_SECS);
            continue;
        }
        consecutive_failures = 0;

        cJSON *arr = cJSON_Parse(msg_mb.memory);
        if (cJSON_IsArray(arr)) {
            int count = cJSON_GetArraySize(arr);
            for (int i = 0; i < count; i++) {
                cJSON *msg = cJSON_GetArrayItem(arr, i);
                cJSON *id_obj = cJSON_GetObjectItem(msg, "id");
                cJSON *author = cJSON_GetObjectItem(msg, "author");
                cJSON *content_obj = cJSON_GetObjectItem(msg, "content");

                if (!cJSON_IsString(id_obj) || !id_obj->valuestring) continue;
                const char *msg_id = id_obj->valuestring;

                if (snowflake_gt(msg_id, last_id)) {
                    strncpy(last_id, msg_id, sizeof(last_id) - 1);
                }

                if (author) {
                    cJSON *is_bot = cJSON_GetObjectItem(author, "bot");
                    if (is_bot && cJSON_IsTrue(is_bot)) continue;
                }

                if (!cJSON_IsString(content_obj) || !content_obj->valuestring) continue;
                const char *content = content_obj->valuestring;

                if (!mentions_bot(content, bot_id)) continue;

                long req_id = ++g_request_counter;
                log_timestamp("INFO", req_id, "Received direct mention message ID %s: \"%s\"", msg_id, content);

                // =========================================================================
                // STEP 2 INTERCEPTION: Local MicroVM System & Sandbox Commands
                // =========================================================================
                char local_reply[2048] = {0};

                if (strstr(content, "!sys")) {
                    char net_info[256];
                    get_guest_ip(net_info, sizeof(net_info));
                    snprintf(local_reply, sizeof(local_reply),
                             "**MicroVM Sandbox Telemetry**\n"
                             "- **VM Identity:** `%s`\n"
                             "- **Guest PID:** `%d`\n"
                             "- **Network:** `%s`",
                             getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01",
                             getpid(), net_info);
                             
                    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
                    continue; // Skip worker thread & Groq API
                } 
                else if (strstr(content, "!write")) {
                    const char *data_ptr = strstr(content, "!write");
                    data_ptr += 6; // Move past "!write"
                    while (isspace((unsigned char)*data_ptr)) data_ptr++; // Trim leading whitespace

                    if (strlen(data_ptr) == 0) {
                        data_ptr = "Default Sandbox Payload Text";
                    }

                    if (write_guest_file("/tmp/sandbox.txt", data_ptr) == 0) {
                        snprintf(local_reply, sizeof(local_reply),
                                 " Successfully wrote payload (`\"%s\"`) to local ephemeral disk (`/tmp/sandbox.txt`) on **%s**.",
                                 data_ptr,
                                 getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01");
                    } else {
                        snprintf(local_reply, sizeof(local_reply), " Failed to write to disk on microVM sandbox.");
                    }
                    
                    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
                    continue; // Skip worker thread & Groq API
                } 
                else if (strstr(content, "!read")) {
                    char file_contents[1024];
                    read_guest_file("/tmp/sandbox.txt", file_contents, sizeof(file_contents));
                    
                    snprintf(local_reply, sizeof(local_reply),
                             "**Read from `/tmp/sandbox.txt` on %s:**\n```\n%s\n```",
                             getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01",
                             file_contents);
                             
                    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
                    continue; // Skip worker thread & Groq API
                }
		else if (strstr(content, "!memstore")) {
    const char *data_ptr = strstr(content, "!memstore") + 9;
    while (isspace((unsigned char)*data_ptr)) data_ptr++;

    if (strlen(data_ptr) > 0) {
        snprintf(g_heap_storage, sizeof(g_heap_storage), "%s", data_ptr);
        snprintf(local_reply, sizeof(local_reply),
                 " Stored payload in heap RAM (`0x%p`) on **%s**.",
                 (void*)g_heap_storage,
                 getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01");
    } else {
        snprintf(local_reply, sizeof(local_reply), " Provide a string to store in memory.");
    }

    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
    continue;
}
else if (strstr(content, "!memread")) {
    snprintf(local_reply, sizeof(local_reply),
             "**Heap Memory Payload on %s (Address `0x%p`):**\n```\n%s\n```",
             getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01",
             (void*)g_heap_storage,
             g_heap_storage);

    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
    continue;
}
else if (strstr(content, "!uptime")) {
    char uptime_str[128];
    get_guest_uptime(uptime_str, sizeof(uptime_str));

    snprintf(local_reply, sizeof(local_reply),
             "**MicroVM Guest Statistics**\n- **Node:** `%s`\n- **%s**",
             getenv("VM_IDENTITY") ? getenv("VM_IDENTITY") : "microvm-guest-01",
             uptime_str);

    send_discord_reply(req_id, channel_id, msg_id, bot_token, local_reply);
    continue;
}
                // =========================================================================

                AsyncJobContext *job = malloc(sizeof(AsyncJobContext));
                job->channel_id = strdup(channel_id);
                job->message_id = strdup(msg_id);
                job->user_prompt = strdup(content);
                job->groq_api_key = strdup(groq_api_key);
                job->bot_token = strdup(bot_token);
                job->request_id = req_id;

                pthread_t thread_id;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

                int rc = pthread_create(&thread_id, &attr, async_groq_worker, (void *)job);
                pthread_attr_destroy(&attr);

                if (rc == 0) {
                    log_timestamp("INFO", req_id, "Successfully spawned background pthread worker.");
                } else {
                    log_timestamp("ERROR", req_id, "pthread_create failed with error code %d!", rc);
                }
            }
            cJSON_Delete(arr);
        }

        sleep(POLL_INTERVAL_SECS);
    }

    free(msg_mb.memory);
    curl_easy_cleanup(h);
    curl_global_cleanup();
    return 0;
}
