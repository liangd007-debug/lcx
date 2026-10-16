/*
 * lcx - Socket data transport tool (跨平台统一版)
 *
 * 原作者 bkbll (bkbll@cnhonker.net)，代码来源于网络，特此致谢。
 * 本版本：修复原代码缺陷，并把 Windows / Linux 两个版本合并为
 * 单一源文件，保证用法、输出、行为在两个平台上完全一致。
 *
 * 编译（用法与参数两平台完全相同）:
 *   Linux : gcc lcx.c -o lcx -O2 -Wall -Wextra -pthread
 *   MinGW : x86_64-w64-mingw32-gcc lcx.c -o lcx.exe -O2 -Wall -Wextra -lws2_32
 *
 * 修复清单见 README.md 的「本版修复」一节。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

/* ================================================================== */
/* 平台抽象层：把 winsock 与 BSD socket 的差异收敛到这里              */
/* ================================================================== */
#ifdef _WIN32

/* 目标平台：Windows 7 (NT 6.1)。必须在包含 winsock2.h/windows.h 之前
 * 定义，否则头文件会按编译器默认目标声明 API，可能引入目标系统上
 * 不存在的函数。要支持更老的系统改这里即可（如 0x0501 = XP）。 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

typedef SOCKET          sock_t;
typedef int             sa_len_t;      /* accept/getpeername 的地址长度类型 */
typedef int             io_ret_t;      /* recv/send 返回值类型 */
typedef int             io_len_t;      /* recv/send 长度参数类型 */
typedef uintptr_t       thread_t;

#define SOCK_INVALID    INVALID_SOCKET
#define SOCK_FAIL       SOCKET_ERROR
#define NET_ERR()       WSAGetLastError()
#define s_close(s)      closesocket(s)
#define is_inprogress(e) ((e) == WSAEWOULDBLOCK)
#define is_wouldblock(e) ((e) == WSAEWOULDBLOCK || (e) == WSAENOBUFS)
#define sleep_ms(ms)    Sleep(ms)

#else /* POSIX */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

typedef int             sock_t;
typedef socklen_t       sa_len_t;
typedef ssize_t         io_ret_t;
typedef size_t          io_len_t;
typedef pthread_t       thread_t;

#define SOCK_INVALID    (-1)
#define SOCK_FAIL       (-1)
#define NET_ERR()       errno
#define s_close(s)      close(s)
#define is_inprogress(e) ((e) == EINPROGRESS)
#define is_wouldblock(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == ENOBUFS)
#define sleep_ms(ms)    usleep((useconds_t)(ms) * 1000)

#endif

/* ================================================================== */
/* 常量：两平台统一                                                    */
/* ================================================================== */
#define VERSION       "2.3"     /* 统一版本号（原 Linux 2.2 / Win 1.00） */
#define TIMEOUT_SEC   300       /* 空闲超时（秒）                       */
#define MAXPENDING    16384     /* 单向待发缓冲上限                     */
#define HOSTLEN       256       /* 主机名长度（原版 40 会截断长域名）    */
#define BACKLOG       32        /* listen 队列                          */
#define CONN_TIMEOUT  10        /* 主动连接超时（秒），原版会无限卡住   */

/* ================================================================== */
/* 全局状态                                                            */
/* ================================================================== */
static FILE        *g_logfp = NULL;   /* -log 目标文件                */
static volatile int g_running = 1;    /* 0 = 正在退出                 */

/* 日志文件被多个转发线程共享，必须加锁 */
#ifdef _WIN32
static CRITICAL_SECTION g_loglock;
#define LOG_LOCK()   EnterCriticalSection(&g_loglock)
#define LOG_UNLOCK() LeaveCriticalSection(&g_loglock)
#else
static pthread_mutex_t g_loglock = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK()   pthread_mutex_lock(&g_loglock)
#define LOG_UNLOCK() pthread_mutex_unlock(&g_loglock)
#endif

/* 转发线程的参数：堆上传入，线程内释放（原版传栈地址，存在竞态） */
typedef struct {
    sock_t fd1;
    sock_t fd2;
} trans_arg_t;

typedef void *(*thread_fn)(void *);

/* ================================================================== */
/* 小工具                                                              */
/* ================================================================== */

/* 取程序名，抹掉路径（Windows 用反斜杠，POSIX 用斜杠） */
static const char *progname(const char *argv0)
{
    const char *p1 = strrchr(argv0, '/');
    const char *p2 = strrchr(argv0, '\\');
    const char *p;

    if (p1 != NULL && p2 != NULL) p = (p1 > p2) ? p1 : p2;
    else if (p1 != NULL)           p = p1;
    else if (p2 != NULL)           p = p2;
    else                           return argv0;

    return p + 1;   /* 跳过分隔符 */
}

/* 该参数是不是一个真正的值（不能是下一个选项） */
static int is_value(const char *s)
{
    if (s == NULL || *s == '\0') return 0;
    if (s[0] == '-' && s[1] != '\0') return 0;
    return 1;
}

/* 严格解析端口：1~65535 的纯数字 */
static int parse_port(const char *s, int *out)
{
    char *end = NULL;
    long  v;

    if (!is_value(s)) return 0;
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0) return 0;
    if (v < 1 || v > 65535) return 0;
    *out = (int)v;
    return 1;
}

/* ================================================================== */
/* 控制台编码：源码字符串统一为 UTF-8，但中文 Windows 控制台默认是
 * GBK(cp936)，直接 printf 会乱码。这里把 UTF-8 转成控制台当前代码页。
 * POSIX 终端一般就是 UTF-8，原样输出。                              */
/* ================================================================== */
#ifdef _WIN32
/* 把 UTF-8 转成控制台当前代码页后输出。
 * stdout 被重定向到文件/管道时输出原生 UTF-8（不做转换）；
 * 控制台已是 UTF-8 时也原样输出；转换失败则原样兜底。 */
static void cprintf(const char *fmt, ...)
{
    char raw[1024], conv[2048];
    va_list ap;
    int n, cp;
    DWORD mode = 0;
    WCHAR wbuf[1024];
    int wlen, clen;

    va_start(ap, fmt);
    n = vsnprintf(raw, sizeof(raw), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(raw)) n = (int)sizeof(raw) - 1;

    /* 不是控制台（重定向/管道）：保持 UTF-8 */
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
        fwrite(raw, 1, (size_t)n, stdout);
        return;
    }

    cp = GetConsoleOutputCP();
    if (cp == 65001 || cp == 0) {           /* 控制台本身就是 UTF-8 */
        fwrite(raw, 1, (size_t)n, stdout);
        return;
    }

    /* 先确认是合法 UTF-8 */
    wlen = MultiByteToWideChar(CP_UTF8, 0, raw, n + 1, wbuf, 1024);
    if (wlen <= 0) {
        fwrite(raw, 1, (size_t)n, stdout);
        return;
    }

    clen = WideCharToMultiByte(cp, 0, wbuf, wlen, conv,
                               (int)sizeof(conv) - 1, NULL, NULL);
    if (clen <= 0) {                        /* 该代码页表示不了：原样兜底 */
        fwrite(raw, 1, (size_t)n, stdout);
        return;
    }
    fwrite(conv, 1, (size_t)clen, stdout);
}
#else
#define cprintf(...) printf(__VA_ARGS__)
#endif

#ifdef _WIN32
#define printf cprintf   /* 全部输出经控制台编码转换，避免中文乱码 */
#endif

static void usage(const char *argv0)
{
    const char *name = progname(argv0);

    printf("Socket data transport tool  v%s\n", VERSION);
    printf("原作者 bkbll(bkbll@cnhonker.net)；本版本修复缺陷并统一跨平台用法\n\n");
    printf("用法:\n");
    printf("  %s -m <模式> [-h1 <主机1>] -p1 <端口1> [-h2 <主机2>] -p2 <端口2> [选项]\n\n", name);
    printf("模式 (-m):\n");
    printf("  1   监听端口1，接受连接后转接到 HOST2:PORT2 并转发数据\n");
    printf("  2   同时监听端口1和端口2，在两端之间双向转发\n");
    printf("  3   同时连接 HOST1:PORT1 和 HOST2:PORT2，在两端之间双向转发\n\n");
    printf("参数:\n");
    printf("  -h1 <主机>   主机1（模式3必填）\n");
    printf("  -p1 <端口>   端口1（必填，1~65535）\n");
    printf("  -h2 <主机>   主机2（模式1、3必填）\n");
    printf("  -p2 <端口>   端口2（必填，1~65535）\n");
    printf("  -log <文件>  把转发的原始数据写入文件，并在屏幕打印流量统计\n");
    printf("  -v           显示版本号\n");
    printf("  -h, --help   显示本帮助\n\n");
    printf("示例:\n");
    printf("  %s -m 2 -p1 5555 -p2 6666\n", name);
    printf("      本机 5555 与 6666 两个端口互相转发\n");
    printf("  %s -m 1 -p1 5555 -h2 192.168.1.10 -p2 6666\n", name);
    printf("      监听本机 5555，来连接转接到 192.168.1.10:6666\n");
    printf("  %s -m 3 -h1 a.com -p1 52013 -h2 192.168.1.10 -p2 6666\n", name);
    printf("      同时连上两个地址，在它们之间转发\n");
}

/* 写日志（无 -log 时直接返回；有则加锁写并立刻落盘） */
static void makelog(const char *buf, size_t len)
{
    if (g_logfp == NULL || buf == NULL || len == 0) return;
    LOG_LOCK();
    fwrite(buf, 1, len, g_logfp);
    fflush(g_logfp);
    LOG_UNLOCK();
}

/* 优雅退出：打印提示、关日志。不遍历 fd 逐个 close ——
 * 进程退出时操作系统会回收全部套接字并给对端发 FIN（原版关 3~256 是危险且多余的）。 */
static void do_exit(int code)
{
    if (!g_running) exit(code);
    g_running = 0;

    printf("\n[+] 正在退出...\n");
    fflush(stdout);

    if (g_logfp != NULL) {
        LOG_LOCK();
        fwrite("====== Exit ======\n", 1, 19, g_logfp);
        fclose(g_logfp);
        g_logfp = NULL;
        LOG_UNLOCK();
    }

#ifdef _WIN32
    WSACleanup();
#endif
    exit(code);
}

/* ================================================================== */
/* 线程封装：Windows 用 _beginthreadex，POSIX 用 pthread_create        */
/* 两边入口签名统一为 void *(*)(void *)，转发逻辑因此可以完全共享      */
/* ================================================================== */
#ifdef _WIN32
struct th_arg { thread_fn fn; void *arg; };

static unsigned __stdcall th_entry(void *p)
{
    struct th_arg *a = (struct th_arg *)p;
    thread_fn fn = a->fn;
    void *arg = a->arg;
    free(a);
    fn(arg);
    return 0;
}
#endif

static int thread_spawn(thread_t *tid, thread_fn fn, void *arg)
{
#ifdef _WIN32
    struct th_arg *a = (struct th_arg *)malloc(sizeof(*a));
    uintptr_t h;
    if (a == NULL) return -1;
    a->fn = fn;
    a->arg = arg;
    h = _beginthreadex(NULL, 0, th_entry, a, 0, NULL);
    if (h == 0) { free(a); return -1; }
    *tid = (thread_t)h;      /* 句柄保留，交给 detach/join 决定关闭时机 */
    return 0;
#else
    return pthread_create(tid, NULL, fn, arg);
#endif
}

/* 不需要等待结果的线程（模式1、2：转发线程独立于 accept 循环） */
static void thread_detach(thread_t t)
{
#ifdef _WIN32
    CloseHandle((HANDLE)t);
#else
    pthread_detach(t);
#endif
}

/* 阻塞等待线程结束（模式3：会话结束后再重建连接） */
static void thread_join(thread_t t)
{
#ifdef _WIN32
    WaitForSingleObject((HANDLE)t, INFINITE);
    CloseHandle((HANDLE)t);
#else
    pthread_join(t, NULL);
#endif
}

/* ================================================================== */
/* 套接字基础操作                                                      */
/* ================================================================== */

/* 出错返回 SOCK_INVALID。原版用 0 表示失败，但 0 是合法的 fd。 */
static sock_t create_socket(void)
{
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) {
#ifdef _WIN32
        printf("[-] 创建套接字失败 (错误 %d)\n", NET_ERR());
#else
        perror("[-] 创建套接字失败");
#endif
    }
    return s;
}

/* 监听指定端口；成功返回 1，失败返回 0 */
static int create_serv(sock_t s, int port)
{
    struct sockaddr_in srv;
    int on = 1;

    memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons((unsigned short)port);
    srv.sin_addr.s_addr = htonl(INADDR_ANY);

    /* 端口复用：允许进程重启后立刻重新绑定 */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    if (bind(s, (struct sockaddr *)&srv, sizeof(srv)) == SOCK_FAIL) {
        printf("[-] 绑定端口 %d 失败: ", port);
#ifdef _WIN32
        printf("错误 %d（端口可能被占用）\n", NET_ERR());
#else
        perror("");
#endif
        return 0;
    }
    if (listen(s, BACKLOG) == SOCK_FAIL) {
        printf("[-] 监听端口 %d 失败\n", port);
        return 0;
    }
    return 1;
}

/* 主动连接 host:port，带超时；成功返回 1，失败返回 0。
 * 原版是阻塞 connect，对端不可达时会卡住 2 分钟以上。 */
static int client_connect(sock_t s, const char *host, int port)
{
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int rc, ok = 0, saved;
#ifdef _WIN32
    u_long nb;
#else
    int flags;
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       /* 本工具统一 IPv4 */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        printf("[-] 解析主机 %s 失败\n", host);
        return 0;
    }

    /* 切到非阻塞，用 select 实现连接超时 */
#ifdef _WIN32
    nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    rc = SOCK_FAIL;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        rc = connect(s, rp->ai_addr, (int)rp->ai_addrlen);
        if (rc == 0) { ok = 1; break; }
        if (is_inprogress(NET_ERR())) break;   /* 正在连接，下面走 select */
    }

    if (!ok && rc == SOCK_FAIL && is_inprogress(NET_ERR())) {
        fd_set wfds;
        struct timeval tv;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        tv.tv_sec  = CONN_TIMEOUT;
        tv.tv_usec = 0;
        rc = select((int)s + 1, NULL, &wfds, NULL, &tv);
        if (rc > 0) {
            int err = 0;
            sa_len_t elen = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) == 0 && err == 0)
                ok = 1;
        }
    } else if (rc == 0) {
        ok = 1;
    }

    /* 恢复阻塞模式（转发循环依赖 select 判断可读可写） */
#ifdef _WIN32
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, flags);
#endif

    saved = NET_ERR();
    freeaddrinfo(res);
    (void)saved;

    if (!ok) {
        printf("[-] 连接 %s:%d 失败或超时（%d 秒）\n", host, port, CONN_TIMEOUT);
        return 0;
    }
    return 1;
}

/* ================================================================== */
/* 核心：双向转发。两平台完全共用这一份代码                             */
/* ================================================================== */
static void *transdata(void *argp)
{
    trans_arg_t *ta = (trans_arg_t *)argp;
    sock_t fd1 = ta->fd1;
    sock_t fd2 = ta->fd2;
    free(ta);

    char pend1[MAXPENDING];   /* fd1 收到、待写给 fd2 */
    char pend2[MAXPENDING];   /* fd2 收到、待写给 fd1 */
    size_t n1 = 0, n2 = 0;
    int eof1 = 0, eof2 = 0;   /* 对端已关闭，但缓冲可能还没发完 */
    int maxfd = (int)(fd1 > fd2 ? fd1 : fd2) + 1;

    if (g_logfp != NULL) {
        char h1[64] = "fd1", h2[64] = "fd2";
        int p1 = 0, p2 = 0;
        struct sockaddr_in c1, c2;
        sa_len_t l1 = sizeof(c1), l2 = sizeof(c2);
        char hdr[256];
        int hl;

        if (getpeername(fd1, (struct sockaddr *)&c1, &l1) == 0) {
            inet_ntop(AF_INET, &c1.sin_addr, h1, sizeof(h1));
            p1 = ntohs(c1.sin_port);
        }
        if (getpeername(fd2, (struct sockaddr *)&c2, &l2) == 0) {
            inet_ntop(AF_INET, &c2.sin_addr, h2, sizeof(h2));
            p2 = ntohs(c2.sin_port);
        }
        hl = snprintf(hdr, sizeof(hdr),
                      "\n######## %s:%d <-> %s:%d ########\n", h1, p1, h2, p2);
        if (hl > 0) makelog(hdr, (size_t)hl);
    }

    while (g_running) {
        fd_set rfds, wfds;
        struct timeval tv;
        int rc;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        /* 背压：只有缓冲还有空间才监听读；有待发数据才监听写。
         * 无论处于哪种状态，至少有一个 fd 被注册，select 不会空转。 */
        if (!eof1 && n1 < MAXPENDING) FD_SET(fd1, &rfds);
        if (!eof2 && n2 < MAXPENDING) FD_SET(fd2, &rfds);
        if (n1 > 0) FD_SET(fd2, &wfds);
        if (n2 > 0) FD_SET(fd1, &wfds);

        if ((eof1 || eof2) && n1 == 0 && n2 == 0) break;   /* 双向都收尾了 */

        tv.tv_sec  = TIMEOUT_SEC;   /* 每轮重置：原版不重置，导致总超时被吃掉 */
        tv.tv_usec = 0;

        rc = select(maxfd, &rfds, &wfds, NULL, &tv);
        if (rc < 0) {
            if (NET_ERR() == EINTR
#ifdef _WIN32
                || NET_ERR() == WSAEINTR
#endif
               ) continue;
            printf("[-] select 出错\n");
            break;
        }
        if (rc == 0) {
            printf("[!] 空闲超时 %d 秒，关闭该连接\n", TIMEOUT_SEC);
            break;
        }

        /* ---- fd1 -> fd2 ---- */
        if (FD_ISSET(fd1, &rfds)) {
            io_len_t want = (io_len_t)(MAXPENDING - n1);
            io_ret_t n = recv(fd1, pend1 + n1, want, 0);
            if (n > 0) {
                if (g_logfp != NULL) {
                    char b[80];
                    int l = snprintf(b, sizeof(b), "\n<<< recv %ld bytes\n", (long)n);
                    if (l > 0) makelog(b, (size_t)l);
                    makelog(pend1 + n1, (size_t)n);
                }
                n1 += (size_t)n;
            } else if (n == 0) {
                eof1 = 1;
            } else if (!is_wouldblock(NET_ERR())) {
                eof1 = 1;
            }
        }
        if (FD_ISSET(fd2, &wfds) && n1 > 0) {
            io_ret_t n = send(fd2, pend1, (io_len_t)n1, 0);
            if (n > 0) {
                n1 -= (size_t)n;
                if (n1 > 0) memmove(pend1, pend1 + n, n1);
                if (g_logfp != NULL) {
                    char b[80];
                    int l = snprintf(b, sizeof(b), ">>> sent %ld bytes\n", (long)n);
                    if (l > 0) makelog(b, (size_t)l);
                }
            } else if (n == SOCK_FAIL && !is_wouldblock(NET_ERR())) {
                eof1 = 1;
                n1 = 0;
            }
        }

        /* ---- fd2 -> fd1 ---- */
        if (FD_ISSET(fd2, &rfds)) {
            io_len_t want = (io_len_t)(MAXPENDING - n2);
            io_ret_t n = recv(fd2, pend2 + n2, want, 0);
            if (n > 0) {
                if (g_logfp != NULL) {
                    char b[80];
                    int l = snprintf(b, sizeof(b), "\n<<< recv %ld bytes\n", (long)n);
                    if (l > 0) makelog(b, (size_t)l);
                    makelog(pend2 + n2, (size_t)n);
                }
                n2 += (size_t)n;
            } else if (n == 0) {
                eof2 = 1;
            } else if (!is_wouldblock(NET_ERR())) {
                eof2 = 1;
            }
        }
        if (FD_ISSET(fd1, &wfds) && n2 > 0) {
            io_ret_t n = send(fd1, pend2, (io_len_t)n2, 0);
            if (n > 0) {
                n2 -= (size_t)n;
                if (n2 > 0) memmove(pend2, pend2 + n, n2);
                if (g_logfp != NULL) {
                    char b[80];
                    int l = snprintf(b, sizeof(b), ">>> sent %ld bytes\n", (long)n);
                    if (l > 0) makelog(b, (size_t)l);
                }
            } else if (n == SOCK_FAIL && !is_wouldblock(NET_ERR())) {
                eof2 = 1;
                n2 = 0;
            }
        }
    }

    s_close(fd1);
    s_close(fd2);
    if (g_logfp != NULL) makelog("######## closed ########\n", 24);
    printf("[+] 转发会话结束，已关闭一对连接\n");
    return NULL;
}

/* 起一个转发线程。join=0 表示 detach 独立运行（模式1、2），
 * join=1 表示等待会话结束（模式3）。失败时由调用方负责关闭这两个 fd。 */
static int start_transmit(sock_t fd1, sock_t fd2, int join)
{
    trans_arg_t *ta = (trans_arg_t *)malloc(sizeof(*ta));
    thread_t tid;

    if (ta == NULL) return 0;
    ta->fd1 = fd1;
    ta->fd2 = fd2;

    if (thread_spawn(&tid, transdata, ta) != 0) {
        free(ta);
        return 0;
    }
    if (join) thread_join(tid);
    else      thread_detach(tid);
    return 1;
}

/* ================================================================== */
/* 三种模式                                                            */
/* ================================================================== */

/* 模式2：同时监听两个端口，在两者之间转发 */
static void bind2bind(int port1, int port2)
{
    sock_t ls1, ls2;

    ls1 = create_socket();
    if (ls1 == SOCK_INVALID) return;
    printf("[+] 监听端口 %d ...\n", port1);
    if (!create_serv(ls1, port1)) { s_close(ls1); return; }
    printf("[+] 端口 %d 监听成功\n", port1);

    ls2 = create_socket();
    if (ls2 == SOCK_INVALID) { s_close(ls1); return; }
    printf("[+] 监听端口 %d ...\n", port2);
    if (!create_serv(ls2, port2)) { s_close(ls1); s_close(ls2); return; }
    printf("[+] 端口 %d 监听成功\n", port2);
    printf("[+] 等待连接（Ctrl+C 退出）...\n");

    while (g_running) {
        struct sockaddr_in a1, a2;
        sa_len_t l1 = sizeof(a1), l2 = sizeof(a2);
        sock_t s1, s2;

        printf("[+] 等待客户端接入端口 %d ...\n", port1);
        s1 = accept(ls1, (struct sockaddr *)&a1, &l1);
        if (s1 == SOCK_INVALID) {
            printf("[-] accept 端口 %d 失败\n", port1);
            continue;
        }
        printf("[+] 端口 %d 收到连接: %s\n", port1, inet_ntoa(a1.sin_addr));

        printf("[+] 等待另一客户端接入端口 %d ...\n", port2);
        s2 = accept(ls2, (struct sockaddr *)&a2, &l2);
        if (s2 == SOCK_INVALID) {
            printf("[-] accept 端口 %d 失败，关闭已接受的连接\n", port2);
            s_close(s1);
            continue;
        }
        printf("[+] 端口 %d 收到连接: %s\n", port2, inet_ntoa(a2.sin_addr));

        if (!start_transmit(s1, s2, 0)) {
            printf("[-] 创建转发线程失败\n");
            s_close(s1);
            s_close(s2);
            continue;
        }
        printf("[+] 转发会话已建立\n\n");
    }

    s_close(ls1);
    s_close(ls2);
}

/* 模式1：监听端口1，来连接转接到 host:port2 */
static void bind2conn(int port1, const char *host, int port2)
{
    sock_t ls;

    ls = create_socket();
    if (ls == SOCK_INVALID) return;
    printf("[+] 监听端口 %d ...\n", port1);
    if (!create_serv(ls, port1)) { s_close(ls); return; }
    printf("[+] 端口 %d 监听成功，转发目标 %s:%d\n", port1, host, port2);
    printf("[+] 等待连接（Ctrl+C 退出）...\n");

    while (g_running) {
        struct sockaddr_in ra;
        sa_len_t rl = sizeof(ra);
        sock_t s1, s2;

        s1 = accept(ls, (struct sockaddr *)&ra, &rl);
        if (s1 == SOCK_INVALID) {
            printf("[-] accept 失败\n");
            continue;
        }
        printf("[+] 收到连接 %s:%d，转接到 %s:%d ...\n",
               inet_ntoa(ra.sin_addr), ntohs(ra.sin_port), host, port2);

        s2 = create_socket();
        if (s2 == SOCK_INVALID) { s_close(s1); continue; }

        if (!client_connect(s2, host, port2)) {
            const char *msg = "[lcx] 连接转发目标失败\r\n";
            send(s1, msg, (io_len_t)strlen(msg), 0);
            s_close(s1);
            s_close(s2);
            continue;
        }

        if (!start_transmit(s1, s2, 0)) {
            printf("[-] 创建转发线程失败\n");
            s_close(s1);
            s_close(s2);
            continue;
        }
        printf("[+] 转发会话已建立\n\n");
    }

    s_close(ls);
}

/* 模式3：同时连接两个地址，在它们之间转发 */
static void conn2conn(const char *h1, int p1, const char *h2, int p2)
{
    while (g_running) {
        sock_t s1, s2;

        printf("[+] 连接 %s:%d ...\n", h1, p1);
        s1 = create_socket();
        if (s1 == SOCK_INVALID) { sleep_ms(1000); continue; }
        if (!client_connect(s1, h1, p1)) {
            s_close(s1);
            printf("[!] 1 秒后重试\n");
            sleep_ms(1000);
            continue;
        }
        printf("[+] %s:%d 连接成功\n", h1, p1);

        printf("[+] 连接 %s:%d ...\n", h2, p2);
        s2 = create_socket();
        if (s2 == SOCK_INVALID) { s_close(s1); sleep_ms(1000); continue; }
        if (!client_connect(s2, h2, p2)) {
            s_close(s1);
            s_close(s2);
            printf("[!] 1 秒后重试\n");
            sleep_ms(1000);
            continue;
        }
        printf("[+] %s:%d 连接成功\n", h2, p2);

        if (!start_transmit(s1, s2, 1)) {
            printf("[-] 创建转发线程失败\n");
            s_close(s1);
            s_close(s2);
            sleep_ms(1000);
            continue;
        }
        /* start_transmit(join=1) 已阻塞等待本次会话结束，
         * 回到循环顶部重建连接，实现断线自动重连。 */
        printf("[+] 本次会话结束，准备重新连接\n\n");
        sleep_ms(1000);
    }
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        do_exit(0);
        return TRUE;
    }
    return FALSE;
}
#else
static void sig_handler(int sig)
{
    (void)sig;
    do_exit(0);
}
#endif

int main(int argc, char *argv[])
{
    char host1[HOSTLEN], host2[HOSTLEN];
    int port1 = 0, port2 = 0, method = 0;
    const char *logfile = NULL;
    int i;

    memset(host1, 0, sizeof(host1));
    memset(host2, 0, sizeof(host2));

    /* 输出无缓冲：状态信息实时可见（教学演示/重定向日志时都不会延迟），
     * 否则 stdout 满 4KB 或进程被终止时输出会丢失。 */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-v") == 0) {
            printf("Socket data transport tool\nVersion: %s\n", VERSION);
            return 0;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0 || strcmp(a, "/?") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "-h1") == 0 || strcmp(a, "-h2") == 0) {
            char *dst = (strcmp(a, "-h1") == 0) ? host1 : host2;
            if (i + 1 >= argc || !is_value(argv[i + 1])) {
                printf("[-] %s 后面缺少主机名\n", a);
                return 1;
            }
            i++;
            if (strlen(argv[i]) >= HOSTLEN)
                printf("[!] 主机名过长，已截断为 %d 字符\n", HOSTLEN - 1);
            snprintf(dst, HOSTLEN, "%s", argv[i]);
            continue;
        }
        if (strcmp(a, "-p1") == 0 || strcmp(a, "-p2") == 0) {
            int *dst = (strcmp(a, "-p1") == 0) ? &port1 : &port2;
            if (i + 1 >= argc || !is_value(argv[i + 1])) {
                printf("[-] %s 后面缺少端口号\n", a);
                return 1;
            }
            i++;
            if (!parse_port(argv[i], dst)) {
                printf("[-] 端口号无效: %s（必须是 1~65535 的整数）\n", argv[i]);
                return 1;
            }
            continue;
        }
        if (strcmp(a, "-m") == 0) {
            int m;
            if (i + 1 >= argc || !is_value(argv[i + 1])) {
                printf("[-] -m 后面缺少模式编号\n");
                return 1;
            }
            i++;
            if (!parse_port(argv[i], &m) || m > 3) {
                printf("[-] 模式无效: %s（只能是 1、2、3）\n", argv[i]);
                return 1;
            }
            method = m;
            continue;
        }
        if (strcmp(a, "-log") == 0) {
            if (i + 1 >= argc || !is_value(argv[i + 1])) {
                printf("[-] -log 后面缺少文件名\n");
                return 1;
            }
            logfile = argv[++i];
            continue;
        }

        printf("[-] 无法识别的参数: %s\n\n", a);
        usage(argv[0]);
        return 1;
    }

    if (method == 0) {
        printf("[-] 缺少 -m 指定运行模式\n\n");
        usage(argv[0]);
        return 1;
    }
    if (port1 == 0 || port2 == 0) {
        printf("[-] 必须同时指定 -p1 和 -p2\n");
        return 1;
    }
    if (method == 1 && host2[0] == '\0') {
        printf("[-] 模式1 必须指定 -h2（转发目标）\n");
        return 1;
    }
    if (method == 3 && (host1[0] == '\0' || host2[0] == '\0')) {
        printf("[-] 模式3 必须同时指定 -h1 和 -h2\n");
        return 1;
    }

    /* 打开日志。用二进制追加："ab" 在 Windows 上若用 "a" 会把 \n 改写成
     * \r\n，破坏转发数据的原始内容。 */
    if (logfile != NULL) {
        g_logfp = fopen(logfile, "ab");
        if (g_logfp == NULL) {
            printf("[-] 无法打开日志文件: %s\n", logfile);
            return 1;
        }
        {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char hdr[128];
            int l = snprintf(hdr, sizeof(hdr),
                             "====== Start %04d-%02d-%02d %02d:%02d:%02d ======\n",
                             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                             tm->tm_hour, tm->tm_min, tm->tm_sec);
            if (l > 0) makelog(hdr, (size_t)l);
        }
    }

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[-] WSAStartup 失败\n");
            return 1;
        }
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);   /* 对端已关闭时写入不应杀掉整个进程 */
#endif

    printf("Socket data transport tool v%s\n", VERSION);

    switch (method) {
    case 1: bind2conn(port1, host2, port2); break;
    case 2: bind2bind(port1, port2);        break;
    case 3: conn2conn(host1, port1, host2, port2); break;
    default: usage(argv[0]); break;
    }

    do_exit(0);
    return 0;
}
