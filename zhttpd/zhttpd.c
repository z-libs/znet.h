
/* zhttpd.c - v1.1.0 Production
   
   Usage:
     ./zhttpd [DIR] [PORT] [THREADS]
     Example: ./zhttpd . 8080 512

   COMPILE:
     Linux:   gcc zhttpd.c -o zhttpd -std=c11
     Windows: gcc zhttpd.c -o zhttpd.exe -lws2_32 -std=c11
*/

#if !defined(_WIN32)
#   define _POSIX_C_SOURCE 200809L
#   define _DEFAULT_SOURCE
#   include <netinet/tcp.h>
#   include <netinet/in.h>
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

// Configuration defaults.

#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 128
#define CHUNK_SIZE 16384     
#define TIMEOUT_MS 5000      
#define MAX_HEADER_SIZE 8192 

// Globals.
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
const char *root_dir = ".";
int g_thread_count = DEFAULT_THREADS;

// Signals.
void handle_sig(int sig) 
{
    (void)sig;
    pool.running = 0;
    zcond_broadcast(&pool.notify);
}

// Setup helper.

zres setup_server(znet_socket *out_server, int port) 
{
    if (znet_init() != 0) return zres_err(zerr_create(1, "Net init failed"));

    znet_socket s = znet_socket_create(ZNET_IPV4, ZNET_TCP);
    if (!s.valid) return zres_err(zerr_errno(errno, "Socket create failed"));

    int opt = 1;
#   ifdef _WIN32
    setsockopt((SOCKET)s.handle, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#   else
    setsockopt((int)s.handle, SOL_SOCKET, SO_REUSEADDR, (void*)&opt, sizeof(opt));
#   endif

    znet_addr bind_addr;
    znet_addr_from_str("0.0.0.0", port, &bind_addr);
    
    if (Z_OK != znet_bind(s, bind_addr)) 
    {
        return zres_err(zerr_errno(errno, "Bind failed"));
    }
    if (Z_OK != znet_listen(s, 1024)) 
    {
        return zres_err(zerr_errno(errno, "Listen failed"));
    }

    *out_server = s;
    return zres_ok();
}

// Logging
void access_log(const char *ip, int status, const char *method, const char *path) 
{
    // Uncomment for debugging I guess.
    // Maybe I will add a macro flag or something.
    /*
    time_t now = time(NULL);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] %s \"%s %s\" %d\n", tbuf, ip, method, path, status);
    */
}

// Helpers.
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

void serve_file(znet_socket client, const char *full_path, int status_code, bool keep_alive) 
{
    FILE *f = fopen(full_path, "rb");
    if (!f) 
    {
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    zstr header = zstr_init();
    zstr_fmt(&header, 
        "HTTP/1.1 %d %s\r\n"
        "Server: zhttpd/1.1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: %s\r\n\r\n", 
        status_code, (status_code == 200 ? "OK" : "Not Found"),
        get_mime(full_path), 
        fsize,
        keep_alive ? "keep-alive" : "close");
   
    send_all(client, zstr_cstr(&header), zstr_len(&header));
    zstr_free(&header);

    char chunk[CHUNK_SIZE];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) 
    {
        if (!send_all(client, chunk, n)) 
        {
            break;
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

// Request handler.
void handle_request(znet_socket client, const char *client_ip) 
{
    char buf[MAX_HEADER_SIZE];
    znet_set_timeout(client, TIMEOUT_MS);

    while (1) 
    {
        z_ssize_t received = znet_recv(client, buf, MAX_HEADER_SIZE - 1);
        if (received <= 0) 
        {
            break; 
        }
        buf[received] = '\0';

        bool keep_alive = false;
        
        if (strstr(buf, "Connection: keep-alive") || strstr(buf, "Connection: Keep-Alive")) 
        {
            keep_alive = true;
        }

        zstr_view req_v = zstr_view_from(buf);
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
            if (!keep_alive) 
            {
                break;
            }
            continue;
        }

        zstr url_path = zstr_from_view(url_v);
        zstr fs_path = zfile_join(root_dir, zstr_cstr(&url_path));
        zfile_normalize(&fs_path);

        if (zstr_contains(&fs_path, "..")) 
        {
            send_error_page(client, 403, "Access Denied", keep_alive);
            zstr_free(&url_path); zstr_free(&fs_path);
            if (!keep_alive) break;
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
                    serve_file(client, zstr_cstr(&index_path), 200, keep_alive);
                    access_log(client_ip, 200, "GET", zstr_cstr(&url_path));
                } 
                else 
                {
                    send_error_page(client, 403, "Forbidden", keep_alive);
                    access_log(client_ip, 403, "GET", zstr_cstr(&url_path));
                }
                zstr_free(&index_path);
            } 
            else 
            {
                serve_file(client, zstr_cstr(&fs_path), 200, keep_alive);
                access_log(client_ip, 200, "GET", zstr_cstr(&url_path));
            }
        } 
        else 
        {
            send_error_page(client, 404, "Page Not Found", keep_alive);
            access_log(client_ip, 404, "GET", zstr_cstr(&url_path));
        }
        zstr_free(&url_path);
        zstr_free(&fs_path);
        
        if (!keep_alive) 
        {
            break;
        }
    }
}

// Worker.
void worker_routine(void *arg) 
{
    (void)arg;
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
        handle_request(job.socket, job.client_ip);
        znet_close(&job.socket);
    }
}

int main(int argc, char **argv) 
{
    // Argument Parsing: ./zhttpd [DIR] [PORT] [THREADS]
    int port = DEFAULT_PORT;
    
    if (argc > 1) 
    {
        root_dir = argv[1];
    }
    if (argc > 2) 
    {
        port = atoi(argv[2]);
    }
    if (argc > 3) 
    {
        g_thread_count = atoi(argv[3]);
    }

    if (port <= 0) 
    {
        port = DEFAULT_PORT;
    }
    if (g_thread_count < 1) 
    {
        g_thread_count = 1;
    }
    if (g_thread_count > 10000) 
    {
        g_thread_count = 10000;
    }

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);
#   ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); 
#   endif

    znet_socket server;
    zres res = setup_server(&server, port);

    if (!res.is_ok) 
    {
        zerr_print(res.err);
        return 1;
    }

    pool.size = g_thread_count * 4;
    pool.queue = calloc(pool.size, sizeof(job_t));
    pool.head = 0; pool.tail = 0; pool.count = 0; pool.running = 1;
    zmutex_init(&pool.lock); zcond_init(&pool.notify);

    zthread_t *threads = (zthread_t*)malloc(sizeof(zthread_t) * g_thread_count);
    if (!threads) 
    {
        fprintf(stderr, "Failed to allocate memory for threads.\n");
        return 1;
    }

    printf("=> zhttpd v1.3 (Smart Mode)\n");
    printf("   Serving: %s\n", root_dir);
    printf("   Port:    %d\n", port);
    printf("   Threads: %d\n", g_thread_count);

    for (int i = 0; i < g_thread_count; i++) 
    {
        zthread_create(&threads[i], worker_routine, NULL);
    }

    while (pool.running) 
    {
        znet_addr client_addr;
        znet_socket client = znet_accept(server, &client_addr);
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
    return 0;
}
