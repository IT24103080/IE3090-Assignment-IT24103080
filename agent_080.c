// agent_080.c - RemoteOps Agent
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT    9410
#define SID     "0803"
#define TOKEN   "OPS-3080"
#define LOGFILE "remoteops_IT24103080.log"
#define STORAGE "./agentfiles/IT24103080/"

// ---- Global monitor state ----
static volatile int monitor_active = 0;
static int  monitor_udp_port = 0;
static char monitor_client_ip[64];
static pthread_t monitor_tid;

// ---------- Logging ----------
static void log_event(const char *event) {
    FILE *fp = fopen(LOGFILE, "a");
    if (!fp) return;
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(fp, "[%s] %s\n", ts, event);
    fclose(fp);
}

// ---------- Send a formatted response with SID ----------
static void send_response(int fd, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 32, fmt, ap);
    va_end(ap);
    strcat(buf, " SID:");
    strcat(buf, SID);
    strcat(buf, "\n");
    send(fd, buf, strlen(buf), 0);
}

// ---------- Read one line (until '\n') ----------
static int read_line(int fd, char *out, int max) {
    int i = 0;
    char c;
    while (i < max - 1) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;       // disconnect
        if (c == '\n') break;
        if (c != '\r') out[i++] = c;  // strip CR
    }
    out[i] = '\0';
    return i;
}

// ---------- Read exactly `n` bytes into a file ----------
static long recv_exact_to_file(int fd, FILE *fp, long n) {
    char buf[4096];
    long got = 0;
    while (got < n) {
        int chunk = (n - got) > (long)sizeof(buf) ? (int)sizeof(buf) : (int)(n - got);
        int r = recv(fd, buf, chunk, 0);
        if (r <= 0) break;
        fwrite(buf, 1, r, fp);
        got += r;
    }
    return got;
}

// ---------- UDP monitor thread ----------
static void* monitor_loop(void *arg) {
    (void)arg;
    int usock = socket(AF_INET, SOCK_DGRAM, 0);
    if (usock < 0) return NULL;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(monitor_udp_port);
    inet_pton(AF_INET, monitor_client_ip, &dest.sin_addr);

    while (monitor_active) {
        // Read CPU load
        float cpu = 0.0f;
        FILE *fp = fopen("/proc/loadavg", "r");
        if (fp) { fscanf(fp, "%f", &cpu); fclose(fp); }

        // Read memory
        long mtot = 0, mavail = 0;
        fp = fopen("/proc/meminfo", "r");
        if (fp) {
            char key[64]; long val;
            while (fscanf(fp, "%63s %ld", key, &val) == 2) {
                if (strcmp(key, "MemTotal:") == 0) mtot = val;
                if (strcmp(key, "MemAvailable:") == 0) { mavail = val; break; }
                // skip rest of line
                int ch; while ((ch = fgetc(fp)) != '\n' && ch != EOF);
            }
            fclose(fp);
        }
        long mem_used_mb = (mtot - mavail) / 1024;

        // Read uptime
        double up = 0.0;
        fp = fopen("/proc/uptime", "r");
        if (fp) { fscanf(fp, "%lf", &up); fclose(fp); }

        char msg[256];
        snprintf(msg, sizeof(msg),
                 "SYSINFO %.2f %ld %d SID:%s\n",
                 cpu, mem_used_mb, (int)up, SID);
        sendto(usock, msg, strlen(msg), 0,
               (struct sockaddr*)&dest, sizeof(dest));

        sleep(2);
    }
    close(usock);
    return NULL;
}

// ---------- Per-client handler (thread) ----------
typedef struct {
    int fd;
    struct sockaddr_in addr;
} client_t;

static void handle_client(client_t *cl) {
    int fd = cl->fd;
    char ip[64];
    inet_ntop(AF_INET, &cl->addr.sin_addr, ip, sizeof(ip));

    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "CONNECT from %s:%d", ip, ntohs(cl->addr.sin_port));
    log_event(logbuf);
    printf("[+] %s\n", logbuf);

    int authenticated = 0;
    char line[2048];

    while (read_line(fd, line, sizeof(line)) >= 0) {
        printf("[CMD] %s\n", line);
        snprintf(logbuf, sizeof(logbuf), "CMD: %s", line);
        log_event(logbuf);

        char cmd[64] = {0}, a1[256] = {0}, a2[256] = {0};
        sscanf(line, "%63s %255s %255s", cmd, a1, a2);

        // ---- AUTH ----
        if (strcmp(cmd, "AUTH") == 0) {
            if (strcmp(a1, TOKEN) == 0) {
                authenticated = 1;
                send_response(fd, "OK AUTHENTICATED");
                log_event("AUTH OK");
            } else {
                send_response(fd, "ERR 001 AUTH_FAILED");
                log_event("AUTH FAIL");
            }
            continue;
        }

        if (!authenticated) {
            send_response(fd, "ERR 001 AUTH_FAILED");
            continue;
        }

        // ---- SYSINFO ----
        if (strcmp(cmd, "SYSINFO") == 0) {
            float cpu = 0.0f;
            FILE *fp = fopen("/proc/loadavg", "r");
            if (fp) { fscanf(fp, "%f", &cpu); fclose(fp); }

            long mtot = 0, mavail = 0;
            fp = fopen("/proc/meminfo", "r");
            if (fp) {
                char key[64]; long val;
                while (fscanf(fp, "%63s %ld", key, &val) == 2) {
                    if (strcmp(key, "MemTotal:") == 0) mtot = val;
                    if (strcmp(key, "MemAvailable:") == 0) { mavail = val; break; }
                    int ch; while ((ch = fgetc(fp)) != '\n' && ch != EOF);
                }
                fclose(fp);
            }
            long mem_mb = (mtot - mavail) / 1024;

            double up = 0.0;
            fp = fopen("/proc/uptime", "r");
            if (fp) { fscanf(fp, "%lf", &up); fclose(fp); }

            send_response(fd, "OK SYSINFO %.2f %ld %d", cpu, mem_mb, (int)up);
            continue;
        }

        // ---- LISTPROC ----
        if (strcmp(cmd, "LISTPROC") == 0) {
            char procs[2048] = {0};
            FILE *fp = popen("ps -eo comm --no-headers | head -20 | tr '\\n' ','", "r");
            if (fp) {
                fread(procs, 1, sizeof(procs) - 1, fp);
                pclose(fp);
            }
            int l = strlen(procs);
            if (l > 0 && procs[l-1] == ',') procs[l-1] = '\0';
            send_response(fd, "OK PROCs %s", procs);
            continue;
        }

        // ---- EXEC (whitelist) ----
        if (strcmp(cmd, "EXEC") == 0) {
            const char *ok[] = {"DATE","UPTIME","DISKFREE","HOSTNAME","WHOAMI",NULL};
            int allowed = 0;
            for (int i = 0; ok[i]; i++) if (strcmp(a1, ok[i]) == 0) { allowed = 1; break; }
            if (!allowed) {
                send_response(fd, "ERR 002 COMMAND_NOT_ALLOWED");
                continue;
            }
            char sc[64];
            if (!strcmp(a1,"DATE"))      strcpy(sc, "date");
            else if (!strcmp(a1,"UPTIME"))    strcpy(sc, "uptime");
            else if (!strcmp(a1,"DISKFREE"))  strcpy(sc, "df -h /");
            else if (!strcmp(a1,"HOSTNAME"))  strcpy(sc, "hostname");
            else                              strcpy(sc, "whoami");

            char out[1024] = {0};
            FILE *fp = popen(sc, "r");
            if (fp) { fread(out, 1, sizeof(out)-1, fp); pclose(fp); }
            for (int i = 0; out[i]; i++) if (out[i] == '\n') out[i] = ' ';
            send_response(fd, "OK EXEC_RESULT %s", out);
            continue;
        }

        // ---- PUT ----
        if (strcmp(cmd, "PUT") == 0) {
            long fsize = atol(a2);
            if (fsize < 0 || fsize > 50L*1024*1024) {
                send_response(fd, "ERR 004 FILE_TOO_LARGE");
                // drain
                char tmp[1024]; long left = fsize;
                while (left > 0) { int r = recv(fd, tmp, left>1024?1024:left, 0); if (r<=0) break; left -= r; }
                continue;
            }
            char path[512];
            snprintf(path, sizeof(path), "%s%s", STORAGE, a1);
            FILE *fp = fopen(path, "wb");
            if (!fp) {
                send_response(fd, "ERR 006 FILE_WRITE_FAIL");
                char tmp[1024]; long left = fsize;
                while (left > 0) { int r = recv(fd, tmp, left>1024?1024:left, 0); if (r<=0) break; left -= r; }
                continue;
            }
            long got = recv_exact_to_file(fd, fp, fsize);
            fclose(fp);
            if (got == fsize) {
                send_response(fd, "OK FILE_RECEIVED %s", a1);
                snprintf(logbuf, sizeof(logbuf), "PUT %s (%ld bytes)", a1, fsize);
                log_event(logbuf);
            } else {
                send_response(fd, "ERR 007 INCOMPLETE");
            }
            continue;
        }

        // ---- GET ----
        if (strcmp(cmd, "GET") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s%s", STORAGE, a1);
            FILE *fp = fopen(path, "rb");
            if (!fp) { send_response(fd, "ERR 005 FILE_NOT_FOUND"); continue; }

            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char hdr[512];
            snprintf(hdr, sizeof(hdr), "OK FILE_SEND %s %ld SID:%s\n", a1, fsize, SID);
            send(fd, hdr, strlen(hdr), 0);

            char buf[4096]; int n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
                send(fd, buf, n, 0);
            fclose(fp);
            snprintf(logbuf, sizeof(logbuf), "GET %s (%ld bytes)", a1, fsize);
            log_event(logbuf);
            continue;
        }

        // ---- MONITOR ----
        if (strcmp(cmd, "MONITOR") == 0) {
            if (strcmp(a1, "START") == 0) {
                monitor_udp_port = atoi(a2);
                strncpy(monitor_client_ip, ip, sizeof(monitor_client_ip)-1);
                if (!monitor_active) {
                    monitor_active = 1;
                    pthread_create(&monitor_tid, NULL, monitor_loop, NULL);
                    pthread_detach(monitor_tid);
                }
                send_response(fd, "OK MONITOR_STARTED");
                log_event("MONITOR START");
            } else if (strcmp(a1, "STOP") == 0) {
                monitor_active = 0;
                send_response(fd, "OK MONITOR_STOPPED");
                log_event("MONITOR STOP");
            } else {
                send_response(fd, "ERR 999 UNKNOWN_COMMAND");
            }
            continue;
        }

        // ---- QUIT ----
        if (strcmp(cmd, "QUIT") == 0) {
            send_response(fd, "OK BYE");
            break;
        }

        send_response(fd, "ERR 999 UNKNOWN_COMMAND");
    }

    close(fd);
    snprintf(logbuf, sizeof(logbuf), "DISCONNECT %s", ip);
    log_event(logbuf);
    printf("[-] %s\n", logbuf);
}

// ---------- main ----------
int main(void) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(server_fd, 10);

    printf("RemoteOps Agent listening on port %d\n", PORT);
    log_event("AGENT STARTED");

    while (1) {
        client_t *cl = malloc(sizeof(client_t));
        socklen_t len = sizeof(cl->addr);
        cl->fd = accept(server_fd, (struct sockaddr*)&cl->addr, &len);
        if (cl->fd < 0) { free(cl); continue; }

        pthread_t tid;
        pthread_create(&tid, NULL, (void*(*)(void*))handle_client, cl);
        pthread_detach(tid);
    }
    close(server_fd);
    return 0;
}
