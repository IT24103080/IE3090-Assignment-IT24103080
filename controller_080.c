// controller_080.c - RemoteOps Controller
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT      9410
#define SERVER_IP "127.0.0.1"
#define SID       "0803"
#define TOKEN     "OPS-3080"

static volatile int udp_running = 0;
static pthread_t    udp_tid;

// ---------- UDP listener thread ----------
static void* udp_listener(void *arg) {
    int port = *(int*)arg;
    free(arg);

    int us = socket(AF_INET, SOCK_DGRAM, 0);
    if (us < 0) return NULL;
    int opt = 1;
    setsockopt(us, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(us, (struct sockaddr*)&a, sizeof(a)) < 0) {
        perror("udp bind"); close(us); return NULL;
    }
    printf("[UDP] listening on %d\n", port);

    char buf[1024];
    while (udp_running) {
        struct sockaddr_in src; socklen_t sl = sizeof(src);
        int n = recvfrom(us, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &sl);
        if (n > 0) { buf[n] = '\0'; printf("[UDP] %s", buf); }
    }
    close(us);
    return NULL;
}

// ---------- Read one TCP line ----------
static int read_line(int fd, char *out, int max) {
    int i = 0; char c;
    while (i < max-1) {
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') out[i++] = c;
    }
    out[i] = '\0';
    return i;
}

// ---------- Main ----------
int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &sa.sin_addr);

    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("connect"); return 1;
    }
    printf("Connected to Agent on %s:%d\n", SERVER_IP, PORT);
    printf("Commands: AUTH <token> | SYSINFO | LISTPROC | EXEC <name>\n");
    printf("          PUT <file> | GET <file> | MONITOR START <port> |\n");
    printf("          MONITOR STOP | QUIT\n\n");

    char line[1024];
    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;

        // ---- PUT: send header, then raw bytes ----
        if (strncmp(line, "PUT ", 4) == 0) {
            char fname[256]; long dummy;
            if (sscanf(line, "PUT %255s %ld", fname, &dummy) != 2) {
                printf("Usage: PUT <filename> <filesize>\n");
                continue;
            }
            FILE *fp = fopen(fname, "rb");
            if (!fp) { printf("Cannot open %s\n", fname); continue; }
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char hdr[512];
            snprintf(hdr, sizeof(hdr), "PUT %s %ld\n", fname, sz);
            send(sock, hdr, strlen(hdr), 0);

            char buf[4096]; int n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
                send(sock, buf, n, 0);
            fclose(fp);

            char resp[1024];
            int r = read_line(sock, resp, sizeof(resp));
            if (r >= 0) printf("< %s\n", resp);
            continue;
        }

        // ---- GET: send command, read header, then file ----
        if (strncmp(line, "GET ", 4) == 0) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "%s\n", line);
            send(sock, cmd, strlen(cmd), 0);

            char hdr[1024];
            int r = read_line(sock, hdr, sizeof(hdr));
            if (r < 0) { printf("Connection closed\n"); break; }
            printf("< %s\n", hdr);

            if (strncmp(hdr, "OK FILE_SEND", 12) == 0) {
                char fname[256]; long sz;
                if (sscanf(hdr, "OK FILE_SEND %255s %ld", fname, &sz) == 2) {
                    char outname[300];
                    snprintf(outname, sizeof(outname), "dl_%s", fname);
                    FILE *fp = fopen(outname, "wb");
                    long got = 0;
                    while (got < sz) {
                        char buf[4096];
                        int chunk = (sz - got) > 4096 ? 4096 : (int)(sz - got);
                        int n = recv(sock, buf, chunk, 0);
                        if (n <= 0) break;
                        fwrite(buf, 1, n, fp);
                        got += n;
                    }
                    fclose(fp);
                    printf("[saved as %s, %ld bytes]\n", outname, got);
                }
            }
            continue;
        }

        // ---- MONITOR START: also open UDP listener ----
        if (strncmp(line, "MONITOR START", 13) == 0) {
            int uport = 0;
            sscanf(line, "MONITOR START %d", &uport);
            if (uport > 0 && !udp_running) {
                udp_running = 1;
                int *p = malloc(sizeof(int)); *p = uport;
                pthread_create(&udp_tid, NULL, udp_listener, p);
                pthread_detach(udp_tid);
                sleep(1); // give it time to bind
            }
        }

        // Send command normally
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s\n", line);
        send(sock, cmd, strlen(cmd), 0);

        char resp[2048];
        int r = read_line(sock, resp, sizeof(resp));
        if (r < 0) { printf("Connection closed\n"); break; }
        printf("< %s\n", resp);

        if (strncmp(line, "QUIT", 4) == 0) break;
    }

    udp_running = 0;
    close(sock);
    return 0;
}
