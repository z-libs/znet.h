
/* zhttpd_async.c - v1.0.0 High-Performance (Linux Only)
 *
 * Usage:
 * ./zhttpd-async [DIR]
 * Example: ./zhttpd-async .
 *
 * COMPILE:
 * Linux:   gcc zhttpd_async.c -o zhttpd-async -std=c11 -O3 -D_GNU_SOURCE
 * Windows: Not Supported (Use zhttpd_std.c)
 *
 * OR BUILD:
 * Linux: make
*/

#define ZERROR_IMPLEMENTATION
#define ZERROR_SHORT_NAMES
#include "zerror.h"

#define ZFILE_IMPLEMENTATION
#include "zfile.h"

#define ZSTR_IMPLEMENTATION
#include "zstr.h"

#define ZNET_IMPLEMENTATION
#include "znet.h"

#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <netinet/tcp.h>

#define MAX_CONNS 10000
#define BUF_SIZE  8192
#define MAX_EVENTS 1024

typedef struct 
{
    int port;
    int workers;
    char root[1024];
} ServerConfig;

ServerConfig g_config = 
{ 
    .port = 8080, 
    .workers = 0,
    .root = "." 
};

static int g_epoll_fd;
static char g_date_header[64];
static time_t g_last_date = 0;

typedef enum 
{
    STATE_READ,
    STATE_WRITE_HEADER,
    STATE_WRITE_FILE,
    STATE_WRITE_MEM,
    STATE_CLOSE
} conn_state;

typedef struct 
{
    int fd;
    conn_state state;
    char req[BUF_SIZE];
    int req_len;
    char res_head[1024];
    int head_len;
    int head_sent;
    int file_fd;
    off_t file_off;
    size_t file_rem;
    char *mem_buf;
    int mem_len;
    int mem_sent;
    bool keep_alive;
} zconn_t;

static zconn_t g_conns[MAX_CONNS];

static void update_date(void) 
{
    time_t now = time(NULL);
    if (now != g_last_date) 
    {
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(g_date_header, sizeof(g_date_header), "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
        g_last_date = now;
    }
}

static inline int znet_raw(znet_socket s) 
{ 
    return (int)s.handle; 
}

static void conn_reset(zconn_t *c) 
{
    c->state = STATE_READ;
    c->req_len = 0;
    c->head_len = 0; c->head_sent = 0;
    if (c->file_fd != -1) 
    { 
        close(c->file_fd); 
        c->file_fd = -1; 
    }
    if (c->mem_buf) 
    { 
        free(c->mem_buf); 
        c->mem_buf = NULL; 
    }
    c->keep_alive = false;
}

static void conn_close(zconn_t *c) 
{
    if (c->fd != -1) 
    {
        epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        znet_socket s = 
        { 
            .handle = c->fd, 
            .valid = true 
        };
        znet_close(&s);
        c->fd = -1;
    }
    conn_reset(c);
}

static void prepare_error(zconn_t *c, int code, const char *msg) 
{
    update_date();
    if (c->mem_buf) 
    {
        free(c->mem_buf);
    }
    
    char *body = malloc(512);
    int blen = sprintf(body, "<h1>%d %s</h1>", code, msg);
    c->mem_buf = body; 
    c->mem_len = blen; 
    c->mem_sent = 0;
    
    c->head_len = sprintf(c->res_head,
        "HTTP/1.1 %d %s\r\nServer: zhttpd-async\r\n%sContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        code, msg, g_date_header, blen);
    
    c->keep_alive = false;
    c->state = STATE_WRITE_HEADER;
}

static void prepare_file(zconn_t *c, const char *path) 
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) 
    { 
        prepare_error(c, 404, "Not Found"); 
        return; 
    }
    
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) 
    { 
        close(fd); 
        prepare_error(c, 403, "Is Directory"); 
        return; 
    }
    
    update_date();
    c->file_fd = fd;
    c->file_rem = st.st_size;
    c->file_off = 0;
    
    c->head_len = sprintf(c->res_head,
        "HTTP/1.1 200 OK\r\nServer: zhttpd-async\r\n%sContent-Length: %ld\r\nConnection: %s\r\n\r\n",
        g_date_header, st.st_size, c->keep_alive ? "Keep-Alive" : "close");
        
    c->state = STATE_WRITE_HEADER;
}

static void handle_io(zconn_t *c, uint32_t events) 
{
    if (events & EPOLLIN) 
    {
        znet_socket s = 
        { 
            .handle = c->fd, 
            .valid = true 
        };
        int n = znet_recv(s, c->req + c->req_len, BUF_SIZE - c->req_len - 1);
        
        if (n <= 0) 
        { 
            conn_close(c); 
            return; 
        }
        c->req_len += n;
        c->req[c->req_len] = 0;
        
        if (strstr(c->req, "\r\n\r\n")) 
        {
            if (strncmp(c->req, "GET ", 4) != 0) 
            { 
                prepare_error(c, 405, "Bad Method"); 
            }
            else 
            {
                char *u = c->req + 4;
                char *e = strchr(u, ' '); if(e) *e=0;
                c->keep_alive = (strcasestr(c->req, "keep-alive") != NULL);
                zstr path = zfile_join(g_config.root, u);
                prepare_file(c, zstr_cstr(&path));
                zstr_free(&path);
            }
            struct epoll_event ev = 
            { 
                .events = EPOLLOUT, 
                .data.ptr = c 
            };
            epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
        }
    }
    
    if (events & EPOLLOUT) 
    {
        znet_socket s = 
        { 
            .handle = c->fd, 
            .valid = true 
        };
        
        if (c->state == STATE_WRITE_HEADER) 
        {
            int n = znet_send(s, c->res_head + c->head_sent, c->head_len - c->head_sent);
            if (n < 0) 
            { 
                conn_close(c); 
                return; 
            }
            c->head_sent += n;
            if (c->head_sent == c->head_len) 
            {
                if (c->file_fd != -1) 
                {
                    c->state = STATE_WRITE_FILE;
                }
                else if (c->mem_buf)  
                {
                    c->state = STATE_WRITE_MEM;
                }
            }
        }
        else if (c->state == STATE_WRITE_FILE) 
        {
            ssize_t n = sendfile(c->fd, c->file_fd, &c->file_off, c->file_rem);
            if (n < 0) 
            { 
                if (errno != EAGAIN) 
                {
                    conn_close(c); 
                }return; 
            }
            c->file_rem -= n;
            if (c->file_rem == 0) 
            {
                if (c->keep_alive) 
                {
                    conn_reset(c);
                    struct epoll_event ev = 
                    { 
                        .events = EPOLLIN, 
                        .data.ptr = c 
                    };
                    epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
                } 
                else 
                {
                    conn_close(c);
                }
            }
        }
        else if (c->state == STATE_WRITE_MEM) 
        {
            int n = znet_send(s, c->mem_buf + c->mem_sent, c->mem_len - c->mem_sent);
            if (n < 0) 
            { 
                conn_close(c); 
                return; 
            }
            c->mem_sent += n;
            if (c->mem_sent == c->mem_len) 
            {
                conn_close(c);
            }
        }
    }
}

void run_worker(znet_socket server_sock)
 {
    for(int i = 0; i<MAX_CONNS; i++) 
    {
        g_conns[i].fd = -1;
    }
    g_epoll_fd = epoll_create1(0);
    int raw_server = znet_raw(server_sock);
    
    struct epoll_event ev = 
    { 
        .events = EPOLLIN, 
        .data.fd = raw_server 
    };
    epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, raw_server, &ev);
    
    struct epoll_event events[MAX_EVENTS];
    while(1) 
    {
        int nfds = epoll_wait(g_epoll_fd, events, MAX_EVENTS, -1);
        for(int i = 0; i<nfds; i++) 
        {
            if (events[i].data.fd == raw_server) 
            {
                while(1) 
                {
                    znet_socket client = znet_accept(server_sock, NULL);
                    if (!client.valid) 
                    {
                        break; 
                    }
                    
                    int cfd = znet_raw(client);
                    if (cfd >= MAX_CONNS) 
                    { 
                        znet_close(&client); 
                        continue; 
                    }
                    
                    znet_set_nonblocking(client, true);
                    int flag=1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, 4);
                    
                    zconn_t *c = &g_conns[cfd];
                    c->fd = cfd; conn_reset(c);
                    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
                    epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, cfd, &ev);
                }
            } 
            else 
            {
                handle_io((zconn_t*)events[i].data.ptr, events[i].events);
            }
        }
    }
}

zres server_entry(int argc, char **argv) 
{
    check_sys(znet_init(), "Network init failed");
    if (argc > 1) 
    {
        strncpy(g_config.root, argv[1], 1023);
    }
    
    znet_socket s = znet_socket_create(ZNET_IPV4, ZNET_TCP);
    int opt = 1;
    setsockopt(znet_raw(s), SOL_SOCKET, SO_REUSEADDR, &opt, 4);
    setsockopt(znet_raw(s), SOL_SOCKET, SO_REUSEPORT, &opt, 4);
    
    znet_addr addr;
    znet_addr_from_str("0.0.0.0", g_config.port, &addr);
    
    if (znet_bind(s, addr) != Z_OK) 
    {
        return zres_err(zerr_create(1, "Bind failed"));
    }
    if (znet_listen(s, 10000) != Z_OK) 
    {
        return zres_err(zerr_create(2, "Listen failed"));
    }
    
    znet_set_nonblocking(s, true);
    int workers = get_nprocs();
    printf("=> zhttpd-async | Port %d | Workers %d\n", g_config.port, workers);
    
    for(int i = 0; i<workers; i++) 
    {
        if (fork() == 0) 
        { 
            run_worker(s); 
            exit(0); 
        }
    }
    while(1) 
    {
        wait(NULL);
    }
    return zres_ok();
}

int main(int argc, char **argv) 
{
    return run(server_entry(argc, argv));
}
