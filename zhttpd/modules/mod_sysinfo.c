
#if defined (_WIN32)
#   warning "Not available on Windows."
#else

#include <sys/sysinfo.h>
#include "../zmodule.h"

bool sysinfo_handler(znet_socket c, zstr_view m, zstr_view p, zstr_view req, zstr_view ip) 
{
    (void)m; 
    (void)req; 
    (void)ip;
    
    if (!zstr_view_eq(p, "/api/sysinfo")) 
    {
        return false;
    }
    
    struct sysinfo i; 
    if (0 != sysinfo(&i)) 
    {
        return false;
    }

    unsigned long total = i.totalram / 1024 / 1024;
    unsigned long free  = i.freeram / 1024 / 1024;
    unsigned long used  = total - free;

    float l1  = i.loads[0] / 65536.0f;
    float l5  = i.loads[1] / 65536.0f;
    float l15 = i.loads[2] / 65536.0f;

    // I need to change this once I have 'zjson.h'.
    zstr json = zstr_init();
    zstr_fmt(&json, 
        "{\n"
        "  \"uptime_sec\": %ld,\n"
        "  \"processes\": %d,\n"
        "  \"ram\": {\n"
        "    \"total_mb\": %lu,\n"
        "    \"used_mb\": %lu,\n"
        "    \"free_mb\": %lu\n"
        "  },\n"
        "  \"load\": [%.2f, %.2f, %.2f]\n"
        "}",
        i.uptime, 
        i.procs,
        total, used, free,
        l1, l5, l15
    );
    
    zstr h = zstr_init();
    zstr_fmt(&h, 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", 
        zstr_len(&json)
    );
    
    znet_send(c, zstr_cstr(&h), zstr_len(&h));
    znet_send(c, zstr_cstr(&json), zstr_len(&json));
    
    zstr_free(&h); 
    zstr_free(&json);
    
    return true;
}

zmodule_def z_module_entry = 
{ 
    .name = "SysInfo", 
    .handler = sysinfo_handler 
};

#endif

