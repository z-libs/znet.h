
/* zhttpd.c - v1.1.0 Production
 *
 * Usage:
 *   ./zhttpd [DIR] [PORT] [THREADS]
 *   Example: ./zhttpd . 8080 512
 *
 *  COMPILE:
 *   Linux:   gcc zhttpd.c -o zhttpd -std=c11
 *   Windows: gcc zhttpd.c -o zhttpd.exe -lws2_32 -std=c11
 *
 *  OR BUILD:
 *   Linux: make
 *   Windows: build
*/

#if !defined(_WIN32)
#   define _POSIX_C_SOURCE 200809L
#   define _DEFAULT_SOURCE
#   include <netinet/tcp.h>
#   include <netinet/in.h>
#   include <unistd.h>
#endif

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <winsock2.h>
#endif

#define ZERROR_IMPLEMENTATION
#define ZERROR_SHORT_NAMES
#include "zerror.h"

#define ZFILE_IMPLEMENTATION
#include "zfile.h"

#define ZNET_IMPLEMENTATION
#include "znet.h"

#define ZTHREAD_IMPLEMENTATION
#include "zthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#   define write(fd, buf, len) send(fd, buf, len, 0)
#endif

// Helper to get file modification time.
static time_t get_file_mtime(const char *path) 
{
    struct stat st;
    if (stat(path, &st) == 0) 
    {
        return st.st_mtime;
    }
    return 0;
}

// Defaults.

#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 128
#define DEFAULT_TIMEOUT 5000
#define DEFAULT_CHUNK 16384
#define MAX_HEADER_SIZE 8192

// Cache defaults.
#define DEFAULT_CACHE_ENABLED 1
#define DEFAULT_CACHE_MAX_SIZE (64 * 1024 * 1024)  // 64 MB total cache
#define DEFAULT_CACHE_MAX_FILE (1 * 1024 * 1024)   // 1 MB max per file
#define DEFAULT_CACHE_ENTRIES 1024                  // Max cached files 

// Globals.

int g_port = DEFAULT_PORT;
int g_threads = DEFAULT_THREADS;
int g_timeout = DEFAULT_TIMEOUT;
int g_chunk_size = DEFAULT_CHUNK;
char g_root[1024] = ".";

// Cache globals.
int g_cache_enabled = DEFAULT_CACHE_ENABLED;
size_t g_cache_max_size = DEFAULT_CACHE_MAX_SIZE;
size_t g_cache_max_file = DEFAULT_CACHE_MAX_FILE;
int g_cache_max_entries = DEFAULT_CACHE_ENTRIES;

typedef struct 
{
    znet_socket socket;
    char client_ip[64];
} job_t;

typedef struct 
{
    job_t *queue;
    int head, tail, count, size;
    zmutex_t lock;      
    zcond_t  notify;    
    volatile int running;
} ThreadPool;

ThreadPool pool;

// File cache entry.
typedef struct CacheEntry 
{
    char *path;              // File path (key)
    char *data;              // File contents
    size_t size;             // File size
    time_t mtime;            // Last modification time
    time_t last_access;      // Last access time (for LRU)
    struct CacheEntry *prev; // LRU list prev
    struct CacheEntry *next; // LRU list next
} CacheEntry;

// File cache.
typedef struct 
{
    CacheEntry **buckets;    // Hash table buckets
    int bucket_count;        // Number of buckets
    CacheEntry *lru_head;    // Most recently used
    CacheEntry *lru_tail;    // Least recently used
    size_t total_size;       // Total cached bytes
    int entry_count;         // Number of cached files
    zmutex_t lock;           // Thread safety
    size_t hits;             // Cache hits
    size_t misses;           // Cache misses
} FileCache;

FileCache g_cache;

// Signal handler.
void handle_sig(int sig) 
{
    (void)sig;
    
    static time_t last_time = 0;
    time_t now = time(NULL);

    if (now - last_time < 2) 
    {
        pool.running = 0;
        zcond_broadcast(&pool.notify); // Wake up workers.
        
        // Use write() because printf() is unsafe in signal handlers.
        const char msg[] = "\n>>> Shutdown Initiated. Bye!\n";
        write(1, msg, sizeof(msg) - 1);
    } 
    else 
    {
        last_time = now;
        const char msg[] = "\n[?] Press Ctrl+C again to stop server.\n";
        write(1, msg, sizeof(msg) - 1);
    }
}

// =========================================================================
// FILE CACHE FUNCTIONS
// =========================================================================

// Simple hash function for file paths.
static unsigned int cache_hash(const char *path) 
{
    unsigned int hash = 5381;
    while (*path) 
    {
        hash = ((hash << 5) + hash) + (unsigned char)(*path++);
    }
    return hash;
}

// Initialize the cache.
void cache_init(void) 
{
    if (!g_cache_enabled) return;
    
    g_cache.bucket_count = g_cache_max_entries;
    g_cache.buckets = (CacheEntry**)calloc(g_cache.bucket_count, sizeof(CacheEntry*));
    g_cache.lru_head = NULL;
    g_cache.lru_tail = NULL;
    g_cache.total_size = 0;
    g_cache.entry_count = 0;
    g_cache.hits = 0;
    g_cache.misses = 0;
    zmutex_init(&g_cache.lock);
}

// Free a cache entry.
static void cache_free_entry(CacheEntry *entry) 
{
    if (entry) 
    {
        free(entry->path);
        free(entry->data);
        free(entry);
    }
}

// Move entry to front of LRU list (most recently used).
static void cache_lru_touch(CacheEntry *entry) 
{
    if (entry == g_cache.lru_head) return; // Already at front
    
    // Remove from current position
    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (entry == g_cache.lru_tail) g_cache.lru_tail = entry->prev;
    
    // Insert at front
    entry->prev = NULL;
    entry->next = g_cache.lru_head;
    if (g_cache.lru_head) g_cache.lru_head->prev = entry;
    g_cache.lru_head = entry;
    if (!g_cache.lru_tail) g_cache.lru_tail = entry;
    
    entry->last_access = time(NULL);
}

// Remove entry from hash table and LRU list.
static void cache_remove_entry(CacheEntry *entry) 
{
    // Remove from hash table
    unsigned int idx = cache_hash(entry->path) % g_cache.bucket_count;
    CacheEntry *curr = g_cache.buckets[idx];
    CacheEntry *prev_bucket = NULL;
    
    while (curr) 
    {
        if (curr == entry) 
        {
            if (prev_bucket) 
                prev_bucket->next = curr->next;
            else 
                g_cache.buckets[idx] = curr->next;
            break;
        }
        prev_bucket = curr;
        curr = curr->next;
    }
    
    // Remove from LRU list
    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (entry == g_cache.lru_head) g_cache.lru_head = entry->next;
    if (entry == g_cache.lru_tail) g_cache.lru_tail = entry->prev;
    
    g_cache.total_size -= entry->size;
    g_cache.entry_count--;
}

// Evict least recently used entries until we have enough space.
static void cache_evict(size_t needed_size) 
{
    while (g_cache.lru_tail && 
           (g_cache.total_size + needed_size > g_cache_max_size || 
            g_cache.entry_count >= g_cache_max_entries)) 
    {
        CacheEntry *victim = g_cache.lru_tail;
        cache_remove_entry(victim);
        cache_free_entry(victim);
    }
}

// Look up a file in the cache. Returns NULL if not found or stale.
CacheEntry* cache_lookup(const char *path) 
{
    if (!g_cache_enabled || !g_cache.buckets) return NULL;
    
    unsigned int idx = cache_hash(path) % g_cache.bucket_count;
    CacheEntry *entry = g_cache.buckets[idx];
    
    while (entry) 
    {
        if (strcmp(entry->path, path) == 0) 
        {
            // Check if file was modified since cached
            time_t current_mtime = get_file_mtime(path);
            if (current_mtime != entry->mtime) 
            {
                // File changed, remove stale entry
                cache_remove_entry(entry);
                cache_free_entry(entry);
                return NULL;
            }
            cache_lru_touch(entry);
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

// Insert a file into the cache.
CacheEntry* cache_insert(const char *path, const char *data, size_t size, time_t mtime) 
{
    if (!g_cache_enabled || !g_cache.buckets) return NULL;
    if (size > g_cache_max_file) return NULL; // Too big to cache
    
    // Evict if necessary
    cache_evict(size);
    
    // Create new entry
    CacheEntry *entry = (CacheEntry*)malloc(sizeof(CacheEntry));
    if (!entry) return NULL;
    
    entry->path = strdup(path);
    entry->data = (char*)malloc(size);
    if (!entry->path || !entry->data) 
    {
        free(entry->path);
        free(entry->data);
        free(entry);
        return NULL;
    }
    
    memcpy(entry->data, data, size);
    entry->size = size;
    entry->mtime = mtime;
    entry->last_access = time(NULL);
    entry->prev = NULL;
    entry->next = NULL;
    
    // Add to hash table
    unsigned int idx = cache_hash(path) % g_cache.bucket_count;
    entry->next = g_cache.buckets[idx];
    if (g_cache.buckets[idx]) g_cache.buckets[idx]->prev = entry;
    g_cache.buckets[idx] = entry;
    
    // Add to LRU front
    entry->next = g_cache.lru_head;
    entry->prev = NULL;
    if (g_cache.lru_head) g_cache.lru_head->prev = entry;
    g_cache.lru_head = entry;
    if (!g_cache.lru_tail) g_cache.lru_tail = entry;
    
    g_cache.total_size += size;
    g_cache.entry_count++;
    
    return entry;
}

// Free the entire cache.
void cache_destroy(void) 
{
    if (!g_cache.buckets) return;
    
    CacheEntry *curr = g_cache.lru_head;
    while (curr) 
    {
        CacheEntry *next = curr->next;
        cache_free_entry(curr);
        curr = next;
    }
    
    free(g_cache.buckets);
    g_cache.buckets = NULL;
}

void load_config(const char *filename) 
{
    if (!zfile_exists(filename)) 
    {
        return;
    }

    printf("Loading config: %s\n", filename);
    
    ZFILE_FOR_EACH_LINE(filename, line) 
    {
        if (line.len == 0 || line.data[0] == '#') 
        {
            continue;
        }
        
        zstr_split_iter it = zstr_split_init(line, "=");
        zstr_view key, val;

        if (zstr_split_next(&it, &key) && zstr_split_next(&it, &val)) 
        {
            if (zstr_view_eq(key, "port"))       zstr_view_to_int(val, &g_port);
            if (zstr_view_eq(key, "threads"))    zstr_view_to_int(val, &g_threads);
            if (zstr_view_eq(key, "timeout"))    zstr_view_to_int(val, &g_timeout);
            if (zstr_view_eq(key, "chunk_size")) zstr_view_to_int(val, &g_chunk_size);
            
            // Cache settings
            if (zstr_view_eq(key, "cache_enabled")) zstr_view_to_int(val, &g_cache_enabled);
            if (zstr_view_eq(key, "cache_max_size")) 
            {
                int mb = 0;
                zstr_view_to_int(val, &mb);
                g_cache_max_size = (size_t)mb * 1024 * 1024;
            }
            if (zstr_view_eq(key, "cache_max_file")) 
            {
                int kb = 0;
                zstr_view_to_int(val, &kb);
                g_cache_max_file = (size_t)kb * 1024;
            }
            if (zstr_view_eq(key, "cache_max_entries")) zstr_view_to_int(val, &g_cache_max_entries);
            
            if (zstr_view_eq(key, "root")) 
            {
                int len = (int)val.len;
                if (len > 1023) 
                {
                    len = 1023;
                }
                memcpy(g_root, val.data, len);
                g_root[len] = '\0';
                if (len > 0 && (g_root[len-1] == '\r' || g_root[len-1] == '\n'))
                { 
                    g_root[len-1] = '\0';
                }
            }
        }
    }
}

zres setup_server(znet_socket *out_server, int port) 
{
    check_sys(znet_init(), "Network init failed");

    znet_socket s = znet_socket_create(ZNET_IPV4, ZNET_TCP);
    ensure(s.valid, 1, "Socket creation failed");

    int opt = 1;
#   ifdef _WIN32
    setsockopt((SOCKET)s.handle, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#   else
    setsockopt((int)s.handle, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt));
#   endif

    znet_addr bind_addr;
    znet_addr_from_str("0.0.0.0", port, &bind_addr);
    
    if (znet_bind(s, bind_addr) != Z_OK) 
    {
        return zres_err(zerr_create(2, "Bind failed on port %d", port));
    }

    if (znet_listen(s, 1024) != Z_OK) 
    {
        return zres_err(zerr_create(3, "Listen failed"));
    }

    *out_server = s;
    return zres_ok();
}

const char* get_mime(const char *path) 
{
    zstr_view ext = zfile_ext(path);
    if (zstr_view_eq(ext, ".html")) return "text/html; charset=utf-8"; 
    if (zstr_view_eq(ext, ".css"))  return "text/css";
    if (zstr_view_eq(ext, ".js"))   return "application/javascript";
    if (zstr_view_eq(ext, ".json")) return "application/json";
    if (zstr_view_eq(ext, ".png"))  return "image/png";
    if (zstr_view_eq(ext, ".jpg"))  return "image/jpeg";
    return "application/octet-stream";
}

bool send_all(znet_socket s, const char *buf, size_t len) 
{
    size_t total = 0;
    while (total < len) 
    {
        z_ssize_t n = znet_send(s, buf + total, len - total);
        if (n <= 0) 
        {
            return false;
        }
        total += n;
    }
    return true;
}

void serve_file(znet_socket client, const char *full_path, int status_code, bool keep_alive, char *buffer, size_t buf_len) 
{
    CacheEntry *cached = NULL;
    
    // Try cache lookup (with lock for thread safety)
    if (g_cache_enabled) 
    {
        zmutex_lock(&g_cache.lock);
        cached = cache_lookup(full_path);
        if (cached) 
        {
            g_cache.hits++;
            
            // Build and send header
            zstr header = zstr_init();
            zstr_fmt(&header, 
                "HTTP/1.1 %d %s\r\n"
                "Server: zhttpd/1.1\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n"
                "X-Cache: HIT\r\n\r\n", 
                status_code, (status_code == 200 ? "OK" : "Not Found"),
                get_mime(full_path), 
                cached->size,
                keep_alive ? "keep-alive" : "close");
            
            send_all(client, zstr_cstr(&header), zstr_len(&header));
            zstr_free(&header);
            
            // Send cached data
            send_all(client, cached->data, cached->size);
            zmutex_unlock(&g_cache.lock);
            return;
        }
        g_cache.misses++;
        zmutex_unlock(&g_cache.lock);
    }
    
    // Cache miss - read from file
    FILE *f = fopen(full_path, "rb");
    if (!f) 
    {
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // Determine if file should be cached
    bool should_cache = g_cache_enabled && (size_t)fsize <= g_cache_max_file;
    char *file_data = NULL;
    
    if (should_cache) 
    {
        // Read entire file for caching
        file_data = (char*)malloc(fsize);
        if (file_data) 
        {
            if (fread(file_data, 1, fsize, f) != (size_t)fsize) 
            {
                free(file_data);
                file_data = NULL;
                should_cache = false;
                fseek(f, 0, SEEK_SET); // Reset for normal read
            }
        } 
        else 
        {
            should_cache = false;
        }
    }

    zstr header = zstr_init();
    zstr_fmt(&header, 
        "HTTP/1.1 %d %s\r\n"
        "Server: zhttpd/1.1\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: %s\r\n"
        "X-Cache: MISS\r\n\r\n", 
        status_code, (status_code == 200 ? "OK" : "Not Found"),
        get_mime(full_path), 
        fsize,
        keep_alive ? "keep-alive" : "close");
   
    send_all(client, zstr_cstr(&header), zstr_len(&header));
    zstr_free(&header);

    if (should_cache && file_data) 
    {
        // Send from cached data and add to cache
        send_all(client, file_data, fsize);
        
        zmutex_lock(&g_cache.lock);
        cache_insert(full_path, file_data, fsize, get_file_mtime(full_path));
        zmutex_unlock(&g_cache.lock);
        
        free(file_data);
    } 
    else 
    {
        // Stream file in chunks
        size_t n;
        while ((n = fread(buffer, 1, buf_len, f)) > 0) 
        {
            if (!send_all(client, buffer, n)) 
            {
                break;
            }
        }
    }
    fclose(f);
}

void send_error_page(znet_socket client, int code, const char *msg, bool keep_alive) 
{
    char body[512];
    int len = snprintf(body, sizeof(body), "<h1>%d %s</h1><p>%s</p>", code, (code==404?"Not Found":"Error"), msg);
    char header[512];
    int hlen = snprintf(header, sizeof(header), 
        "HTTP/1.1 %d Error\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n", 
        code, len, keep_alive ? "keep-alive" : "close");
    send_all(client, header, hlen);
    send_all(client, body, len);
}

void handle_request(znet_socket client, const char *client_ip, char *io_buffer, size_t io_size) 
{
    size_t header_limit = io_size > MAX_HEADER_SIZE ? MAX_HEADER_SIZE : io_size;
    znet_set_timeout(client, g_timeout);

    while (1) 
    {
        z_ssize_t received = znet_recv(client, io_buffer, header_limit - 1);
        if (received <= 0) 
        {
            break; 
        }
        io_buffer[received] = '\0';

        bool keep_alive = false;
        if (strstr(io_buffer, "Connection: keep-alive") || strstr(io_buffer, "Connection: Keep-Alive")) 
        {
            keep_alive = true;
        }

        zstr_view req_v = zstr_view_from(io_buffer);
        zstr_split_iter lines = zstr_split_init(req_v, "\r\n");
        zstr_view line;
        if (!zstr_split_next(&lines, &line)) 
        {
            break;
        }

        zstr_split_iter parts = zstr_split_init(line, " ");
        zstr_view method_v, url_v;
        if (!zstr_split_next(&parts, &method_v) || !zstr_split_next(&parts, &url_v)) 
        {
            break;
        }

        if (!zstr_view_eq(method_v, "GET"))
        {
            send_error_page(client, 405, "Only GET supported.", keep_alive);
            if (!keep_alive) break;
            continue;
        }

        zstr url_path = zstr_from_view(url_v);
        zstr fs_path = zfile_join(g_root, zstr_cstr(&url_path));
        zfile_normalize(&fs_path);

        if (zstr_contains(&fs_path, ".."))
        {
            send_error_page(client, 403, "Access Denied", keep_alive);
            zstr_free(&url_path); zstr_free(&fs_path);
            if (!keep_alive) 
            {
                break;
            }
            continue;
        }

        if (zfile_exists(zstr_cstr(&fs_path))) 
        {
            if (zfile_is_dir(zstr_cstr(&fs_path))) 
            {
                size_t url_len = zstr_len(&url_path);
                if (url_len > 0 && zstr_cstr(&url_path)[url_len - 1] != '/') 
                {
                    char header[1024];
                    snprintf(header, sizeof(header), 
                        "HTTP/1.1 301 Moved Permanently\r\nLocation: %s/\r\nContent-Length: 0\r\nConnection: %s\r\n\r\n", 
                        zstr_cstr(&url_path), keep_alive ? "keep-alive" : "close");
                    send_all(client, header, strlen(header));
                    zstr_free(&url_path); zstr_free(&fs_path);
                    if (!keep_alive)
                    {
                        break;
                    }
                    continue;
                }
                zstr index_path = zfile_join(zstr_cstr(&fs_path), "index.html");
                if (zfile_exists(zstr_cstr(&index_path))) 
                {
                    serve_file(client, zstr_cstr(&index_path), 200, keep_alive, io_buffer, io_size);
                } 
                else 
                {
                    send_error_page(client, 403, "Forbidden", keep_alive);
                }
                zstr_free(&index_path);
            } 
            else 
            {
                serve_file(client, zstr_cstr(&fs_path), 200, keep_alive, io_buffer, io_size);
            }
        } 
        else 
        {
            send_error_page(client, 404, "Page Not Found", keep_alive);
        }
        zstr_free(&url_path);
        zstr_free(&fs_path);
        
        if (!keep_alive) 
        {
            break;
        }
    }
}

void worker_routine(void *arg) 
{
    (void)arg;
    char *thread_buffer = (char*)malloc(g_chunk_size);
    if (!thread_buffer) 
    {
        return;
    }

    while (1) 
    {
        job_t job;
        zmutex_lock(&pool.lock); 
        while (pool.count == 0 && pool.running) 
        {
            zcond_wait(&pool.notify, &pool.lock);
        }
        if (!pool.running) 
        { 
            zmutex_unlock(&pool.lock); 
            break; 
        }
        job = pool.queue[pool.head];
        pool.head = (pool.head + 1) % pool.size;
        pool.count--;
        zmutex_unlock(&pool.lock);
        
        handle_request(job.socket, job.client_ip, thread_buffer, g_chunk_size);
        znet_close(&job.socket);
    }
    free(thread_buffer);
}

zres run_server(int argc, char **argv)
{
    load_config("zhttpd.conf");

    if (argc > 1) 
    {
        if (zfile_is_file(argv[1])) 
        {
            load_config(argv[1]);
        }
        else 
        {
            strncpy(g_root, argv[1], sizeof(g_root)-1);
        }
    }
    if (argc > 2) g_port = atoi(argv[2]);
    if (argc > 3) g_threads = atoi(argv[3]);

    if (g_port <= 0)         g_port = 8080;
    if (g_threads < 1)       g_threads = 1;
    if (g_threads > 10000)   g_threads = 10000;
    if (g_chunk_size < 1024) g_chunk_size = 1024;

#   ifdef _WIN32
    signal(SIGINT, handle_sig);
#   else
    struct sigaction sa;
    sa.sa_handler = handle_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); 
#   endif

    znet_socket server;
    check_wrap(setup_server(&server, g_port), "Server setup failed");

    pool.size = g_threads * 4;
    pool.queue = calloc(pool.size, sizeof(job_t));
    ensure(pool.queue != NULL, 4, "Queue alloc failed");
    
    pool.head = 0; pool.tail = 0; pool.count = 0; pool.running = 1;
    zmutex_init(&pool.lock); zcond_init(&pool.notify);

    zthread_t *threads = (zthread_t*)malloc(sizeof(zthread_t) * g_threads);
    ensure(threads != NULL, 4, "Thread alloc failed");

    // Initialize file cache
    cache_init();

    printf("=> zhttpd v1.9\n");
    printf("   Root:    %s\n", g_root);
    printf("   Port:    %d\n", g_port);
    printf("   Threads: %d\n", g_threads);
    printf("   Chunk:   %d bytes\n", g_chunk_size);
    if (g_cache_enabled) 
    {
        printf("   Cache:   enabled (max %zu MB, %zu KB/file, %d entries)\n", 
            g_cache_max_size / (1024*1024), 
            g_cache_max_file / 1024, 
            g_cache_max_entries);
    } 
    else 
    {
        printf("   Cache:   disabled\n");
    }

    for (int i = 0; i < g_threads; i++) 
    {
        zthread_create(&threads[i], worker_routine, NULL);
    }

    while (pool.running) 
    {
        znet_addr client_addr;
        znet_socket client = znet_accept(server, &client_addr);
        
        if (!pool.running) 
        {
            break;
        }

        if (client.valid) 
        {
            int flag = 1;
            setsockopt((int)client.handle, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

            zmutex_lock(&pool.lock);
            if (pool.count < pool.size) 
            {
                pool.queue[pool.tail].socket = client;
                znet_addr_to_str(client_addr, pool.queue[pool.tail].client_ip, 64);
                pool.tail = (pool.tail + 1) % pool.size;
                pool.count++;
                zcond_signal(&pool.notify);
            } 
            else 
            {
                znet_close(&client);
            }
            zmutex_unlock(&pool.lock);
        }
    }

    free(threads);
    free(pool.queue);
    znet_close(&server);
    znet_term();
    
    // Show cache stats and cleanup
    if (g_cache_enabled && g_cache.buckets) 
    {
        size_t total = g_cache.hits + g_cache.misses;
        double hit_rate = total > 0 ? (100.0 * g_cache.hits / total) : 0.0;
        printf("\n[Cache Stats]\n");
        printf("   Hits:   %zu\n", g_cache.hits);
        printf("   Misses: %zu\n", g_cache.misses);
        printf("   Rate:   %.1f%%\n", hit_rate);
        printf("   Size:   %zu KB in %d files\n", g_cache.total_size / 1024, g_cache.entry_count);
    }
    cache_destroy();
    
    return zres_ok();
}

int main(int argc, char **argv) 
{
    return run(run_server(argc, argv));
}
