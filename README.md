# lcx — 跨平台端口转发 / 端口反弹工具

Socket data transport tool，原作者 bkbll (bkbll@cnhonker.net)，代码来源于网络，长时间已找不到原始出处，故无法标注来源，向作者致谢致歉。

本版本把原先的 `linux_lcx.c` 与 `win_lcx.c` **合并为单一源文件 `code/lcx.c`**：用法、参数、输出、行为在 Windows 与 Linux 上完全一致，一份代码编出两个平台，从结构上杜绝「两个版本越改越不像」的问题。

---

## 一、版本产物（binary/）

| 文件 | 平台 | 架构 | 说明 |
|---|---|---|---|
| `lcx_win_x64.exe` | Windows 7 及以上 | x86_64 | 静态链接，无第三方 DLL 依赖 |
| `lcx_win_x86.exe` | Windows 7 及以上 | x86（32 位） | 静态链接，32 位系统可用 |
| `lcx_linux_x64` | Linux | x86_64 | 动态链接 glibc |
| `lcx_linux_x86` | Linux | x86（32 位） | 动态链接 glibc |
| `orig/linux_amd64_lcx` | Linux | x86_64 | 项目原始二进制（2017），仅存档 |

**Windows 兼容性**：编译时指定 `_WIN32_WINNT=0x0601`（Windows 7），静态链接，只依赖系统自带的 `KERNEL32.dll` / `msvcrt.dll` / `WS2_32.dll`，干净的 Win7 上可直接运行，无需安装任何运行库。需要更老的系统（如 XP）改源码里的 `_WIN32_WINNT` 后重编即可。

**控制台中文**：源码字符串统一为 UTF-8；Windows 上输出到控制台时自动转换成当前控制台代码页（中文系统为 GBK/cp936），帮助信息与状态输出在 cmd、PowerShell 下均正常显示，不会乱码；输出被重定向到文件时保持原生 UTF-8。

---

## 二、用法（两个平台完全相同）

```
lcx -m <模式> [-h1 <主机1>] -p1 <端口1> [-h2 <主机2>] -p2 <端口2> [选项]
```

### 模式（-m）

| 模式 | 含义 |
|---|---|
| `1` | 监听端口1，接受连接后转接到 HOST2:PORT2 并转发数据 |
| `2` | 同时监听端口1和端口2，在两端之间双向转发 |
| `3` | 同时连接 HOST1:PORT1 和 HOST2:PORT2，在两端之间双向转发 |

### 参数

| 参数 | 说明 |
|---|---|
| `-h1 <主机>` | 主机1（模式3必填） |
| `-p1 <端口>` | 端口1（必填，1~65535） |
| `-h2 <主机>` | 主机2（模式1、3必填） |
| `-p2 <端口>` | 端口2（必填，1~65535） |
| `-log <文件>` | 把转发的原始数据写入文件，并在屏幕打印流量统计 |
| `-v` | 显示版本号 |
| `-h`、`--help` | 显示帮助 |

### 示例

```bash
# 端口反弹：本机 5555 与 6666 两个端口互相转发
./lcx -m 2 -p1 5555 -p2 6666

# 端口转发：监听本机 5555，来连接转接到 192.168.1.10:6666
./lcx -m 1 -p1 5555 -h2 192.168.1.10 -p2 6666

# 双向连接：同时连上两个地址，在它们之间转发
./lcx -m 3 -h1 a.com -p1 52013 -h2 192.168.1.10 -p2 6666
```

退出方式：`Ctrl+C`（会打印退出提示并关闭日志文件）。

---

## 三、编译

```bash
# Linux x64
gcc lcx.c -o lcx_linux_x64 -O2 -Wall -Wextra -pthread

# Linux x86（需先安装 gcc-multilib）
gcc -m32 lcx.c -o lcx_linux_x86 -O2 -Wall -Wextra -pthread

# Windows x64（MinGW-w64 交叉编译，静态链接，目标 Win7）
x86_64-w64-mingw32-gcc lcx.c -o lcx_win_x64.exe \
    -O2 -Wall -Wextra -static -D_WIN32_WINNT=0x0601 -lws2_32

# Windows x86
i686-w64-mingw32-gcc lcx.c -o lcx_win_x86.exe \
    -O2 -Wall -Wextra -static -D_WIN32_WINNT=0x0601 -lws2_32
```

四个目标均以 `-Wall -Wextra` 编译且**零警告**。

---

## 四、本版修复（相对原 linux_lcx.c / win_lcx.c）

### 4.1 两版共通的缺陷

1. **`main` 隐式 int 返回类型** — 现代 C 标准下未定义，补全为 `int main`。
2. **`extern int errno;`** — 现代 glibc 中 `errno` 是宏（展开为 `__errno_location()`），直接 extern 声明会链接失败。改为正确使用 `<errno.h>`。
3. **`read()` 返回 -1 且 `errno==EINTR` 时直接 `memcpy(..., -1)`** — 用超大长度拷贝导致段错误。现已分别判断 `n>0` / `n==0` / 错误码。
4. **`bind2conn` 失败分支 `close(sockfd1)` 关闭未初始化的 fd** — 关掉随机句柄。现只关闭确实创建成功的套接字。
5. **`#define max(a,b) (a)>(b)?(a):(b)` 缺外层括号** — 展开产生意外优先级，已删除，改用显式比较表达式。
6. **信号处理函数签名不匹配** — `signal(SIGINT, &closeallfd)` 传入无参数函数。现用统一的 `void handler(int)`。
7. **`select()` 的 `timeval` 不重置** — Linux 上 select 会改写超时值，总超时被逐渐吃掉。现每轮循环重新赋值。
8. **非 void 函数执行到末尾无返回值** — `conn2conn` 等可能返回垃圾值，现全部改为 `void`。
9. **`closeallfd()` 遍历关闭 fd 3~256** — 关掉不属于自己的句柄（含日志文件）。进程退出时操作系统会回收全部套接字，无需手动遍历。
10. **`strcpy` / `sprintf` 潜在溢出** — 主机名缓冲区仅 40 字节（长域名被截断），改为 256 字节并统一使用 `snprintf`。
11. **`inet_ntoa()` 非线程安全** — 返回静态缓冲，多线程下互相覆盖，改用 `inet_ntop`。
12. **`create_socket()` 用 0 表示失败** — 0 是合法的 fd，改为 `SOCK_INVALID`。
13. **`accept()` 返回值判断 `<0`** — Windows 下 `SOCKET` 是无符号，`<0` 永远不成立，错误分支失效。统一改为 `== SOCK_INVALID`。
14. **端口未校验** — 原版对 0/65535 之外的端口不做检查（Linux 版完全没有校验）。现统一用 `strtol` 严格解析，范围 1~65535。
15. **未知参数被静默忽略** — 传错参数不报错。现报错并显示帮助，退出码非 0。
16. **`-log` 在 Windows 下用文本模式 `"a"` 打开** — 二进制数据中的 `\n` 会被改写成 `\r\n`，破坏数据原样。改为二进制追加 `"ab"`。
17. **日志文件被多线程并发写入** — 无锁导致内容交叉错乱，加互斥锁保护。

### 4.2 Windows 版特有

18. **`create_socket()` 返回 `int`，64 位下 `SOCKET` 被截断** — x64 构建必然出错。统一用 `sock_t`（Windows 下即 `SOCKET`）。
19. **`errno` / `EINTR` / `ENOSPC` 在 Winsock 中不成立** — Winsock 用 `WSAGetLastError()`，已抽象成 `NET_ERR()` 按平台取值。
20. **线程传参用栈上的 `transocket` 结构** — 下一轮 accept 会覆盖，原版靠 `Sleep(1000)` 硬扛竞态。改为堆分配 + 线程内释放。
21. **线程入口签名强转** — `void(LPVOID)` 被 cast 成 `LPTHREAD_START_ROUTINE`（应返回 `DWORD`），属未定义行为。现用统一 `void *(*)(void *)` 入口 + 平台适配层。
22. **`conn2conn` 内层重连使用已 `closesocket` 的 fd** — 死循环且句柄复用错误。改为每次重连前重新创建套接字。
23. **`l<=0` 分支漏关两个套接字** — 句柄泄漏，已补全关闭路径。
24. **`TerminateThread(NULL)`** — 对空句柄调用无意义且危险，已移除。
25. **`VOID main`** — 改为标准 `int main`。
26. **WSAStartup 版本 1.1** — 升级到 2.2。
27. **未使用的全局 `method` 与局部变量遮蔽** — 已清理。

### 4.3 Linux 版特有

28. **`SIGCLD`** — 改用可移植的 `SIGCHLD`（现由线程模型承担，无需忽略子进程）。
29. **未处理 `SIGPIPE`** — 对端已关闭时写入会让整个进程收到 SIGPIPE 而终止，现已忽略。
30. **`bzero()` 已废弃** — 改用 `memset`。
31. **`gethostbyname()` 不可重入** — 改用 `getaddrinfo()`。
32. **`fork()` 并发模型** — 与 Windows 线程模型分叉，是两版行为漂移的根源。统一改为线程（Windows `_beginthreadex` / POSIX `pthread_create`），转发核心逻辑因此完全共用一份代码。

### 4.4 行为统一

33. **模式3 语义统一** — 原 Windows 版要等 host1 先发数据才连 host2，若对端等待客户端先说话会永久挂起；原 Linux 版则立即连接。现两平台均为**立即建立两个连接后转发**，会话结束后自动重连。
34. **版本号统一** — 原 Linux `2.2` / Windows `1.00`，现统一为 `2.3`。
35. **输出风格统一** — 原 Linux 版裸文本、Windows 版带 `[+]`/`[-]` 前缀，现统一为带前缀格式，中文在两平台都正常显示。
36. **退出逻辑统一** — 原两版 `closeallfd` 调用条件不同，现统一走 `do_exit()`。
37. **主动连接增加超时** — 原版 `connect()` 阻塞，对端不可达时卡住 2 分钟以上，现改为非阻塞 connect + `select`，10 秒超时并提示。
38. **stdout 无缓冲** — 状态信息实时显示，重定向日志或进程被终止时不丢输出。
39. **会话半关闭处理** — 一端 EOF 后仍把缓冲区剩余数据发完再关闭，不再丢数据。

---

## 五、目录结构

```
lcx-master/
├── README.md
├── code/
│   ├── lcx.c              # 统一源码（两平台共用，日常编辑这个）
│   └── orig/              # 原始两版源码存档
│       ├── linux_lcx.c
│       └── win_lcx.c
└── binary/
    ├── lcx_win_x64.exe
    ├── lcx_win_x86.exe
    ├── lcx_linux_x64
    ├── lcx_linux_x86
    └── orig/
        └── linux_amd64_lcx
```

---

## 六、测试情况

四个目标均通过功能回归，共 **20/20 项通过**（Windows x64、Windows x86、Linux x64、Linux x86 各 5 项），覆盖：

- 模式1：监听 → 转接 → 双向回显
- 模式2：A→B、B→A 双向转发，300KB 大数据完整性
- 模式3：一端主动发包经 lcx 转发到另一端

测试环境为 Windows 10（本机）与 Kali Linux（交叉编译 + 实测），**Win7 实机未验证**——但已通过 `_WIN32_WINNT=0x0601`、静态链接、仅依赖系统 DLL 三项措施控制兼容性风险。
