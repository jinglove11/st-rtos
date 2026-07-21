/**
 * @file shell.c
 * @brief 交互式 Shell — 通过串口终端与 RTOS 交互
 */

#include "shell.h"

#if SHELL_ENABLE

#include "kernel.h"
#include "mem.h"
#include "uart.h"
#include "hal.h"
#include "fault.h"
#include "device.h"
#include "endpoint.h"
#include "capability.h"
#include "task.h"
#include "nameserver.h"
#include "driver_proto.h"
#include "driver_registry.h"
#include "driver_client.h"
#include "driver_runtime.h"
#include "fs_proto.h"
#include "fs_runtime.h"
#include "supervisor.h"
#include <string.h>

#if TRACE_ENABLE
#include "trace.h"
#endif

#if KERN_TASK_STATS
#include "stats.h"
#endif

/*============================================================================
 * 常量
 *============================================================================*/

#define SHELL_LINE_MAX   128
#define SHELL_ARGV_MAX   8
#define SHELL_PROMPT     "my-rtos> "
#define SHELL_UART       BOARD_DEFAULT_UART
#define SVC_RUNTIME_DEFAULT_PERIOD 100U
#define SVC_RUNTIME_MAX_PERIOD     10000U
#define SVC_RUNTIME_PRIORITY       4U
#define SVC_RUNTIME_STACK_SIZE     768U

/*============================================================================
 * 输出辅助 (无 printf, 直接操作 UART)
 *============================================================================*/

static void sh_puts(const char *s) {
    uart_puts(SHELL_UART, s);
}

static void sh_putc(char c) {
    uart_putc(SHELL_UART, c);
}

static void sh_putdec(uint32_t v) {
    uart_putdec(SHELL_UART, v);
}

static void sh_puthex(uint32_t v) {
    uart_puthex(SHELL_UART, v);
}

/* 打印带宽度的十进制数 (右对齐, 空格填充) */
static void sh_putdec_width(uint32_t v, int width) {
    char buf[12];
    int i = 0;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        while (v > 0 && i < 11) {
            buf[i++] = '0' + (char)(v % 10);
            v /= 10;
        }
    }
    /* 右对齐 */
    for (int pad = width - i; pad > 0; pad--)
        sh_putc(' ');
    while (i > 0)
        sh_putc(buf[--i]);
}

/*============================================================================
 * 内置命令: help
 *============================================================================*/

/* 前向声明命令表 */
typedef struct {
    const char *name;
    const char *help;
    void (*func)(int argc, char **argv);
} shell_cmd_t;

static void cmd_help(int argc, char **argv);

/* 命令表在文件末尾定义, 这里声明外部引用 */
extern const shell_cmd_t cmd_table[];
extern const int cmd_count;

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Available commands:\r\n");
    for (int i = 0; i < cmd_count; i++) {
        sh_puts("  ");
        sh_puts(cmd_table[i].name);
        /* 对齐到 12 列 */
        int len = (int)strlen(cmd_table[i].name);
        for (int j = len; j < 12; j++) sh_putc(' ');
        sh_puts(cmd_table[i].help);
        sh_puts("\r\n");
    }
}

/*============================================================================
 * 内置命令: ls / cat — Phase D1: 走 fs_server IPC (不再直调内核 vfs_*)
 *============================================================================*/

/* shell_fs_cap:fs_server 的 service cap (cmd_fs 路径设置)。
 * 定义在这里供 cmd_ls/cmd_cat 提前使用。 */
static cap_id_t shell_fs_cap = KERN_INVALID_ID;

/* 获取 fs_server 的 service cap。
 * 返回 >0:可用的 ep cap;<=0:fs_server 未启动。 */
static cap_id_t shell_get_fs_cap(void) {
    return shell_fs_cap;
}

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";
    cap_id_t fs_cap = shell_get_fs_cap();
    if (fs_cap <= 0) {
        sh_puts("ls: fs_server not running (use 'fs start' first)\r\n");
        return;
    }

    /* stat 判断类型 (fs_server Phase B 自管路径解析) */
    vfs_stat_t st;
    int err = fs_stat((int)fs_cap, path, &st, 1000);
    if (err != KERN_OK) {
        sh_puts("ls: ");
        sh_puts(path);
        sh_puts(": No such file or directory\r\n");
        return;
    }

    /* 普通文件:直接打印元数据 */
    if (st.type != INODE_TYPE_DIR) {
        sh_putdec(st.ino);
        sh_puts(st.type == INODE_TYPE_CHRDEV ? "  c  " : "  -  ");
        sh_puts(path);
        sh_puts("\r\n");
        return;
    }

    /* 目录:open + readdir */
    int fd = fs_open((int)fs_cap, path, O_RDONLY, 1000);
    if (fd <= 0) {
        sh_puts("ls: ");
        sh_puts(path);
        sh_puts(": open failed\r\n");
        return;
    }

    /* fs_readdir 把结果放进 msg.path (名字) + msg.length (type) + result (ino)。
     * fs_readdir 客户端 API 用 dirent_t 接收。 */
    dirent_t entry;
    while (fs_readdir((int)fs_cap, fd, &entry, 1000) == KERN_OK) {
        sh_putdec_width(entry.ino, 6);
        sh_puts("  ");
        switch (entry.type) {
        case INODE_TYPE_DIR:   sh_putc('d'); break;
        case INODE_TYPE_FILE:  sh_putc('-'); break;
        case INODE_TYPE_CHRDEV:sh_putc('c'); break;
        default:               sh_putc('?'); break;
        }
        sh_puts("  ");
        sh_puts(entry.name);
        sh_puts("\r\n");
    }

    fs_close((int)fs_cap, fd, 1000);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        sh_puts("Usage: cat <file>\r\n");
        return;
    }

    cap_id_t fs_cap = shell_get_fs_cap();
    if (fs_cap <= 0) {
        sh_puts("cat: fs_server not running (use 'fs start' first)\r\n");
        return;
    }

    int fd = fs_open((int)fs_cap, argv[1], O_RDONLY, 1000);
    if (fd <= 0) {
        sh_puts("cat: ");
        sh_puts(argv[1]);
        sh_puts(": Cannot open\r\n");
        return;
    }

    char buf[56];  /* FS_PAYLOAD_MAX */
    int32_t n;
    while ((n = fs_read((int)fs_cap, fd, buf, sizeof(buf), 1000)) > 0) {
        for (int32_t i = 0; i < n; i++) {
            sh_putc(buf[i]);
        }
    }
    sh_puts("\r\n");

    fs_close((int)fs_cap, fd, 1000);
}

/*============================================================================
 * 内置命令: echo
 *============================================================================*/

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) sh_putc(' ');
        sh_puts(argv[i]);
    }
    sh_puts("\r\n");
}

/*============================================================================
 * 内置命令: clear
 *============================================================================*/

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("\033[2J\033[H");  /* ANSI: 清屏 + 光标归位 */
}

/*============================================================================
 * 内置命令: ps
 *============================================================================*/

static const char *state_str(task_state_t s) {
    switch (s) {
    case TASK_STATE_CREATED:    return "created";
    case TASK_STATE_READY:      return "ready";
    case TASK_STATE_RUNNING:    return "running";
    case TASK_STATE_BLOCKED:    return "blocked";
    case TASK_STATE_SUSPENDED:  return "suspended";
    case TASK_STATE_TERMINATED: return "terminated";
    default:                    return "?";
    }
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;

    sh_puts("  ID  NAME            PRI  STATE     STACK\r\n");
    sh_puts("  --- --------------- ---  --------- -----\r\n");

    for (int i = -1; i < KERNEL_MAX_TASKS; i++) {
        tcb_t *tcb = task_get_tcb((task_id_t)i);
        if (!tcb) continue;
        if (tcb->state == TASK_STATE_CREATED && i >= 0) continue;
        if (tcb->state == TASK_STATE_TERMINATED) continue;

        /* ID */
        sh_putc(' ');
        if (i < 0) {
            sh_puts(" -1");
        } else {
            sh_putdec_width((uint32_t)i, 3);
        }
        sh_puts("  ");

        /* NAME (15 chars) */
        const char *name = tcb->name;
        int len = (int)strlen(name);
        sh_puts(name);
        for (int j = len; j < 16; j++) sh_putc(' ');

        /* PRI */
        sh_putdec_width(tcb->priority, 3);
        sh_puts("  ");

        /* STATE */
        const char *st = state_str(tcb->state);
        sh_puts(st);
        for (int j = (int)strlen(st); j < 10; j++) sh_putc(' ');

        /* STACK usage (approximate: check magic byte) */
        uint8_t *stack = (uint8_t *)tcb->stack_base;
        uint32_t used = 0;
        for (uint32_t j = 0; j < tcb->stack_size; j++) {
            if (stack[j] != STACK_MAGIC_BYTE) {
                used = tcb->stack_size - j;
                break;
            }
        }
        sh_putdec(used);
        sh_putc('/');
        sh_putdec(tcb->stack_size);

        sh_puts("\r\n");
    }
}

/*============================================================================
 * 内置命令: free
 *============================================================================*/

static void cmd_free(int argc, char **argv) {
    (void)argc; (void)argv;

    mem_stats_t st = mem_get_stats();

    sh_puts("Heap memory:\r\n");
    sh_puts("  Total: ");
    sh_putdec((uint32_t)st.total_size);
    sh_puts(" bytes\r\n");

    sh_puts("  Used:  ");
    sh_putdec((uint32_t)st.used_size);
    sh_puts(" bytes\r\n");

    sh_puts("  Free:  ");
    sh_putdec((uint32_t)st.free_size);
    sh_puts(" bytes\r\n");

    sh_puts("  Peak:  ");
    sh_putdec((uint32_t)st.max_used);
    sh_puts(" bytes\r\n");

    sh_puts("  Allocs: ");
    sh_putdec(st.alloc_count);
    sh_puts("  Frees: ");
    sh_putdec(st.free_count);
    sh_puts("  Fails: ");
    sh_putdec(st.fail_count);
    sh_puts("  Live: ");
    sh_putdec(st.outstanding_allocs);
    sh_puts("\r\n");
}

/*============================================================================
 * 内置命令: hexdump
 *============================================================================*/

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    while (*s) {
        char c = *s++;
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else break;
    }
    return v;
}

static uint32_t parse_dec(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 3) {
        sh_puts("Usage: hexdump <addr> <len>\r\n");
        return;
    }

    uint32_t addr = parse_hex(argv[1]);
    uint32_t len  = parse_dec(argv[2]);
    if (len > 256) len = 256;

    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;

    for (uint32_t off = 0; off < len; off += 16) {
        /* 地址 */
        sh_puthex(addr + off);
        sh_puts("  ");

        /* 十六进制 */
        uint32_t row = (len - off > 16) ? 16 : (len - off);
        for (uint32_t i = 0; i < 16; i++) {
            if (i < row) {
                uint8_t byte = p[off + i];
                /* 手动打印两位 hex */
                char hi = "0123456789abcdef"[byte >> 4];
                char lo = "0123456789abcdef"[byte & 0x0f];
                sh_putc(hi);
                sh_putc(lo);
            } else {
                sh_puts("  ");
            }
            if (i == 7) sh_putc(' ');
            sh_putc(' ');
        }

        /* ASCII */
        sh_puts(" |");
        for (uint32_t i = 0; i < row; i++) {
            uint8_t c = p[off + i];
            sh_putc((c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        sh_puts("|\r\n");
    }
}

/*============================================================================
 * 内置命令: top
 *============================================================================*/

#if KERN_TASK_STATS

static void cmd_top(int argc, char **argv) {
    (void)argc; (void)argv;

    sh_puts("  ID  NAME            PRI  STATE      CPU%  STACK\r\n");
    sh_puts("  --- --------------- ---  ---------  ----  -----\r\n");

    for (int i = -1; i < KERNEL_MAX_TASKS; i++) {
        tcb_t *tcb = task_get_tcb((task_id_t)i);
        if (!tcb) continue;
        if (tcb->state == TASK_STATE_CREATED && i >= 0) continue;
        if (tcb->state == TASK_STATE_TERMINATED) continue;

        /* ID */
        sh_putc(' ');
        if (i < 0) {
            sh_puts(" -1");
        } else {
            sh_putdec_width((uint32_t)i, 3);
        }
        sh_puts("  ");

        /* NAME (15 chars) */
        const char *name = tcb->name;
        int len = (int)strlen(name);
        sh_puts(name);
        for (int j = len; j < 16; j++) sh_putc(' ');

        /* PRI */
        sh_putdec_width(tcb->priority, 3);
        sh_puts("  ");

        /* STATE */
        const char *st = state_str(tcb->state);
        sh_puts(st);
        for (int j = (int)strlen(st); j < 10; j++) sh_putc(' ');

        /* CPU% (万分比 → 百分比, 保留1位小数) */
        uint32_t pct_int = tcb->cpu_usage / 100;
        uint32_t pct_frac = tcb->cpu_usage % 100;
        sh_putdec_width(pct_int, 3);
        sh_putc('.');
        if (pct_frac < 10) sh_putc('0');
        sh_putdec(pct_frac);
        sh_putc('%');
        sh_puts("  ");

        /* STACK usage */
        uint8_t *stack = (uint8_t *)tcb->stack_base;
        uint32_t used = 0;
        for (uint32_t j = 0; j < tcb->stack_size; j++) {
            if (stack[j] != STACK_MAGIC_BYTE) {
                used = tcb->stack_size - j;
                break;
            }
        }
        sh_putdec(used);
        sh_putc('/');
        sh_putdec(tcb->stack_size);

        sh_puts("\r\n");
    }
}

#endif /* KERN_TASK_STATS */

/*============================================================================
 * 内置命令: crash
 *============================================================================*/

static void cmd_crash(int argc, char **argv) {
    (void)argc; (void)argv;

    if (crash_dump.fault_type == 0) {
        sh_puts("No crash recorded.\r\n");
        return;
    }

    sh_puts("=== Last Crash Dump ===\r\n");
    sh_puts("Fault Type: ");
    switch (crash_dump.fault_type) {
    case 1: sh_puts("MemManage"); break;
    case 2: sh_puts("BusFault");  break;
    case 3: sh_puts("UsageFault"); break;
    case 4: sh_puts("HardFault"); break;
    default: sh_puts("?"); break;
    }
    sh_puts(" (");
    sh_putdec(crash_dump.fault_type);
    sh_puts(")\r\n");

    sh_puts("Task ID:  ");
    sh_putdec((uint32_t)crash_dump.task_id);
    sh_puts("\r\n");

    sh_puts("PC:       0x");
    sh_puthex(crash_dump.pc);
    sh_puts("\r\nLR:       0x");
    sh_puthex(crash_dump.lr);
    sh_puts("\r\nR0:       0x");
    sh_puthex(crash_dump.r0);
    sh_puts("\r\nR1:       0x");
    sh_puthex(crash_dump.r1);
    sh_puts("\r\nR2:       0x");
    sh_puthex(crash_dump.r2);
    sh_puts("\r\nR3:       0x");
    sh_puthex(crash_dump.r3);
    sh_puts("\r\nR12:      0x");
    sh_puthex(crash_dump.r12);
    sh_puts("\r\nxPSR:     0x");
    sh_puthex(crash_dump.xpsr);
    sh_puts("\r\n");

    sh_puts("CFSR:     0x");
    sh_puthex(crash_dump.cfsr);
    sh_puts(" (MMFSR=");
    sh_puthex(crash_dump.cfsr & 0xFF);
    sh_puts(" BFSR=");
    sh_puthex((crash_dump.cfsr >> 8) & 0xFF);
    sh_puts(" UFSR=");
    sh_puthex((crash_dump.cfsr >> 16) & 0xFFFF);
    sh_puts(")\r\n");

    sh_puts("HFSR:     0x");
    sh_puthex(crash_dump.hfsr);
    sh_puts("\r\nMMFAR:    0x");
    sh_puthex(crash_dump.mmfar);
    sh_puts("\r\nBFAR:     0x");
    sh_puthex(crash_dump.bfar);
    sh_puts("\r\n");

    /* reserved[0] = CONTROL, [1] = MPU_CTRL, [2] = MPU_RBAR, [3] = MPU_RASR */
    sh_puts("CONTROL:  0x");
    sh_puthex(crash_dump.reserved[0]);
    sh_puts("\r\nMPU_CTRL: 0x");
    sh_puthex(crash_dump.reserved[1]);
    sh_puts("\r\nMPU_RBAR: 0x");
    sh_puthex(crash_dump.reserved[2]);
    sh_puts("\r\nMPU_RASR: 0x");
    sh_puthex(crash_dump.reserved[3]);
    sh_puts("\r\n");
}

/*============================================================================
 * 内置命令: stats
 *============================================================================*/

#if KERN_TASK_STATS

static const char *stats_subsys_name(uint8_t id) {
    switch (id) {
    case STATS_SUBSYS_TIMER: return "timer";
    case STATS_SUBSYS_IRQ:   return "irq";
    case STATS_SUBSYS_BH:    return "bh";
    case STATS_SUBSYS_DEV:   return "dev";
    case STATS_SUBSYS_MEM:   return "mem";
    case STATS_SUBSYS_IPC:   return "ipc";
    case STATS_SUBSYS_CAP:   return "cap";
    case STATS_SUBSYS_VFS:   return "vfs";
    default:                 return "?";
    }
}

static void stats_print_subsys(uint8_t id) {
    sh_puts("  ");
    sh_puts(stats_subsys_name(id));
    for (int i = (int)strlen(stats_subsys_name(id)); i < 6; i++) sh_putc(' ');
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_OK));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_ERROR));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_QUEUE_FULL));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_TIMEOUT));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_DELETE));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_CANCEL));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_BUSY));
    sh_puts("   ");
    sh_putdec(stats_get_event_count(id, STATS_COUNTER_NOEXIST));
    sh_puts("\r\n");
}

static void cmd_stats(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        stats_clear_events();
        sh_puts("Stats event counters cleared.\r\n");
        return;
    }

    sh_puts("=== Kernel Statistics ===\r\n");
    sh_puts("Uptime:          ");
    sh_putdec(stats_get_uptime());
    sh_puts(" ticks\r\n");

    sh_puts("Context Switches: ");
    sh_putdec(stats_get_ctx_switches());
    sh_puts("\r\n");

    const kern_stats_t *ks = stats_get_kern_stats();
    sh_puts("IRQs:            ");
    sh_putdec(ks->total_irqs);
    sh_puts("\r\n");

    sh_puts("Max IRQ Latency: ");
    sh_putdec(ks->irq_latency_max);
    sh_puts(" ticks\r\n");

    sh_puts("Faults:          ");
    sh_putdec(ks->fault_count);
    sh_puts("\r\n");

    sh_puts("Syscalls:        ");
    sh_putdec(ks->total_syscalls);
    sh_puts("\r\n");

    sh_puts("\r\nSubsystem Events:\r\n");
    sh_puts("  name   ok err full timeout del cancel busy noexist\r\n");
    for (uint8_t id = 0; id < STATS_SUBSYS_MAX; id++) {
        stats_print_subsys(id);
    }
}

#endif /* KERN_TASK_STATS */

/*============================================================================
 * 内置命令: mem
 *============================================================================*/

static void cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;

    mem_stats_t st = mem_get_stats();

    sh_puts("=== Memory ===\r\n");
    sh_puts("Heap:    ");
    sh_putdec((uint32_t)st.used_size);
    sh_putc('/');
    sh_putdec((uint32_t)st.total_size);
    sh_puts(" bytes (");
    if (st.total_size > 0) {
        sh_putdec(st.used_size * 100 / (uint32_t)st.total_size);
    } else {
        sh_putc('0');
    }
    sh_puts("%)\r\n");

    sh_puts("Peak:    ");
    sh_putdec((uint32_t)st.max_used);
    sh_puts(" bytes\r\n");

    sh_puts("Live:    ");
    sh_putdec(st.outstanding_allocs);
    sh_puts(" allocs\r\n");

    sh_puts("OOM:     ");
    sh_putdec(st.fail_count);
    sh_puts(" fails\r\n");

    sh_puts("BadFree: ");
    sh_putdec(st.invalid_free_count);
    sh_puts("\r\n");

    sh_puts("\r\nPer-task stack:\r\n");
    sh_puts("  ID  NAME            STACK USAGE\r\n");
    sh_puts("  --- --------------- ----------\r\n");

    for (int i = -1; i < KERNEL_MAX_TASKS; i++) {
        tcb_t *tcb = task_get_tcb((task_id_t)i);
        if (!tcb) continue;
        if (tcb->state == TASK_STATE_CREATED && i >= 0) continue;
        if (tcb->state == TASK_STATE_TERMINATED) continue;

        sh_puts("  ");
        if (i < 0) sh_puts("-1");
        else sh_putdec_width((uint32_t)i, 3);

        sh_puts("  ");
        sh_puts(tcb->name);
        int nl = (int)strlen(tcb->name);
        for (int j = nl; j < 16; j++) sh_putc(' ');

        uint8_t *stack = (uint8_t *)tcb->stack_base;
        uint32_t used = 0;
        for (uint32_t j = 0; j < tcb->stack_size; j++) {
            if (stack[j] != STACK_MAGIC_BYTE) {
                used = tcb->stack_size - j;
                break;
            }
        }
        sh_putdec(used);
        sh_putc('/');
        sh_putdec(tcb->stack_size);
        if (used * 100 / tcb->stack_size > 80) sh_puts(" *");
        sh_puts("\r\n");
    }
}

/*============================================================================
 * 内置命令: version
 *============================================================================*/

static void cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts(KERN_NAME " v");
    sh_putdec(KERN_VERSION_MAJOR);
    sh_putc('.');
    sh_putdec(KERN_VERSION_MINOR);
    sh_putc('.');
    sh_putdec(KERN_VERSION_PATCH);
    sh_puts("\r\n");
}

/*============================================================================
 * 内置命令: reset
 *============================================================================*/

static void cmd_reset(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_puts("Resetting...\r\n");
    hal_system_reset();
}

/*============================================================================
 * 内置命令: trace
 *============================================================================*/

#if TRACE_ENABLE

static const char *trace_event_name(uint8_t ev) {
    switch (ev) {
    case TRACE_TASK_SWITCH: return "SWITCH";
    case TRACE_ISR_ENTER:   return "ISR_IN";
    case TRACE_ISR_EXIT:    return "ISR_OUT";
    case TRACE_SYSCALL:     return "SYSCALL";
    case TRACE_IPC_SEND:    return "IPC_SND";
    case TRACE_IPC_RECV:    return "IPC_RCV";
    case TRACE_BH_SCHEDULE: return "BH";
    case TRACE_FAULT:       return "FAULT";
    case TRACE_TIMER:       return "TIMER";
    case TRACE_IRQ:         return "IRQ";
    case TRACE_BH:          return "BH2";
    case TRACE_DEV:         return "DEV";
    case TRACE_MEM:         return "MEM";
    case TRACE_IPC_EVENT:   return "IPC_EVT";
    case TRACE_CAP_EVENT:   return "CAP_EVT";
    case TRACE_VFS_EVENT:   return "VFS_EVT";
    default: return "?";
    }
}

static int8_t parse_trace_event(const char *name) {
    if      (strcmp(name, "switch")  == 0) return TRACE_TASK_SWITCH;
    else if (strcmp(name, "isr")     == 0) return TRACE_ISR_ENTER;
    else if (strcmp(name, "syscall") == 0) return TRACE_SYSCALL;
    else if (strcmp(name, "ipc")     == 0) return TRACE_IPC_SEND;
    else if (strcmp(name, "bh")      == 0) return TRACE_BH;
    else if (strcmp(name, "fault")   == 0) return TRACE_FAULT;
    else if (strcmp(name, "timer")   == 0) return TRACE_TIMER;
    else if (strcmp(name, "irq")     == 0) return TRACE_IRQ;
    else if (strcmp(name, "dev")     == 0) return TRACE_DEV;
    else if (strcmp(name, "mem")     == 0) return TRACE_MEM;
    else if (strcmp(name, "cap")     == 0) return TRACE_CAP_EVENT;
    else if (strcmp(name, "vfs")     == 0) return TRACE_VFS_EVENT;
    else return -1;
}

static void trace_print_entry(const trace_entry_t *e, void *ctx) {
    (void)ctx;
    sh_putc(' ');
    sh_putdec_width(e->tick, 8);
    sh_puts("  ");
    sh_puts(trace_event_name(e->event));
    for (int j = (int)strlen(trace_event_name(e->event)); j < 8; j++) sh_putc(' ');
    sh_puts("  ");
    sh_putdec_width(e->task_id, 4);
    sh_puts("  ");
    sh_putdec(e->data);
    sh_puts("\r\n");
}

static void cmd_trace(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        trace_clear();
        sh_puts("Trace cleared.\r\n");
        return;
    }

    int8_t filter = -1;
    uint16_t limit = 20;
    int argi = 1;

    if (argc > argi && argv[argi][0] >= '0' && argv[argi][0] <= '9') {
        uint32_t parsed = parse_dec(argv[argi]);
        if (parsed > 0) {
            limit = (parsed > TRACE_BUFFER_SIZE) ? TRACE_BUFFER_SIZE : (uint16_t)parsed;
        }
        argi++;
    }

    if (argc > argi) {
        filter = parse_trace_event(argv[argi]);
        if (filter < 0) {
            sh_puts("trace: unknown filter '");
            sh_puts(argv[argi]);
            sh_puts("' (try: switch/isr/syscall/ipc/bh/fault/timer/irq/dev/mem/cap/vfs)\r\n");
            return;
        }
    }

    uint16_t count = trace_get_count();
    sh_puts("Trace entries: ");
    sh_putdec(count);
    if (filter >= 0) {
        sh_puts(" (filtered)");
    }
    sh_puts("\r\n");

    sh_puts("  TICK     EVENT    TASK  DATA\r\n");
    sh_puts("  -------- -------- ----  ----\r\n");

    if (filter >= 0) {
        trace_filter((uint8_t)filter, trace_print_entry, NULL);
    } else {
        uint16_t start = (count > limit) ? (count - limit) : 0;
        for (uint16_t i = start; i < count; i++) {
            const trace_entry_t *e = trace_get_entry(i);
            if (!e) continue;
            trace_print_entry(e, NULL);
        }
    }
}

#endif /* TRACE_ENABLE */

/*============================================================================
 * 内置命令: dev
 *============================================================================*/

#if DRIVER_ENABLE

static const char *device_type_name(device_type_t type) {
    switch (type) {
    case DEVICE_TYPE_CHAR:  return "char";
    case DEVICE_TYPE_BLOCK: return "block";
    default:                return "?";
    }
}

static void cmd_dev(int argc, char **argv) {
    (void)argc; (void)argv;

    sh_puts("  ID  NAME            TYPE   OPEN IRQ\r\n");
    sh_puts("  --- --------------- ------ ---- ---\r\n");

    for (uint16_t i = 0; i < DEVICE_MAX; i++) {
        device_t *dev = device_get_by_index(i);
        if (!dev) continue;

        sh_putc(' ');
        sh_putdec_width(i, 3);
        sh_puts("  ");
        sh_puts(dev->name);
        for (int j = (int)strlen(dev->name); j < 16; j++) sh_putc(' ');

        const char *type = device_type_name(dev->type);
        sh_puts(type);
        for (int j = (int)strlen(type); j < 7; j++) sh_putc(' ');

        sh_putdec_width(dev->open_count, 4);
        sh_putc(' ');
        sh_putdec(dev->irq_num);
        sh_puts("\r\n");
    }
}

#endif /* DRIVER_ENABLE */

/*============================================================================
 * 内置命令: driver
 *============================================================================*/

#if DRIVER_ENABLE

#define DRIVER_SHELL_PROBE_TIMEOUT 100U
#define DRIVER_SHELL_SERVICE_NAME  "dev.uart0"

static task_id_t shell_driver_ns_task = KERN_INVALID_ID;
static ep_id_t shell_driver_ns_ep = KERN_INVALID_ID;
static cap_id_t shell_driver_ns_cap = KERN_INVALID_ID;
static task_id_t shell_driver_uart_task = KERN_INVALID_ID;
static ep_id_t shell_driver_uart_ep = KERN_INVALID_ID;
static cap_id_t shell_driver_uart_cap = KERN_INVALID_ID;
static supervisor_service_t *shell_driver_supervisor;

static supervisor_service_t *shell_driver_supervisor_get(void) {
    if (shell_driver_supervisor == NULL) {
        shell_driver_supervisor =
            supervisor_register_service(DRIVER_SHELL_SERVICE_NAME,
                                        KERN_ERR_STATE);
    }
    return shell_driver_supervisor;
}

static void shell_driver_nameserver_task(void *arg) {
    (void)nameserver_service_run((int)(uintptr_t)arg, 0);
}

static void shell_driver_uart_task_entry(void *arg) {
    (void)uart_server_run((int)(uintptr_t)arg, 0);
}

static void shell_put_flag(uint32_t value, uint32_t flag,
                           const char *name, int *first) {
    if ((value & flag) == 0U) {
        return;
    }
    if (!*first) {
        sh_putc('|');
    }
    sh_puts(name);
    *first = 0;
}

static void shell_put_driver_ops(uint32_t ops) {
    int first = 1;
    uint32_t bits[] = {
        DRIVER_OP_BIT_PING,
        DRIVER_OP_BIT_OPEN,
        DRIVER_OP_BIT_CLOSE,
        DRIVER_OP_BIT_READ,
        DRIVER_OP_BIT_WRITE,
        DRIVER_OP_BIT_IOCTL,
        DRIVER_OP_BIT_POLL,
        DRIVER_OP_BIT_ATTACH,
        DRIVER_OP_BIT_DETACH,
    };

    for (uint32_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        const char *name = driver_op_bit_name(bits[i]);
        if (name != NULL) {
            shell_put_flag(ops, bits[i], name, &first);
        }
    }
    if (first) {
        sh_puts("none");
    }
}

static void shell_put_driver_ioctls(uint32_t ioctls) {
    int first = 1;
    uint32_t bits[] = {
        DRIVER_IOCTL_BIT_GET_EVENTS,
        DRIVER_IOCTL_BIT_GET_RESOURCES,
        DRIVER_IOCTL_BIT_GET_STATUS,
        DRIVER_IOCTL_BIT_CLEAR_STATUS,
    };

    for (uint32_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        const char *name = driver_ioctl_bit_name(bits[i]);
        if (name != NULL) {
            shell_put_flag(ioctls, bits[i], name, &first);
        }
    }
    if (first) {
        sh_puts("none");
    }
}

static void shell_put_driver_resources(uint32_t resources) {
    int first = 1;
    uint32_t bits[] = {
        DRV_RESOURCE_BIT_MMIO,
        DRV_RESOURCE_BIT_IRQ,
    };

    for (uint32_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        const char *name = driver_resource_bit_name(bits[i]);
        if (name != NULL) {
            shell_put_flag(resources, bits[i], name, &first);
        }
    }
    if (first) {
        sh_puts("none");
    }
}

static void shell_put_driver_status_bits(uint32_t status) {
    int first = 1;
    uint32_t bits[] = {
        DRV_STATUS_OPEN,
        DRV_STATUS_MMIO_READY,
        DRV_STATUS_IRQ_BOUND,
        DRV_STATUS_IRQ_PENDING,
        DRV_STATUS_ERROR,
    };

    for (uint32_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        const char *name = driver_status_bit_name(bits[i]);
        if (name != NULL) {
            shell_put_flag(status, bits[i], name, &first);
        }
    }
    if (first) {
        sh_puts("none");
    }
}

static void cmd_driver_abi(void) {
    sh_puts("User driver ABI\r\n");
    sh_puts("  protocol magic: ");
    sh_puthex(DRV_MAGIC);
    sh_puts("\r\n");
    sh_puts("  payload max: ");
    sh_putdec(DRV_PAYLOAD_MAX);
    sh_puts(" bytes\r\n");
    sh_puts("  ops: ping open close read write ioctl poll attach detach\r\n");
    sh_puts("  ioctls: events resources status clear-status\r\n");

    uint32_t resources = DRV_RESOURCE_BIT_MMIO | DRV_RESOURCE_BIT_IRQ;
    sh_puts("  resources: ");
    shell_put_driver_resources(resources);
    sh_puts("\r\n");

    uint32_t status = DRV_STATUS_OPEN | DRV_STATUS_MMIO_READY |
                      DRV_STATUS_IRQ_BOUND | DRV_STATUS_IRQ_PENDING |
                      DRV_STATUS_ERROR;
    sh_puts("  status bits: ");
    shell_put_driver_status_bits(status);
    sh_puts("\r\n");
    sh_puts("  uart server: user-space service, debug UART path retained\r\n");
}

static void cmd_driver_print_desc(const driver_descriptor_t *desc) {
    sh_puts("  service: ");
    sh_puts(desc->service_name);
    sh_puts(" device=");
    sh_puts(desc->device_name);
    sh_puts("\r\n");
    sh_puts("    ops: ");
    shell_put_driver_ops(desc->ops);
    sh_puts("\r\n");
    sh_puts("    ioctls: ");
    shell_put_driver_ioctls(desc->ioctls);
    sh_puts("\r\n");
    sh_puts("    resources: ");
    shell_put_driver_resources(desc->resources);
    sh_puts("\r\n");
    sh_puts("    required: ");
    shell_put_driver_resources(desc->required_resources);
    sh_puts("\r\n");
    sh_puts("    optional: ");
    shell_put_driver_resources(desc->optional_resources);
    sh_puts("\r\n");
    sh_puts("    status bits: ");
    shell_put_driver_status_bits(desc->status_bits);
    sh_puts("\r\n");
}

static void cmd_driver_print_ns_state(const char *label,
                                      const char *live_word) {
    cap_id_t ns_cap = driver_runtime_name_server_cap();
    int err = KERN_OK;
    driver_runtime_ns_state_t state =
        driver_runtime_name_server_state(DRIVER_SHELL_PROBE_TIMEOUT, &err);

    sh_puts("  ");
    sh_puts(label);
    sh_puts(": ");
    if (state == DRIVER_RUNTIME_NS_LIVE) {
        sh_puts(live_word);
        sh_puts("\r\n");
    } else if (state == DRIVER_RUNTIME_NS_BOUND) {
        sh_puts("bound (");
        sh_puts(driver_error_name(err));
        sh_puts(")\r\n");
    } else {
        sh_puts("unbound (");
        sh_puts(driver_error_name(err));
        sh_puts(")\r\n");
    }

    sh_puts("  name-server cap: ");
    if (!driver_runtime_name_server_bound()) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)ns_cap);
        sh_puts("\r\n");
    }
}

static void cmd_driver_print_inbox_state(void) {
    sh_puts("  inbox cap: ");
    if (!driver_runtime_inbox_bound()) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)driver_runtime_inbox_cap());
        sh_puts(driver_runtime_inbox_owned() ? " (owned)" : " (external)");
        sh_puts("\r\n");
    }
}

static int cmd_driver_release_owned_inbox(void) {
    if (!driver_runtime_inbox_bound()) {
        return KERN_OK;
    }
    if (!driver_runtime_inbox_owned()) {
        driver_runtime_clear_inbox();
        return KERN_OK;
    }

    cap_id_t inbox_cap = driver_runtime_inbox_cap();
    void *obj = cap_resolve(inbox_cap, CAP_OBJ_ENDPOINT, CAP_MANAGE);
    if (obj != NULL) {
        ep_id_t ep_id = endpoint_id_from_obj(obj);  /* M2-Step3b */
        (void)endpoint_delete(ep_id);
    } else {
        cap_delete(inbox_cap);
    }
    driver_runtime_clear_inbox();
    return KERN_OK;
}

static void cmd_driver_print_lookup_status(void) {
    cmd_driver_print_ns_state("service lookup", "live");
    cmd_driver_print_inbox_state();
    sh_puts("  name-server task: ");
    if (shell_driver_ns_task < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_driver_ns_task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(shell_driver_ns_task)));
        sh_puts(")");
        sh_puts("\r\n");
    }
    sh_puts("  uart service task: ");
    if (shell_driver_uart_task < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_driver_uart_task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(shell_driver_uart_task)));
        sh_puts(")\r\n");
    }
    sh_puts("  restart count: ");
    supervisor_service_t *svc = shell_driver_supervisor_get();
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("\r\n");
    sh_puts("  recover count: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("\r\n");
    sh_puts("  fault count: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");
    sh_puts("  pending clients: ");
    sh_putdec(supervisor_pending_clients(svc));
    sh_puts("\r\n");
    sh_puts("  last health: ");
    sh_puts(driver_error_name(supervisor_last_health(svc)));
    sh_puts("\r\n");
}

static void cmd_driver_status(const char *service_name) {
    sh_puts("User driver status\r\n");
    if (service_name != NULL) {
        const driver_descriptor_t *desc = NULL;
        int err = driver_registry_query(service_name, &desc);
        if (err != KERN_OK || desc == NULL) {
            sh_puts("  service not found: ");
            sh_puts(service_name);
            sh_puts("\r\n");
            sh_puts("  reason: ");
            sh_puts(driver_error_name(err));
            sh_puts("\r\n");
            return;
        }
        cmd_driver_print_desc(desc);
        sh_puts("  descriptor valid: ");
        sh_puts(driver_registry_validate_desc(desc) == KERN_OK ?
                "yes\r\n" : "no\r\n");
        cmd_driver_print_lookup_status();
        sh_puts("  debug UART: active compatibility path\r\n");
        return;
    }

    sh_puts("  registry entries: ");
    sh_putdec(driver_registry_count());
    sh_puts("\r\n");
    sh_puts("  registry valid: ");
    sh_puts(driver_registry_validate_all() == KERN_OK ? "yes\r\n" : "no\r\n");
    for (uint32_t i = 0; i < driver_registry_count(); i++) {
        const driver_descriptor_t *desc = driver_registry_get(i);
        if (desc == NULL) {
            continue;
        }
        cmd_driver_print_desc(desc);
    }
    cmd_driver_print_lookup_status();
    sh_puts("  uart server: live IPC/name-server path validated\r\n");
    sh_puts("  debug UART: active compatibility path\r\n");
}

static void cmd_driver_release_lookup(cap_id_t service_cap) {
    (void)driver_release_service(driver_runtime_inbox_cap(), service_cap);
    if (service_cap > 0) {
        cap_delete(service_cap);
    }
}

static int cmd_driver_get_service_cap(const char *service_name,
                                      cap_id_t *out_service_cap);

static void cmd_driver_lookup(const char *service_name) {
    const driver_descriptor_t *desc = NULL;
    int err;

    if (service_name == NULL) {
        sh_puts("Usage: driver lookup <service>\r\n");
        return;
    }

    sh_puts("User driver lookup\r\n");
    sh_puts("  service: ");
    sh_puts(service_name);
    sh_puts("\r\n");

    err = driver_registry_query(service_name, &desc);
    if (err != KERN_OK || desc == NULL) {
        sh_puts("  registry: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = driver_registry_validate_desc(desc);
    sh_puts("  descriptor: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    cmd_driver_print_ns_state("lookup", "ready");
    const char *reason = NULL;
    err = driver_runtime_lookup_ready(DRIVER_SHELL_PROBE_TIMEOUT, &reason);
    sh_puts("  lookup ready: ");
    if (err == KERN_OK) {
        sh_puts("yes\r\n");
    } else {
        sh_puts("no (");
        sh_puts(reason ? reason : driver_error_name(err));
        sh_puts(": ");
        sh_puts(driver_error_name(err));
        sh_puts(")\r\n");
    }
    cmd_driver_print_inbox_state();

    if (err == KERN_OK && strcmp(service_name, "dev.uart0") == 0) {
        cap_id_t service_cap = KERN_INVALID_ID;
        int lookup_err = driver_runtime_lookup_uart(driver_runtime_inbox_cap(),
                                                    &service_cap,
                                                    DRIVER_SHELL_PROBE_TIMEOUT);
        sh_puts("  service cap: ");
        if (lookup_err == KERN_OK) {
            sh_putdec((uint32_t)service_cap);
            sh_puts("\r\n");
            int ping_err = driver_ping(service_cap,
                                       DRIVER_SHELL_PROBE_TIMEOUT);
            sh_puts("  ping: ");
            sh_puts(driver_error_name(ping_err));
            sh_puts("\r\n");
            cmd_driver_release_lookup(service_cap);
        } else {
            sh_puts(driver_error_name(lookup_err));
            sh_puts("\r\n");
        }
    }
}

static void cmd_driver_registered(const char *service_name) {
    const driver_descriptor_t *desc = NULL;
    const char *reason = NULL;
    cap_id_t service_cap = KERN_INVALID_ID;
    int err;

    if (service_name == NULL) {
        service_name = "dev.uart0";
    }

    sh_puts("User driver registration\r\n");
    sh_puts("  service: ");
    sh_puts(service_name);
    sh_puts("\r\n");

    err = driver_registry_query(service_name, &desc);
    if (err != KERN_OK || desc == NULL) {
        sh_puts("  descriptor: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = driver_runtime_lookup_ready(DRIVER_SHELL_PROBE_TIMEOUT, &reason);
    sh_puts("  lookup path: ");
    if (err == KERN_OK) {
        sh_puts("ready\r\n");
    } else {
        sh_puts(reason ? reason : driver_error_name(err));
        sh_puts(": ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = cmd_driver_get_service_cap(service_name, &service_cap);
    sh_puts("  service registered: ");
    if (err != KERN_OK) {
        sh_puts("no (");
        sh_puts(driver_error_name(err));
        sh_puts(")\r\n");
        return;
    }
    sh_puts("yes\r\n");

    err = driver_ping(service_cap, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  ping: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    cmd_driver_release_lookup(service_cap);
}

static int cmd_driver_get_service_cap(const char *service_name,
                                      cap_id_t *out_service_cap) {
    if (out_service_cap == NULL) {
        return KERN_ERR_PARAM;
    }
    *out_service_cap = KERN_INVALID_ID;
    if (strcmp(service_name, "dev.uart0") != 0) {
        return KERN_ERR_NOEXIST;
    }
    return driver_runtime_lookup_uart(driver_runtime_inbox_cap(),
                                      out_service_cap,
                                      DRIVER_SHELL_PROBE_TIMEOUT);
}

static void cmd_driver_probe(const char *service_name) {
    const driver_descriptor_t *desc = NULL;
    cap_id_t service_cap = KERN_INVALID_ID;
    uint32_t value = 0;
    int err;

    if (service_name == NULL) {
        sh_puts("Usage: driver probe <service>\r\n");
        return;
    }

    sh_puts("User driver probe\r\n");
    sh_puts("  service: ");
    sh_puts(service_name);
    sh_puts("\r\n");

    err = driver_registry_query(service_name, &desc);
    if (err != KERN_OK || desc == NULL) {
        sh_puts("  registry: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = driver_registry_validate_desc(desc);
    sh_puts("  descriptor: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    err = driver_runtime_lookup_ready(DRIVER_SHELL_PROBE_TIMEOUT, NULL);
    sh_puts("  lookup ready: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    err = cmd_driver_get_service_cap(service_name, &service_cap);
    sh_puts("  service cap: ");
    if (err != KERN_OK) {
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }
    sh_putdec((uint32_t)service_cap);
    sh_puts("\r\n");

    err = driver_ping(service_cap, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  ping: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");

    value = 0;
    err = driver_get_resources(service_cap, &value, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  resources: ");
    sh_puts(driver_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ");
        shell_put_driver_resources(value);
    }
    sh_puts("\r\n");

    value = 0;
    err = driver_get_status(service_cap, &value, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  status: ");
    sh_puts(driver_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ");
        shell_put_driver_status_bits(value);
    }
    sh_puts("\r\n");

    value = 0;
    err = driver_poll(service_cap, &value, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  poll: ");
    sh_puts(driver_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" events=");
        sh_puthex(value);
    }
    sh_puts("\r\n");

    err = driver_open(service_cap, DRV_FLAG_NONE, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  open: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err == KERN_OK) {
        int close_err = driver_close(service_cap, DRIVER_SHELL_PROBE_TIMEOUT);
        sh_puts("  close: ");
        sh_puts(driver_error_name(close_err));
        sh_puts("\r\n");
    }

    cmd_driver_release_lookup(service_cap);
}

static void cmd_driver_probe_mmio(const char *service_name) {
    const driver_descriptor_t *desc = NULL;
    cap_id_t service_cap = KERN_INVALID_ID;
    cap_id_t mmio_cap = KERN_INVALID_ID;
    uint32_t value = 0;
    int err;

    if (service_name == NULL) {
        sh_puts("Usage: driver probe-mmio <service>\r\n");
        return;
    }

    sh_puts("User driver MMIO probe\r\n");
    sh_puts("  service: ");
    sh_puts(service_name);
    sh_puts("\r\n");

    err = driver_registry_query(service_name, &desc);
    if (err != KERN_OK || desc == NULL) {
        sh_puts("  registry: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = driver_registry_validate_desc(desc);
    sh_puts("  descriptor: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    err = driver_runtime_lookup_ready(DRIVER_SHELL_PROBE_TIMEOUT, NULL);
    sh_puts("  lookup ready: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    err = cmd_driver_get_service_cap(service_name, &service_cap);
    sh_puts("  service cap: ");
    if (err != KERN_OK) {
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }
    sh_putdec((uint32_t)service_cap);
    sh_puts("\r\n");

    kern_err_t kerr = kmmio_create_cap(0x40000000UL, 16, 4, CAP_FULL,
                                       &mmio_cap);
    sh_puts("  mmio cap: ");
    if (kerr != KERN_OK) {
        sh_puts(driver_error_name((int)kerr));
        sh_puts("\r\n");
        cmd_driver_release_lookup(service_cap);
        return;
    }
    sh_putdec((uint32_t)mmio_cap);
    sh_puts("\r\n");

    err = driver_attach_cap(service_cap, mmio_cap,
                            DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  attach-mmio: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");

    value = 0;
    err = driver_get_resources(service_cap, &value, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  resources: ");
    sh_puts(driver_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ");
        shell_put_driver_resources(value);
    }
    sh_puts("\r\n");

    value = 0;
    err = driver_get_status(service_cap, &value, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  status: ");
    sh_puts(driver_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ");
        shell_put_driver_status_bits(value);
    }
    sh_puts("\r\n");

    err = driver_open(service_cap, DRV_FLAG_NONE, DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  open: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");
    if (err == KERN_OK) {
        const char probe[] = "probe";
        int write_err = driver_write(service_cap, probe,
                                     (uint32_t)(sizeof(probe) - 1U),
                                     DRIVER_SHELL_PROBE_TIMEOUT);
        sh_puts("  write: ");
        if (write_err >= 0) {
            sh_puts("ok bytes=");
            sh_putdec((uint32_t)write_err);
        } else {
            sh_puts(driver_error_name(write_err));
        }
        sh_puts("\r\n");

        int close_err = driver_close(service_cap, DRIVER_SHELL_PROBE_TIMEOUT);
        sh_puts("  close: ");
        sh_puts(driver_error_name(close_err));
        sh_puts("\r\n");
    }

    err = driver_detach_resource(service_cap, DRV_RESOURCE_MMIO,
                                 DRIVER_SHELL_PROBE_TIMEOUT);
    sh_puts("  detach-mmio: ");
    sh_puts(driver_error_name(err));
    sh_puts("\r\n");

    kerr = kmmio_delete_cap(mmio_cap);
    sh_puts("  delete-mmio: ");
    sh_puts(driver_error_name((int)kerr));
    sh_puts("\r\n");

    cmd_driver_release_lookup(service_cap);
}

static void cmd_driver_bind_ns(const char *arg) {
    int err;

    if (arg == NULL) {
        sh_puts("Usage: driver bind-ns <cap|clear>\r\n");
        return;
    }

    if (strcmp(arg, "clear") == 0 || strcmp(arg, "none") == 0) {
        driver_runtime_clear_name_server();
        sh_puts("User driver name-server binding cleared\r\n");
        cmd_driver_print_lookup_status();
        return;
    }

    cap_id_t ns_cap = (cap_id_t)parse_dec(arg);
    err = driver_runtime_bind_name_server(ns_cap);
    if (err != KERN_OK) {
        sh_puts("driver bind-ns: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    sh_puts("User driver name-server binding updated\r\n");
    cmd_driver_print_lookup_status();
}

static void cmd_driver_bind_inbox(const char *arg) {
    int err;

    if (arg == NULL) {
        sh_puts("Usage: driver bind-inbox <cap|auto|clear>\r\n");
        return;
    }

    if (strcmp(arg, "clear") == 0 || strcmp(arg, "none") == 0) {
        (void)cmd_driver_release_owned_inbox();
        sh_puts("User driver inbox binding cleared\r\n");
        cmd_driver_print_inbox_state();
        return;
    }

    if (strcmp(arg, "auto") == 0) {
        if (driver_runtime_inbox_bound()) {
            sh_puts("driver bind-inbox: busy\r\n");
            cmd_driver_print_inbox_state();
            return;
        }

        ep_id_t ep_id = endpoint_create("drv_inbox", KERN_EP_MSG_SIZE, 2);
        if (ep_id < 0) {
            sh_puts("driver bind-inbox: resource\r\n");
            return;
        }

        cap_id_t inbox_cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
        if (inbox_cap < 0) {
            (void)endpoint_delete(ep_id);
            sh_puts("driver bind-inbox: resource\r\n");
            return;
        }

        err = driver_runtime_bind_owned_inbox(inbox_cap);
        if (err != KERN_OK) {
            (void)endpoint_delete(ep_id);
            sh_puts("driver bind-inbox: ");
            sh_puts(driver_error_name(err));
            sh_puts("\r\n");
            return;
        }

        sh_puts("User driver inbox created\r\n");
        cmd_driver_print_inbox_state();
        return;
    }

    cap_id_t inbox_cap = (cap_id_t)parse_dec(arg);
    err = driver_runtime_bind_inbox(inbox_cap);
    if (err != KERN_OK) {
        sh_puts("driver bind-inbox: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    sh_puts("User driver inbox binding updated\r\n");
    cmd_driver_print_inbox_state();
}

static void cmd_driver_ns_stop(void) {
    if (shell_driver_uart_task >= 0 || shell_driver_uart_ep >= 0) {
        sh_puts("driver ns-stop: uart service active\r\n");
        return;
    }
    if (shell_driver_ns_task >= 0) {
        (void)task_delete(shell_driver_ns_task);
        shell_driver_ns_task = KERN_INVALID_ID;
    }
    if (shell_driver_ns_ep >= 0) {
        (void)endpoint_delete(shell_driver_ns_ep);
        shell_driver_ns_ep = KERN_INVALID_ID;
    }
    shell_driver_ns_cap = KERN_INVALID_ID;
    driver_runtime_clear_name_server();
    sh_puts("User driver name-server stopped\r\n");
    cmd_driver_print_lookup_status();
}

static void cmd_driver_uart_stop(void) {
    int unregister_err = KERN_ERR_STATE;

    if (driver_runtime_name_server_bound()) {
        unregister_err = nameserver_unregister(driver_runtime_name_server_cap(),
                                               "dev.uart0", 0x55415254U,
                                               DRIVER_SHELL_PROBE_TIMEOUT);
    }
    if (shell_driver_uart_task >= 0) {
        (void)task_delete(shell_driver_uart_task);
        shell_driver_uart_task = KERN_INVALID_ID;
    }
    if (shell_driver_uart_ep >= 0) {
        (void)endpoint_delete(shell_driver_uart_ep);
        shell_driver_uart_ep = KERN_INVALID_ID;
    }
    shell_driver_uart_cap = KERN_INVALID_ID;
    sh_puts("User driver UART service stopped\r\n");
    sh_puts("  unregister: ");
    sh_puts(driver_error_name(unregister_err));
    sh_puts("\r\n");
    cmd_driver_print_lookup_status();
}

static void cmd_driver_uart_fault(void) {
    int unregister_err = KERN_ERR_STATE;

    if (driver_runtime_name_server_bound()) {
        unregister_err = nameserver_unregister(driver_runtime_name_server_cap(),
                                               "dev.uart0", 0x55415254U,
                                               DRIVER_SHELL_PROBE_TIMEOUT);
    }
    if (shell_driver_uart_task >= 0) {
        (void)task_delete(shell_driver_uart_task);
        shell_driver_uart_task = KERN_INVALID_ID;
    }
    if (shell_driver_uart_ep >= 0) {
        (void)endpoint_delete(shell_driver_uart_ep);
        shell_driver_uart_ep = KERN_INVALID_ID;
    }
    shell_driver_uart_cap = KERN_INVALID_ID;
    supervisor_service_t *svc = shell_driver_supervisor_get();
    supervisor_record_fault(svc);
    supervisor_set_health(svc, KERN_ERR_FAULT);
    sh_puts("User driver UART service fault injected\r\n");
    sh_puts("  unregister: ");
    sh_puts(driver_error_name(unregister_err));
    sh_puts("\r\n");
    cmd_driver_print_lookup_status();
}

static void cmd_driver_uart_start(void) {
    if (!driver_runtime_name_server_bound()) {
        sh_puts("driver uart-start: name-server\r\n");
        return;
    }
    if (shell_driver_uart_task >= 0 || shell_driver_uart_ep >= 0) {
        sh_puts("driver uart-start: busy\r\n");
        cmd_driver_print_lookup_status();
        return;
    }

    ep_id_t ep_id = endpoint_create("drv_uart", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        sh_puts("driver uart-start: resource\r\n");
        return;
    }

    task_id_t tid = task_create_user("drv_uart", shell_driver_uart_task_entry,
                                     NULL, 13, 1536);
    if (tid < 0) {
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: resource\r\n");
        return;
    }

    tcb_t *uart_tcb = task_get_tcb(tid);
    cap_id_t service_cap = cap_create_for(uart_tcb, endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE);
    if (service_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: resource\r\n");
        return;
    }

    cap_id_t root_cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                   CAP_READ | CAP_WRITE | CAP_TRANSFER,
                                   0);
    if (root_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: resource\r\n");
        return;
    }

    tcb_t *task = task_get_tcb(tid);
    if (task == NULL || task->sp == NULL) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: state\r\n");
        return;
    }
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)task->sp + 32U);
    *stacked_r0 = (uint32_t)service_cap;

    kern_err_t err = task_start(tid);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    (void)task_yield();
    err = nameserver_register(driver_runtime_name_server_cap(),
                              "dev.uart0", root_cap, 0x55415254U,
                              DRIVER_SHELL_PROBE_TIMEOUT);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver uart-start: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    shell_driver_uart_task = tid;
    shell_driver_uart_ep = ep_id;
    shell_driver_uart_cap = root_cap;
    sh_puts("User driver UART service started\r\n");
    cmd_driver_print_lookup_status();
}

static void cmd_driver_ns_start(void) {
    if (shell_driver_ns_task >= 0 || shell_driver_ns_ep >= 0 ||
        driver_runtime_name_server_bound()) {
        sh_puts("driver ns-start: busy\r\n");
        cmd_driver_print_lookup_status();
        return;
    }

    ep_id_t ep_id = endpoint_create("drv_ns", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        sh_puts("driver ns-start: resource\r\n");
        return;
    }

    task_id_t tid = task_create_user("drv_ns", shell_driver_nameserver_task,
                                     NULL, 13, 1536);
    if (tid < 0) {
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: resource\r\n");
        return;
    }

    tcb_t *ns_tcb = task_get_tcb(tid);
    cap_id_t ns_service_cap = cap_create_for(ns_tcb, endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                             CAP_READ | CAP_WRITE);
    if (ns_service_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: resource\r\n");
        return;
    }

    cap_id_t root_cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    if (root_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: resource\r\n");
        return;
    }

    tcb_t *task = task_get_tcb(tid);
    if (task == NULL || task->sp == NULL) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: state\r\n");
        return;
    }
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)task->sp + 32U);
    *stacked_r0 = (uint32_t)ns_service_cap;

    kern_err_t err = task_start(tid);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = driver_runtime_bind_name_server(root_cap);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("driver ns-start: ");
        sh_puts(driver_error_name(err));
        sh_puts("\r\n");
        return;
    }

    shell_driver_ns_task = tid;
    shell_driver_ns_ep = ep_id;
    shell_driver_ns_cap = root_cap;
    sh_puts("User driver name-server started\r\n");
    (void)task_yield();
    cmd_driver_print_lookup_status();
}

static int cmd_driver_health_probe(void);

static void cmd_driver_up(void) {
    sh_puts("User driver stack up\r\n");

    if (!driver_runtime_inbox_bound()) {
        cmd_driver_bind_inbox("auto");
        if (!driver_runtime_inbox_bound()) {
            sh_puts("driver up: inbox failed\r\n");
            return;
        }
    } else {
        sh_puts("  inbox: already ready\r\n");
    }

    if (shell_driver_ns_task < 0 && shell_driver_ns_ep < 0 &&
        !driver_runtime_name_server_bound()) {
        cmd_driver_ns_start();
        if (!driver_runtime_name_server_bound()) {
            sh_puts("driver up: name-server failed\r\n");
            return;
        }
    } else {
        sh_puts("  name-server: already ready\r\n");
    }

    if (shell_driver_uart_task < 0 && shell_driver_uart_ep < 0 &&
        shell_driver_uart_cap < 0) {
        cmd_driver_uart_start();
        if (shell_driver_uart_task < 0 || shell_driver_uart_ep < 0) {
            sh_puts("driver up: service failed\r\n");
            return;
        }
    } else {
        sh_puts("  service: already ready\r\n");
    }

    sh_puts("User driver stack ready\r\n");
    supervisor_set_health(shell_driver_supervisor_get(),
                          cmd_driver_health_probe());
    cmd_driver_status(NULL);
}

static void cmd_driver_down(void) {
    sh_puts("User driver stack down\r\n");
    supervisor_set_health(shell_driver_supervisor_get(), KERN_ERR_STATE);

    if (shell_driver_uart_task >= 0 || shell_driver_uart_ep >= 0 ||
        shell_driver_uart_cap >= 0) {
        cmd_driver_uart_stop();
    } else {
        sh_puts("  service: already stopped\r\n");
    }

    if (shell_driver_ns_task >= 0 || shell_driver_ns_ep >= 0 ||
        driver_runtime_name_server_bound()) {
        cmd_driver_ns_stop();
    } else {
        sh_puts("  name-server: already stopped\r\n");
    }

    if (driver_runtime_inbox_bound()) {
        cmd_driver_bind_inbox("clear");
    } else {
        sh_puts("  inbox: already cleared\r\n");
    }

    sh_puts("User driver stack stopped\r\n");
    cmd_driver_status(NULL);
}

static void cmd_driver_restart(void) {
    sh_puts("User driver stack restart\r\n");
    supervisor_record_restart(shell_driver_supervisor_get());
    cmd_driver_down();
    cmd_driver_up();
}

static int cmd_driver_health_probe(void) {
    cap_id_t service_cap = KERN_INVALID_ID;
    int err = driver_runtime_lookup_ready(DRIVER_SHELL_PROBE_TIMEOUT, NULL);
    if (err != KERN_OK) {
        return err;
    }

    err = cmd_driver_get_service_cap(DRIVER_SHELL_SERVICE_NAME, &service_cap);
    if (err != KERN_OK) {
        return err;
    }

    err = driver_ping(service_cap, DRIVER_SHELL_PROBE_TIMEOUT);
    cmd_driver_release_lookup(service_cap);
    return err;
}

static void cmd_driver_health(void) {
    sh_puts("User driver health\r\n");
    int err = cmd_driver_health_probe();
    supervisor_set_health(shell_driver_supervisor_get(), err);
    if (err != KERN_OK) {
        sh_puts("  state: ");
        sh_puts(err == KERN_ERR_STATE ? "stopped" : driver_error_name(err));
        sh_puts("\r\n");
        cmd_driver_status(NULL);
        return;
    }

    sh_puts("  ping: ok\r\n");
    cmd_driver_status(NULL);
}

static void cmd_driver_recover(void) {
    sh_puts("User driver stack recover\r\n");
    supervisor_record_recover(shell_driver_supervisor_get());
    int err = cmd_driver_health_probe();
    supervisor_set_health(shell_driver_supervisor_get(), err);
    if (err != KERN_OK) {
        sh_puts("  health: ");
        sh_puts(err == KERN_ERR_STATE ? "stopped" : driver_error_name(err));
        sh_puts("\r\n");
        cmd_driver_up();
        return;
    }

    sh_puts("  ping: ok\r\n");
    supervisor_set_health(shell_driver_supervisor_get(), KERN_OK);
    sh_puts("User driver stack healthy\r\n");
    cmd_driver_status(NULL);
}

static void cmd_driver(int argc, char **argv) {
    if (argc <= 1 || strcmp(argv[1], "abi") == 0) {
        cmd_driver_abi();
        return;
    }
    if (strcmp(argv[1], "status") == 0) {
        cmd_driver_status((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "up") == 0) {
        cmd_driver_up();
        return;
    }
    if (strcmp(argv[1], "down") == 0) {
        cmd_driver_down();
        return;
    }
    if (strcmp(argv[1], "restart") == 0) {
        cmd_driver_restart();
        return;
    }
    if (strcmp(argv[1], "health") == 0) {
        cmd_driver_health();
        return;
    }
    if (strcmp(argv[1], "recover") == 0) {
        cmd_driver_recover();
        return;
    }
    if (strcmp(argv[1], "lookup") == 0) {
        cmd_driver_lookup((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "registered") == 0) {
        cmd_driver_registered((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "probe") == 0) {
        cmd_driver_probe((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "probe-mmio") == 0) {
        cmd_driver_probe_mmio((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "bind-ns") == 0) {
        cmd_driver_bind_ns((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "bind-inbox") == 0) {
        cmd_driver_bind_inbox((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "ns-start") == 0) {
        cmd_driver_ns_start();
        return;
    }
    if (strcmp(argv[1], "ns-stop") == 0) {
        cmd_driver_ns_stop();
        return;
    }
    if (strcmp(argv[1], "uart-start") == 0) {
        cmd_driver_uart_start();
        return;
    }
    if (strcmp(argv[1], "uart-stop") == 0) {
        cmd_driver_uart_stop();
        return;
    }

    sh_puts("Usage: driver [abi|status [service]|up|down|restart|health|recover|lookup <service>|registered [service]|probe <service>|probe-mmio <service>|bind-ns <cap|clear>|bind-inbox <cap|auto|clear>|ns-start|ns-stop|uart-start|uart-stop]\r\n");
}

#endif /* DRIVER_ENABLE */

/*============================================================================
 * 内置命令: fs
 *============================================================================*/

#if CAP_ENABLE

#define FS_SHELL_PROBE_TIMEOUT 100U
#define FS_SHELL_OWNER_BADGE   0x46530001U
#define FS_SHELL_SERVICE_NAME  "fs.ramfs"

static task_id_t shell_fs_task = KERN_INVALID_ID;
static ep_id_t shell_fs_ep = KERN_INVALID_ID;
/* shell_fs_cap 定义在前面 (cmd_ls/cmd_cat 用) */
static task_id_t shell_fs_ns_task = KERN_INVALID_ID;
static ep_id_t shell_fs_ns_ep = KERN_INVALID_ID;
static cap_id_t shell_fs_ns_cap = KERN_INVALID_ID;
static ep_id_t shell_fs_inbox_ep = KERN_INVALID_ID;
static cap_id_t shell_fs_inbox_cap = KERN_INVALID_ID;
static supervisor_service_t *shell_fs_supervisor;

static supervisor_service_t *shell_fs_supervisor_get(void) {
    if (shell_fs_supervisor == NULL) {
        shell_fs_supervisor =
            supervisor_register_service(FS_SHELL_SERVICE_NAME,
                                        KERN_ERR_STATE);
    }
    return shell_fs_supervisor;
}

static void shell_fs_task_entry(void *arg) {
    (void)fs_server_run((int)(uintptr_t)arg, 0);
}

static void shell_fs_nameserver_task(void *arg) {
    (void)nameserver_service_run((int)(uintptr_t)arg, 0);
}

static void cmd_fs_abi(void) {
    sh_puts("User FS ABI\r\n");
    sh_puts("  protocol magic: ");
    sh_puthex(FS_MAGIC);
    sh_puts("\r\n");
    sh_puts("  path max: ");
    sh_putdec(FS_PATH_MAX);
    sh_puts(" bytes\r\n");
    sh_puts("  payload max: ");
    sh_putdec(FS_PAYLOAD_MAX);
    sh_puts(" bytes\r\n");
    sh_puts("  service fds: ");
    sh_putdec(FS_FD_MAX);
    sh_puts("\r\n");
    sh_puts("  ops: ping open close read write lseek readdir unlink mkdir stat\r\n");
    sh_puts("  backend: compatibility VFS syscalls\r\n");
}

static void cmd_fs_print_lookup_ready(void) {
    const char *reason = NULL;
    int err = fs_runtime_lookup_ready(FS_SHELL_PROBE_TIMEOUT, &reason);

    sh_puts("  lookup path ready: ");
    if (err == KERN_OK) {
        sh_puts("yes\r\n");
    } else {
        sh_puts("no (");
        sh_puts(reason ? reason : fs_error_name(err));
        sh_puts(": ");
        sh_puts(fs_error_name(err));
        sh_puts(")\r\n");
    }
}

static void cmd_fs_status(void) {
    sh_puts("User FS status\r\n");
    sh_puts("  service name: ");
    sh_puts(FS_SHELL_SERVICE_NAME);
    sh_puts("\r\n");
    sh_puts("  name-server cap: ");
    if (shell_fs_ns_cap < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_fs_ns_cap);
        int err = nameserver_ping(shell_fs_ns_cap, FS_SHELL_PROBE_TIMEOUT);
        sh_puts(" (");
        sh_puts(fs_error_name(err));
        sh_puts(")\r\n");
    }
    sh_puts("  inbox cap: ");
    if (shell_fs_inbox_cap < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_fs_inbox_cap);
        sh_puts(" (owned)\r\n");
    }
    sh_puts("  service cap: ");
    if (shell_fs_cap < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_fs_cap);
        sh_puts("\r\n");
    }
    sh_puts("  service task: ");
    if (shell_fs_task < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_fs_task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(shell_fs_task)));
        sh_puts(")\r\n");
    }
    sh_puts("  name-server task: ");
    if (shell_fs_ns_task < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_fs_ns_task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(shell_fs_ns_task)));
        sh_puts(")\r\n");
    }
    cmd_fs_print_lookup_ready();
    sh_puts("  restart count: ");
    supervisor_service_t *svc = shell_fs_supervisor_get();
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("\r\n");
    sh_puts("  recover count: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("\r\n");
    sh_puts("  fault count: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");
    sh_puts("  pending clients: ");
    sh_putdec(supervisor_pending_clients(svc));
    sh_puts("\r\n");
    sh_puts("  last health: ");
    sh_puts(fs_error_name(supervisor_last_health(svc)));
    sh_puts("\r\n");
    sh_puts("  ops: ping|open|close|read|write|lseek|readdir|unlink|mkdir|stat\r\n");
    sh_puts("  backend: compatibility VFS syscalls\r\n");
}

static int cmd_fs_lookup_service(cap_id_t *out_cap) {
    return fs_runtime_lookup_service(FS_SHELL_SERVICE_NAME, out_cap,
                                     FS_SHELL_PROBE_TIMEOUT);
}

static int cmd_fs_acquire_service(cap_id_t *out_cap, int *out_live_lookup) {
    if (out_cap == NULL || out_live_lookup == NULL) {
        return KERN_ERR_PARAM;
    }

    *out_cap = KERN_INVALID_ID;
    *out_live_lookup = 0;

    if (fs_runtime_name_server_bound() && fs_runtime_inbox_bound()) {
        int err = cmd_fs_lookup_service(out_cap);
        if (err == KERN_OK) {
            *out_live_lookup = 1;
        }
        return err;
    }

    if (shell_fs_cap <= 0) {
        return KERN_ERR_STATE;
    }
    *out_cap = shell_fs_cap;
    return KERN_OK;
}

static void cmd_fs_release_service(int live_lookup, cap_id_t service_cap) {
    if (live_lookup) {
        (void)fs_runtime_release_service(service_cap);
    }
}

static void cmd_fs_start(void) {
    if (shell_fs_task >= 0 || shell_fs_ep >= 0 || shell_fs_cap >= 0) {
        sh_puts("fs start: busy\r\n");
        cmd_fs_status();
        return;
    }

    ep_id_t ep_id = endpoint_create("fs_shell", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        sh_puts("fs start: resource\r\n");
        return;
    }

    task_id_t tid = task_create_user("fs_shell", shell_fs_task_entry,
                                     NULL, 13, 1536);
    if (tid < 0) {
        (void)endpoint_delete(ep_id);
        sh_puts("fs start: resource\r\n");
        return;
    }

    tcb_t *fs_tcb = task_get_tcb(tid);
    cap_id_t service_cap = cap_create_for(fs_tcb, endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE);
    if (service_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs start: resource\r\n");
        return;
    }

    cap_id_t root_cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                   CAP_READ | CAP_WRITE | CAP_TRANSFER,
                                   0);
    if (root_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs start: resource\r\n");
        return;
    }

    tcb_t *task = task_get_tcb(tid);
    if (task == NULL || task->sp == NULL) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs start: state\r\n");
        return;
    }
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)task->sp + 32U);
    *stacked_r0 = (uint32_t)service_cap;

    kern_err_t err = task_start(tid);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs start: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    shell_fs_task = tid;
    shell_fs_ep = ep_id;
    shell_fs_cap = root_cap;
    if (shell_fs_ns_cap > 0) {
        err = nameserver_register(shell_fs_ns_cap, FS_SHELL_SERVICE_NAME,
                                  root_cap, FS_SHELL_OWNER_BADGE,
                                  FS_SHELL_PROBE_TIMEOUT);
        if (err != KERN_OK) {
            (void)task_delete(tid);
            (void)endpoint_delete(ep_id);
            shell_fs_task = KERN_INVALID_ID;
            shell_fs_ep = KERN_INVALID_ID;
            shell_fs_cap = KERN_INVALID_ID;
            sh_puts("fs start register: ");
            sh_puts(fs_error_name(err));
            sh_puts("\r\n");
            return;
        }
    }
    sh_puts("User FS service started\r\n");
    (void)task_yield();
    cmd_fs_status();
}

static void cmd_fs_stop(void) {
    int unregister_err = KERN_ERR_STATE;

    if (shell_fs_ns_cap > 0 && shell_fs_cap > 0) {
        unregister_err = nameserver_unregister(shell_fs_ns_cap,
                                               FS_SHELL_SERVICE_NAME,
                                               FS_SHELL_OWNER_BADGE,
                                               FS_SHELL_PROBE_TIMEOUT);
    }
    if (shell_fs_task >= 0) {
        (void)task_delete(shell_fs_task);
        shell_fs_task = KERN_INVALID_ID;
    }
    if (shell_fs_ep >= 0) {
        (void)endpoint_delete(shell_fs_ep);
        shell_fs_ep = KERN_INVALID_ID;
    }
    shell_fs_cap = KERN_INVALID_ID;
    sh_puts("User FS service stopped\r\n");
    sh_puts("  unregister: ");
    sh_puts(fs_error_name(unregister_err));
    sh_puts("\r\n");
    cmd_fs_status();
}

static void cmd_fs_fault(void) {
    int unregister_err = KERN_ERR_STATE;

    if (shell_fs_ns_cap > 0 && shell_fs_cap > 0) {
        unregister_err = nameserver_unregister(shell_fs_ns_cap,
                                               FS_SHELL_SERVICE_NAME,
                                               FS_SHELL_OWNER_BADGE,
                                               FS_SHELL_PROBE_TIMEOUT);
    }
    if (shell_fs_task >= 0) {
        (void)task_delete(shell_fs_task);
        shell_fs_task = KERN_INVALID_ID;
    }
    if (shell_fs_ep >= 0) {
        (void)endpoint_delete(shell_fs_ep);
        shell_fs_ep = KERN_INVALID_ID;
    }
    shell_fs_cap = KERN_INVALID_ID;
    supervisor_service_t *svc = shell_fs_supervisor_get();
    supervisor_record_fault(svc);
    supervisor_set_health(svc, KERN_ERR_FAULT);
    sh_puts("User FS service fault injected\r\n");
    sh_puts("  unregister: ");
    sh_puts(fs_error_name(unregister_err));
    sh_puts("\r\n");
    cmd_fs_status();
}

static void cmd_fs_probe_cap(cap_id_t service_cap) {
    if (service_cap <= 0) {
        sh_puts("fs probe: service\r\n");
        return;
    }

    sh_puts("User FS probe\r\n");
    int err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  ping: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    int fd = fs_open(service_cap, "/tmp", O_RDONLY, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  open /tmp: ");
    if (fd <= 0) {
        sh_puts(fs_error_name(fd));
        sh_puts("\r\n");
        return;
    }
    sh_puts("ok fd=");
    sh_putdec((uint32_t)fd);
    sh_puts("\r\n");

    dirent_t entry;
    err = fs_readdir(service_cap, fd, &entry, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  readdir: ");
    sh_puts(fs_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ");
        sh_putdec(entry.ino);
        sh_puts(" ");
        switch (entry.type) {
        case INODE_TYPE_DIR: sh_putc('d'); break;
        case INODE_TYPE_FILE: sh_putc('-'); break;
        case INODE_TYPE_CHRDEV: sh_putc('c'); break;
        default: sh_putc('?'); break;
        }
        sh_puts(" ");
        sh_puts(entry.name);
    }
    sh_puts("\r\n");

    err = fs_close(service_cap, fd, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  close: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");

    (void)fs_unlink(service_cap, "/tmp/fsp", FS_SHELL_PROBE_TIMEOUT);
    fd = fs_open(service_cap, "/tmp/fsp", O_WRONLY | O_CREAT | O_TRUNC,
                 FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  create /tmp/fsp: ");
    if (fd <= 0) {
        sh_puts(fs_error_name(fd));
        sh_puts("\r\n");
        return;
    }
    sh_puts("ok fd=");
    sh_putdec((uint32_t)fd);
    sh_puts("\r\n");

    const char payload[] = "probe";
    err = fs_write(service_cap, fd, payload,
                   (uint32_t)(sizeof(payload) - 1U),
                   FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  write: ");
    if (err == (int)(sizeof(payload) - 1U)) {
        sh_puts("ok bytes=");
        sh_putdec((uint32_t)err);
    } else {
        sh_puts(fs_error_name(err));
    }
    sh_puts("\r\n");

    int close_err = fs_close(service_cap, fd, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  close file: ");
    sh_puts(fs_error_name(close_err));
    sh_puts("\r\n");

    vfs_stat_t st;
    err = fs_stat(service_cap, "/tmp/fsp", &st, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  stat file: ");
    sh_puts(fs_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ino=");
        sh_putdec(st.ino);
        sh_puts(" size=");
        sh_putdec(st.size);
        sh_puts(" type=");
        sh_puts(st.type == INODE_TYPE_FILE ? "file" : "?");
    }
    sh_puts("\r\n");

    err = fs_unlink(service_cap, "/tmp/fsp", FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  unlink file: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");

    (void)fs_unlink(service_cap, "/tmp/fspd", FS_SHELL_PROBE_TIMEOUT);
    err = fs_mkdir(service_cap, "/tmp/fspd", FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  mkdir /tmp/fspd: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    if (err != KERN_OK) {
        return;
    }

    err = fs_stat(service_cap, "/tmp/fspd", &st, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  stat dir: ");
    sh_puts(fs_error_name(err));
    if (err == KERN_OK) {
        sh_puts(" ino=");
        sh_putdec(st.ino);
        sh_puts(" type=");
        sh_puts(st.type == INODE_TYPE_DIR ? "dir" : "?");
    }
    sh_puts("\r\n");

    err = fs_unlink(service_cap, "/tmp/fspd", FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  unlink dir: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
}

static void cmd_fs_probe(void) {
    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;

    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (live_lookup) {
        sh_puts("User FS lookup\r\n");
        sh_puts("  service cap: ");
        sh_putdec((uint32_t)service_cap);
        sh_puts("\r\n");
    }
    if (err != KERN_OK) {
        sh_puts("fs probe: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    cmd_fs_probe_cap(service_cap);
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_lookup(void) {
    cap_id_t service_cap = KERN_INVALID_ID;
    int err = cmd_fs_lookup_service(&service_cap);

    sh_puts("User FS lookup\r\n");
    sh_puts("  service: ");
    sh_puts(FS_SHELL_SERVICE_NAME);
    sh_puts("\r\n");
    sh_puts("  service cap: ");
    if (err != KERN_OK) {
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }
    sh_putdec((uint32_t)service_cap);
    sh_puts("\r\n");
    err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  ping: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    (void)fs_runtime_release_service(service_cap);
}

static void cmd_fs_registered(void) {
    const char *reason = NULL;
    cap_id_t service_cap = KERN_INVALID_ID;

    sh_puts("User FS registration\r\n");
    int err = fs_runtime_lookup_ready(FS_SHELL_PROBE_TIMEOUT, &reason);
    sh_puts("  lookup path: ");
    if (err == KERN_OK) {
        sh_puts("ready\r\n");
    } else {
        sh_puts(reason ? reason : fs_error_name(err));
        sh_puts(": ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = cmd_fs_lookup_service(&service_cap);
    sh_puts("  service registered: ");
    if (err != KERN_OK) {
        sh_puts("no (");
        sh_puts(fs_error_name(err));
        sh_puts(")\r\n");
        return;
    }
    sh_puts("yes\r\n");

    err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("  ping: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    (void)fs_runtime_release_service(service_cap);
}

static void cmd_fs_ls(const char *path) {
    if (path == NULL) {
        path = "/";
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs ls: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    int fd = fs_open(service_cap, path, O_RDONLY, FS_SHELL_PROBE_TIMEOUT);
    if (fd <= 0) {
        sh_puts("fs ls: ");
        sh_puts(path);
        sh_puts(": ");
        sh_puts(fs_error_name(fd));
        sh_puts("\r\n");
        cmd_fs_release_service(live_lookup, service_cap);
        return;
    }

    dirent_t entry;
    while (fs_readdir(service_cap, fd, &entry, FS_SHELL_PROBE_TIMEOUT) == KERN_OK) {
        sh_putdec_width(entry.ino, 6);
        sh_puts("  ");
        switch (entry.type) {
        case INODE_TYPE_DIR:    sh_putc('d'); break;
        case INODE_TYPE_FILE:   sh_putc('-'); break;
        case INODE_TYPE_CHRDEV: sh_putc('c'); break;
        default:                sh_putc('?'); break;
        }
        sh_puts("  ");
        sh_puts(entry.name);
        sh_puts("\r\n");
    }

    err = fs_close(service_cap, fd, FS_SHELL_PROBE_TIMEOUT);
    if (err != KERN_OK) {
        sh_puts("fs ls close: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
    }
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_cat(const char *path) {
    if (path == NULL) {
        sh_puts("Usage: fs cat <file>\r\n");
        return;
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs cat: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    int fd = fs_open(service_cap, path, O_RDONLY, FS_SHELL_PROBE_TIMEOUT);
    if (fd <= 0) {
        sh_puts("fs cat: ");
        sh_puts(path);
        sh_puts(": ");
        sh_puts(fs_error_name(fd));
        sh_puts("\r\n");
        cmd_fs_release_service(live_lookup, service_cap);
        return;
    }

    char buf[FS_PAYLOAD_MAX + 1U];
    int32_t n;
    while ((n = fs_read(service_cap, fd, buf, FS_PAYLOAD_MAX,
                        FS_SHELL_PROBE_TIMEOUT)) > 0) {
        buf[n] = '\0';
        sh_puts(buf);
    }
    sh_puts("\r\n");

    err = fs_close(service_cap, fd, FS_SHELL_PROBE_TIMEOUT);
    if (err != KERN_OK) {
        sh_puts("fs cat close: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
    }
    cmd_fs_release_service(live_lookup, service_cap);
}

static int cmd_fs_write_chunk(cap_id_t service_cap, int fd,
                              const char *data, uint32_t len) {
    uint32_t off = 0;

    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > FS_PAYLOAD_MAX) {
            chunk = FS_PAYLOAD_MAX;
        }
        int n = fs_write(service_cap, fd, data + off, chunk,
                         FS_SHELL_PROBE_TIMEOUT);
        if (n != (int)chunk) {
            return n < 0 ? n : KERN_ERR_STATE;
        }
        off += chunk;
    }
    return KERN_OK;
}

static void cmd_fs_write_file(int argc, char **argv) {
    if (argc < 4) {
        sh_puts("Usage: fs write <file> <text>\r\n");
        return;
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs write: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    int fd = fs_open(service_cap, argv[2], O_WRONLY | O_CREAT | O_TRUNC,
                     FS_SHELL_PROBE_TIMEOUT);
    if (fd <= 0) {
        sh_puts("fs write: ");
        sh_puts(argv[2]);
        sh_puts(": ");
        sh_puts(fs_error_name(fd));
        sh_puts("\r\n");
        cmd_fs_release_service(live_lookup, service_cap);
        return;
    }

    uint32_t total = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3) {
            err = cmd_fs_write_chunk(service_cap, fd, " ", 1U);
            if (err != KERN_OK) {
                break;
            }
            total++;
        }
        uint32_t len = (uint32_t)strlen(argv[i]);
        err = cmd_fs_write_chunk(service_cap, fd, argv[i], len);
        if (err != KERN_OK) {
            break;
        }
        total += len;
    }

    int close_err = fs_close(service_cap, fd, FS_SHELL_PROBE_TIMEOUT);
    if (err == KERN_OK && close_err != KERN_OK) {
        err = close_err;
    }
    if (err == KERN_OK) {
        sh_puts("fs write: ok bytes=");
        sh_putdec(total);
        sh_puts("\r\n");
    } else {
        sh_puts("fs write: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
    }
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_rm(const char *path) {
    if (path == NULL) {
        sh_puts("Usage: fs rm <file>\r\n");
        return;
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs rm: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = fs_unlink(service_cap, path, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("fs rm: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_mkdir(const char *path) {
    if (path == NULL) {
        sh_puts("Usage: fs mkdir <dir>\r\n");
        return;
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs mkdir: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    err = fs_mkdir(service_cap, path, FS_SHELL_PROBE_TIMEOUT);
    sh_puts("fs mkdir: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_stat(const char *path) {
    if (path == NULL) {
        sh_puts("Usage: fs stat <path>\r\n");
        return;
    }

    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    if (err != KERN_OK) {
        sh_puts("fs stat: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    vfs_stat_t st;
    err = fs_stat(service_cap, path, &st, FS_SHELL_PROBE_TIMEOUT);
    if (err != KERN_OK) {
        sh_puts("fs stat: ");
        sh_puts(path);
        sh_puts(": ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        cmd_fs_release_service(live_lookup, service_cap);
        return;
    }

    sh_puts("User FS stat\r\n");
    sh_puts("  path: ");
    sh_puts(path);
    sh_puts("\r\n");
    sh_puts("  ino: ");
    sh_putdec(st.ino);
    sh_puts("\r\n");
    sh_puts("  type: ");
    switch (st.type) {
    case INODE_TYPE_DIR: sh_puts("dir"); break;
    case INODE_TYPE_FILE: sh_puts("file"); break;
    case INODE_TYPE_CHRDEV: sh_puts("chrdev"); break;
    default: sh_puts("?"); break;
    }
    sh_puts("\r\n");
    sh_puts("  size: ");
    sh_putdec(st.size);
    sh_puts("\r\n");
    cmd_fs_release_service(live_lookup, service_cap);
}

static void cmd_fs_bind_inbox(const char *arg) {
    if (arg == NULL) {
        sh_puts("Usage: fs bind-inbox <auto|clear>\r\n");
        return;
    }
    if (strcmp(arg, "clear") == 0 || strcmp(arg, "none") == 0) {
        if (shell_fs_inbox_ep >= 0) {
            (void)endpoint_delete(shell_fs_inbox_ep);
        } else if (shell_fs_inbox_cap >= 0) {
            cap_delete(shell_fs_inbox_cap);
        }
        shell_fs_inbox_ep = KERN_INVALID_ID;
        shell_fs_inbox_cap = KERN_INVALID_ID;
        fs_runtime_clear_inbox();
        sh_puts("User FS inbox cleared\r\n");
        cmd_fs_status();
        return;
    }
    if (strcmp(arg, "auto") != 0) {
        sh_puts("fs bind-inbox: param\r\n");
        return;
    }
    if (shell_fs_inbox_cap >= 0 || shell_fs_inbox_ep >= 0) {
        sh_puts("fs bind-inbox: busy\r\n");
        cmd_fs_status();
        return;
    }
    ep_id_t ep_id = endpoint_create("fs_inbox", KERN_EP_MSG_SIZE, 2);
    if (ep_id < 0) {
        sh_puts("fs bind-inbox: resource\r\n");
        return;
    }
    cap_id_t cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    if (cap < 0) {
        (void)endpoint_delete(ep_id);
        sh_puts("fs bind-inbox: resource\r\n");
        return;
    }
    shell_fs_inbox_ep = ep_id;
    shell_fs_inbox_cap = cap;
    (void)fs_runtime_bind_owned_inbox(cap);
    sh_puts("User FS inbox created\r\n");
    cmd_fs_status();
}

static void cmd_fs_ns_start(void) {
    if (shell_fs_ns_task >= 0 || shell_fs_ns_ep >= 0 ||
        shell_fs_ns_cap >= 0) {
        sh_puts("fs ns-start: busy\r\n");
        cmd_fs_status();
        return;
    }

    ep_id_t ep_id = endpoint_create("fs_ns", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        sh_puts("fs ns-start: endpoint\r\n");
        return;
    }
    task_id_t tid = task_create_user("fs_ns", shell_fs_nameserver_task,
                                     NULL, 13, 1536);
    if (tid < 0) {
        (void)endpoint_delete(ep_id);
        sh_puts("fs ns-start: task\r\n");
        return;
    }
    tcb_t *ns_tcb = task_get_tcb(tid);
    cap_id_t service_cap = cap_create_for(ns_tcb, endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE);
    if (service_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs ns-start: service cap\r\n");
        return;
    }
    cap_id_t root_cap = cap_create(endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT, CAP_FULL, 0);
    if (root_cap < 0) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs ns-start: root cap\r\n");
        return;
    }
    tcb_t *task = task_get_tcb(tid);
    if (task == NULL || task->sp == NULL) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs ns-start: state\r\n");
        return;
    }
    uint32_t *stacked_r0 = (uint32_t *)((uint8_t *)task->sp + 32U);
    *stacked_r0 = (uint32_t)service_cap;

    kern_err_t err = task_start(tid);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        (void)endpoint_delete(ep_id);
        sh_puts("fs ns-start: ");
        sh_puts(fs_error_name(err));
        sh_puts("\r\n");
        return;
    }

    shell_fs_ns_task = tid;
    shell_fs_ns_ep = ep_id;
    shell_fs_ns_cap = root_cap;
    (void)fs_runtime_bind_name_server(root_cap);
    sh_puts("User FS name-server started\r\n");
    (void)task_yield();
    cmd_fs_status();
}

static void cmd_fs_ns_stop(void) {
    if (shell_fs_task >= 0 || shell_fs_ep >= 0) {
        sh_puts("fs ns-stop: fs service active\r\n");
        return;
    }
    if (shell_fs_ns_task >= 0) {
        (void)task_delete(shell_fs_ns_task);
        shell_fs_ns_task = KERN_INVALID_ID;
    }
    if (shell_fs_ns_ep >= 0) {
        (void)endpoint_delete(shell_fs_ns_ep);
        shell_fs_ns_ep = KERN_INVALID_ID;
    }
    shell_fs_ns_cap = KERN_INVALID_ID;
    fs_runtime_clear_name_server();
    sh_puts("User FS name-server stopped\r\n");
    cmd_fs_status();
}

static void cmd_fs_up(void) {
    sh_puts("User FS stack up\r\n");
    int created_inbox = 0;
    int created_ns = 0;

    if (shell_fs_inbox_cap < 0 && shell_fs_inbox_ep < 0) {
        cmd_fs_bind_inbox("auto");
        if (shell_fs_inbox_cap < 0) {
            sh_puts("fs up: inbox failed\r\n");
            return;
        }
        created_inbox = 1;
    } else {
        sh_puts("  inbox: already ready\r\n");
    }

    if (shell_fs_ns_cap < 0 && shell_fs_ns_task < 0 &&
        shell_fs_ns_ep < 0) {
        cmd_fs_ns_start();
        if (shell_fs_ns_cap < 0) {
            sh_puts("fs up: name-server failed\r\n");
            if (created_inbox) {
                cmd_fs_bind_inbox("clear");
            }
            return;
        }
        created_ns = 1;
    } else {
        sh_puts("  name-server: already ready\r\n");
    }

    if (shell_fs_cap < 0 && shell_fs_task < 0 && shell_fs_ep < 0) {
        cmd_fs_start();
        if (shell_fs_cap < 0) {
            sh_puts("fs up: service failed\r\n");
            if (created_ns) {
                cmd_fs_ns_stop();
            }
            if (created_inbox) {
                cmd_fs_bind_inbox("clear");
            }
            return;
        }
    } else {
        sh_puts("  service: already ready\r\n");
    }

    sh_puts("User FS stack ready\r\n");
    supervisor_set_health(shell_fs_supervisor_get(), KERN_OK);
    {
        cap_id_t service_cap = KERN_INVALID_ID;
        int live_lookup = 0;
        int health = cmd_fs_acquire_service(&service_cap, &live_lookup);
        if (health == KERN_OK) {
            health = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
            cmd_fs_release_service(live_lookup, service_cap);
        }
        supervisor_set_health(shell_fs_supervisor_get(), health);
    }
    cmd_fs_status();
}

static void cmd_fs_down(void) {
    sh_puts("User FS stack down\r\n");
    supervisor_set_health(shell_fs_supervisor_get(), KERN_ERR_STATE);

    if (shell_fs_task >= 0 || shell_fs_ep >= 0 || shell_fs_cap >= 0) {
        cmd_fs_stop();
    } else {
        sh_puts("  service: already stopped\r\n");
    }

    if (shell_fs_ns_task >= 0 || shell_fs_ns_ep >= 0 ||
        shell_fs_ns_cap >= 0) {
        cmd_fs_ns_stop();
    } else {
        sh_puts("  name-server: already stopped\r\n");
    }

    if (shell_fs_inbox_ep >= 0 || shell_fs_inbox_cap >= 0) {
        cmd_fs_bind_inbox("clear");
    } else {
        sh_puts("  inbox: already cleared\r\n");
    }

    sh_puts("User FS stack stopped\r\n");
    cmd_fs_status();
}

static void cmd_fs_restart(void) {
    sh_puts("User FS stack restart\r\n");
    supervisor_record_restart(shell_fs_supervisor_get());
    cmd_fs_down();
    cmd_fs_up();
}

static void cmd_fs_health(void) {
    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;

    sh_puts("User FS health\r\n");
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    supervisor_set_health(shell_fs_supervisor_get(), err);
    if (err != KERN_OK) {
        sh_puts("  state: ");
        sh_puts(err == KERN_ERR_STATE ? "stopped" : fs_error_name(err));
        sh_puts("\r\n");
        cmd_fs_status();
        return;
    }

    err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
    cmd_fs_release_service(live_lookup, service_cap);
    supervisor_set_health(shell_fs_supervisor_get(), err);
    sh_puts("  ping: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    cmd_fs_status();
}

static void cmd_fs_recover(void) {
    cap_id_t service_cap = KERN_INVALID_ID;
    int live_lookup = 0;

    sh_puts("User FS stack recover\r\n");
    supervisor_record_recover(shell_fs_supervisor_get());
    int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
    supervisor_set_health(shell_fs_supervisor_get(), err);
    if (err != KERN_OK) {
        sh_puts("  health: ");
        sh_puts(err == KERN_ERR_STATE ? "stopped" : fs_error_name(err));
        sh_puts("\r\n");
        cmd_fs_up();
        return;
    }

    err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
    cmd_fs_release_service(live_lookup, service_cap);
    supervisor_set_health(shell_fs_supervisor_get(), err);
    sh_puts("  ping: ");
    sh_puts(fs_error_name(err));
    sh_puts("\r\n");
    if (err == KERN_OK) {
        supervisor_set_health(shell_fs_supervisor_get(), KERN_OK);
        sh_puts("User FS stack healthy\r\n");
        cmd_fs_status();
        return;
    }

    cmd_fs_restart();
}

static void cmd_fs(int argc, char **argv) {
    if (argc <= 1 || strcmp(argv[1], "abi") == 0) {
        cmd_fs_abi();
        return;
    }
    if (strcmp(argv[1], "status") == 0) {
        cmd_fs_status();
        return;
    }
    if (strcmp(argv[1], "up") == 0) {
        cmd_fs_up();
        return;
    }
    if (strcmp(argv[1], "down") == 0) {
        cmd_fs_down();
        return;
    }
    if (strcmp(argv[1], "restart") == 0) {
        cmd_fs_restart();
        return;
    }
    if (strcmp(argv[1], "health") == 0) {
        cmd_fs_health();
        return;
    }
    if (strcmp(argv[1], "recover") == 0) {
        cmd_fs_recover();
        return;
    }
    if (strcmp(argv[1], "start") == 0) {
        cmd_fs_start();
        return;
    }
    if (strcmp(argv[1], "stop") == 0) {
        cmd_fs_stop();
        return;
    }
    if (strcmp(argv[1], "probe") == 0) {
        cmd_fs_probe();
        return;
    }
    if (strcmp(argv[1], "lookup") == 0) {
        cmd_fs_lookup();
        return;
    }
    if (strcmp(argv[1], "registered") == 0) {
        cmd_fs_registered();
        return;
    }
    if (strcmp(argv[1], "ls") == 0) {
        cmd_fs_ls((argc > 2) ? argv[2] : "/");
        return;
    }
    if (strcmp(argv[1], "cat") == 0) {
        cmd_fs_cat((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "write") == 0) {
        cmd_fs_write_file(argc, argv);
        return;
    }
    if (strcmp(argv[1], "rm") == 0) {
        cmd_fs_rm((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "mkdir") == 0) {
        cmd_fs_mkdir((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "stat") == 0) {
        cmd_fs_stat((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "bind-inbox") == 0) {
        cmd_fs_bind_inbox((argc > 2) ? argv[2] : NULL);
        return;
    }
    if (strcmp(argv[1], "ns-start") == 0) {
        cmd_fs_ns_start();
        return;
    }
    if (strcmp(argv[1], "ns-stop") == 0) {
        cmd_fs_ns_stop();
        return;
    }

    sh_puts("Usage: fs [abi|status|up|down|restart|health|recover|start|stop|probe|lookup|registered|ls [path]|cat <file>|write <file> <text>|rm <file>|mkdir <dir>|stat <path>|bind-inbox <auto|clear>|ns-start|ns-stop]\r\n");
}

#endif /* VFS_ENABLE && CAP_ENABLE */

/*============================================================================
 * 内置命令: svc
 *============================================================================*/

#if DRIVER_ENABLE || CAP_ENABLE

static void cmd_svc_print_row(const supervisor_service_t *svc,
                              const char *kind,
                              task_id_t task,
                              int lookup_ready,
                              const char *(*err_name)(int)) {
    sh_puts("  ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");

    sh_puts("    type: ");
    sh_puts(kind);
    sh_puts("\r\n");

    sh_puts("    task: ");
    if (task < 0) {
        sh_puts("none");
    } else {
        sh_putdec((uint32_t)task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(task)));
        sh_puts(")");
    }
    sh_puts("\r\n");

    sh_puts("    lookup: ");
    sh_puts(lookup_ready == KERN_OK ? "ready" : err_name(lookup_ready));
    sh_puts("\r\n");

    sh_puts("    restarts: ");
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("  recovers: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("  faults: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");

    sh_puts("    pending: ");
    sh_putdec(supervisor_pending_clients(svc));
    sh_puts("  health: ");
    sh_puts(err_name(supervisor_last_health(svc)));
    sh_puts("\r\n");

    sh_puts("    policy: ");
    sh_puts(supervisor_restart_policy_name(supervisor_restart_policy(svc)));
    sh_puts("  max restarts: ");
    sh_putdec(supervisor_max_restarts(svc));
    sh_puts("\r\n");
}

static void cmd_svc_print_generic(const supervisor_service_t *svc) {
    sh_puts("  ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
    sh_puts("    type: service\r\n");
    sh_puts("    task: unknown\r\n");
    sh_puts("    lookup: unknown\r\n");
    sh_puts("    restarts: ");
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("  recovers: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("  faults: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");
    sh_puts("    pending: ");
    sh_putdec(supervisor_pending_clients(svc));
    sh_puts("  health: ");
    sh_putdec((uint32_t)supervisor_last_health(svc));
    sh_puts("\r\n");
    sh_puts("    policy: ");
    sh_puts(supervisor_restart_policy_name(supervisor_restart_policy(svc)));
    sh_puts("  max restarts: ");
    sh_putdec(supervisor_max_restarts(svc));
    sh_puts("\r\n");
}

static void cmd_svc_register_defaults(void) {
#if DRIVER_ENABLE
    (void)shell_driver_supervisor_get();
#endif
#if CAP_ENABLE
    (void)shell_fs_supervisor_get();
#endif
}

static int cmd_svc_check_health(supervisor_service_t *svc);
static const char *cmd_svc_health_name(const supervisor_service_t *svc,
                                       int err);

static void cmd_svc_policy(int argc, char **argv) {
    if (argc < 4 || argc > 5) {
        sh_puts("Usage: svc policy <service> <manual|auto> [max]\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc policy: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    supervisor_restart_policy_t policy;
    int err = supervisor_parse_restart_policy(argv[3], &policy);
    if (err != KERN_OK) {
        sh_puts("svc policy: invalid policy\r\n");
        return;
    }

    uint32_t max_restarts = 0;
    if (policy == SUPERVISOR_RESTART_AUTO && argc >= 5) {
        max_restarts = parse_dec(argv[4]);
    }

    supervisor_set_restart_policy(svc, policy, max_restarts);
    sh_puts("User service policy updated\r\n");
    sh_puts("  service: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
    sh_puts("  policy: ");
    sh_puts(supervisor_restart_policy_name(supervisor_restart_policy(svc)));
    sh_puts("\r\n");
    sh_puts("  max restarts: ");
    sh_putdec(supervisor_max_restarts(svc));
    sh_puts("\r\n");
}

static void cmd_svc_reset(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc reset <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc reset: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    int err = cmd_svc_check_health(svc);
    supervisor_reset_service(svc, err);
    sh_puts("User service supervisor reset\r\n");
    sh_puts("  service: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
    sh_puts("  policy: ");
    sh_puts(supervisor_restart_policy_name(supervisor_restart_policy(svc)));
    sh_puts("\r\n");
    sh_puts("  restarts: ");
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("  recovers: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("  faults: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");
    sh_puts("  health: ");
    sh_puts(cmd_svc_health_name(svc, supervisor_last_health(svc)));
    sh_puts("\r\n");
}

static void cmd_svc_clear(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc clear <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc clear: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    supervisor_clear_counts(svc);
    sh_puts("User service counters cleared\r\n");
    sh_puts("  service: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
    sh_puts("  policy: ");
    sh_puts(supervisor_restart_policy_name(supervisor_restart_policy(svc)));
    sh_puts("  max restarts: ");
    sh_putdec(supervisor_max_restarts(svc));
    sh_puts("\r\n");
    sh_puts("  restarts: ");
    sh_putdec(supervisor_restart_count(svc));
    sh_puts("  recovers: ");
    sh_putdec(supervisor_recover_count(svc));
    sh_puts("  faults: ");
    sh_putdec(supervisor_fault_count(svc));
    sh_puts("\r\n");
    sh_puts("  health: ");
    sh_puts(cmd_svc_health_name(svc, supervisor_last_health(svc)));
    sh_puts("\r\n");
}

static void cmd_svc_recover(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc recover <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc recover: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_recover();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_recover();
        return;
    }
#endif

    sh_puts("svc recover: no recover handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_start(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc start <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc start: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_up();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_up();
        return;
    }
#endif

    sh_puts("svc start: no start handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_health(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc health <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc health: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_health();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_health();
        return;
    }
#endif

    sh_puts("svc health: no health handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_probe_service(supervisor_service_t *svc) {
#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_probe(supervisor_service_name(svc));
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_probe();
        return;
    }
#endif

    sh_puts("svc probe: no probe handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_probe(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc probe <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc probe: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    cmd_svc_probe_service(svc);
}

static int cmd_svc_check_health(supervisor_service_t *svc) {
    if (svc == NULL) {
        return KERN_ERR_PARAM;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        int err = cmd_driver_health_probe();
        supervisor_set_health(svc, err);
        return err;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cap_id_t service_cap = KERN_INVALID_ID;
        int live_lookup = 0;
        int err = cmd_fs_acquire_service(&service_cap, &live_lookup);
        if (err == KERN_OK) {
            err = fs_ping(service_cap, FS_SHELL_PROBE_TIMEOUT);
            cmd_fs_release_service(live_lookup, service_cap);
        }
        supervisor_set_health(svc, err);
        return err;
    }
#endif

    supervisor_set_health(svc, KERN_ERR_NOEXIST);
    return KERN_ERR_NOEXIST;
}

static const char *cmd_svc_health_name(const supervisor_service_t *svc,
                                       int err) {
#if DRIVER_ENABLE
    if (svc != NULL &&
        strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        return err == KERN_ERR_STATE ? "stopped" : driver_error_name(err);
    }
#endif
#if CAP_ENABLE
    if (svc != NULL &&
        strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        return err == KERN_ERR_STATE ? "stopped" : fs_error_name(err);
    }
#endif

    return err == KERN_OK ? "ok" : "error";
}

static void cmd_svc_restart_service(supervisor_service_t *svc) {
#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_restart();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_restart();
        return;
    }
#endif

    sh_puts("svc supervise: no restart handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_supervise_one(supervisor_service_t *svc) {
    if (svc == NULL) {
        return;
    }

    int err = cmd_svc_check_health(svc);
    sh_puts("  ");
    sh_puts(supervisor_service_name(svc));
    sh_puts(": ");
    sh_puts(cmd_svc_health_name(svc, err));

    if (err == KERN_OK) {
        sh_puts("\r\n");
        return;
    }

    if (supervisor_restart_policy(svc) != SUPERVISOR_RESTART_AUTO) {
        sh_puts(" manual\r\n");
        return;
    }

    if (!supervisor_should_auto_restart(svc)) {
        sh_puts(" restart limit\r\n");
        return;
    }

    sh_puts(" restarting\r\n");
    cmd_svc_restart_service(svc);
}

static void cmd_svc_supervise(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        sh_puts("Usage: svc supervise [service]\r\n");
        return;
    }

    cmd_svc_register_defaults();

    sh_puts("User service supervise\r\n");
    if (argc == 3) {
        supervisor_service_t *svc = supervisor_find_service(argv[2]);
        if (svc == NULL) {
            sh_puts("svc supervise: service not found: ");
            sh_puts(argv[2]);
            sh_puts("\r\n");
            return;
        }
        cmd_svc_supervise_one(svc);
        return;
    }

    uint32_t count = supervisor_service_count();
    for (uint32_t i = 0; i < count; i++) {
        cmd_svc_supervise_one(supervisor_service_at(i));
    }
}

static int cmd_svc_service_has_task(const supervisor_service_t *svc) {
    if (svc == NULL) {
        return 0;
    }
#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        return shell_driver_uart_task >= 0;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        return shell_fs_task >= 0;
    }
#endif
    return 0;
}

static void cmd_svc_stats(int argc, char **argv) {
    if (argc != 2) {
        sh_puts("Usage: svc stats\r\n");
        return;
    }

    (void)argv;
    cmd_svc_register_defaults();

    uint32_t total = supervisor_service_count();
    uint32_t running = 0;
    uint32_t unhealthy = 0;
    uint32_t restarts = 0;
    uint32_t recovers = 0;
    uint32_t faults = 0;

    for (uint32_t i = 0; i < total; i++) {
        supervisor_service_t *svc = supervisor_service_at(i);
        if (svc == NULL) {
            continue;
        }
        if (cmd_svc_service_has_task(svc)) {
            running++;
        }
        if (supervisor_last_health(svc) != KERN_OK) {
            unhealthy++;
        }
        restarts += supervisor_restart_count(svc);
        recovers += supervisor_recover_count(svc);
        faults += supervisor_fault_count(svc);
    }

    sh_puts("User service stats\r\n");
    sh_puts("  services: ");
    sh_putdec(total);
    sh_puts("\r\n");
    sh_puts("  running: ");
    sh_putdec(running);
    sh_puts("  unhealthy: ");
    sh_putdec(unhealthy);
    sh_puts("\r\n");
    sh_puts("  restarts: ");
    sh_putdec(restarts);
    sh_puts("  recovers: ");
    sh_putdec(recovers);
    sh_puts("  faults: ");
    sh_putdec(faults);
    sh_puts("\r\n");
}

static task_id_t shell_svc_runtime_task = KERN_INVALID_ID;
static volatile uint32_t shell_svc_runtime_period = SVC_RUNTIME_DEFAULT_PERIOD;
static volatile uint32_t shell_svc_runtime_ticks = 0U;
static volatile uint32_t shell_svc_runtime_sweeps = 0U;
static volatile uint32_t shell_svc_runtime_checks = 0U;
static volatile uint32_t shell_svc_runtime_actions = 0U;
static volatile int shell_svc_runtime_last_health = KERN_ERR_STATE;
static const char *shell_svc_runtime_last_service = "none";
static const char *shell_svc_runtime_last_action = "none";

typedef enum {
    SVC_RUNTIME_MODE_TICK = 0,
    SVC_RUNTIME_MODE_HEALTH = 1,
    SVC_RUNTIME_MODE_AUTO = 2,
} svc_runtime_mode_t;

static volatile svc_runtime_mode_t shell_svc_runtime_mode =
    SVC_RUNTIME_MODE_TICK;

static void svc_runtime_task(void *arg);

static const char *svc_runtime_mode_name(svc_runtime_mode_t mode) {
    if (mode == SVC_RUNTIME_MODE_AUTO) {
        return "auto-restart";
    }
    return mode == SVC_RUNTIME_MODE_HEALTH ? "health-sweep" : "tick-only";
}

static int svc_runtime_parse_mode(const char *name,
                                  svc_runtime_mode_t *out_mode) {
    if (name == NULL || out_mode == NULL) {
        return KERN_ERR_PARAM;
    }
    if (strcmp(name, "tick") == 0 || strcmp(name, "tick-only") == 0) {
        *out_mode = SVC_RUNTIME_MODE_TICK;
        return KERN_OK;
    }
    if (strcmp(name, "health") == 0 || strcmp(name, "health-sweep") == 0) {
        *out_mode = SVC_RUNTIME_MODE_HEALTH;
        return KERN_OK;
    }
    if (strcmp(name, "auto") == 0 || strcmp(name, "auto-restart") == 0) {
        *out_mode = SVC_RUNTIME_MODE_AUTO;
        return KERN_OK;
    }
    return KERN_ERR_PARAM;
}

static void svc_runtime_health_sweep(void) {
    cmd_svc_register_defaults();

    uint32_t count = supervisor_service_count();
    for (uint32_t i = 0; i < count; i++) {
        supervisor_service_t *svc = supervisor_service_at(i);
        if (svc == NULL) {
            continue;
        }
        int err = cmd_svc_check_health(svc);
        shell_svc_runtime_last_service = supervisor_service_name(svc);
        shell_svc_runtime_last_health = err;
        shell_svc_runtime_checks++;

        if (shell_svc_runtime_mode != SVC_RUNTIME_MODE_AUTO) {
            shell_svc_runtime_last_action = "observe";
            continue;
        }
        if (err == KERN_OK) {
            shell_svc_runtime_last_action = "ok";
            continue;
        }
        if (supervisor_restart_policy(svc) != SUPERVISOR_RESTART_AUTO) {
            shell_svc_runtime_last_action = "manual";
            continue;
        }
        if (!supervisor_should_auto_restart(svc)) {
            shell_svc_runtime_last_action = "restart-limit";
            continue;
        }

        shell_svc_runtime_last_action = "restart";
        shell_svc_runtime_actions++;
        cmd_svc_restart_service(svc);
    }

    shell_svc_runtime_sweeps++;
}

static int svc_runtime_start_task(uint32_t period, svc_runtime_mode_t mode) {
    if (period == 0U || period > SVC_RUNTIME_MAX_PERIOD) {
        return KERN_ERR_PARAM;
    }
    if (shell_svc_runtime_task >= 0) {
        return KERN_ERR_STATE;
    }

    shell_svc_runtime_period = period;
    shell_svc_runtime_ticks = 0U;
    shell_svc_runtime_sweeps = 0U;
    shell_svc_runtime_checks = 0U;
    shell_svc_runtime_actions = 0U;
    shell_svc_runtime_last_health = KERN_ERR_STATE;
    shell_svc_runtime_last_service = "none";
    shell_svc_runtime_last_action = "none";
    shell_svc_runtime_mode = mode;

    task_id_t tid = task_create("svc.rt", svc_runtime_task, NULL,
                                SVC_RUNTIME_PRIORITY,
                                SVC_RUNTIME_STACK_SIZE);
    if (tid < 0) {
        return KERN_ERR_RESOURCE;
    }

    int err = task_start(tid);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        return err;
    }

    shell_svc_runtime_task = tid;
    return KERN_OK;
}

static int svc_runtime_stop_task(void) {
    if (shell_svc_runtime_task < 0) {
        return KERN_ERR_STATE;
    }

    task_id_t tid = shell_svc_runtime_task;
    shell_svc_runtime_task = KERN_INVALID_ID;
    int err = task_delete(tid);
    if (err != KERN_OK) {
        shell_svc_runtime_task = tid;
    }
    return err;
}

static void svc_runtime_task(void *arg) {
    (void)arg;

    for (;;) {
        uint32_t period = shell_svc_runtime_period;
        if (period == 0U) {
            period = SVC_RUNTIME_DEFAULT_PERIOD;
        }
        (void)task_delay(period);
        shell_svc_runtime_ticks++;
        if (shell_svc_runtime_mode == SVC_RUNTIME_MODE_HEALTH ||
            shell_svc_runtime_mode == SVC_RUNTIME_MODE_AUTO) {
            svc_runtime_health_sweep();
        }
    }
}

static void cmd_svc_runtime_status(void) {
    sh_puts("User service runtime\r\n");
    sh_puts("  state: ");
    sh_puts(shell_svc_runtime_task < 0 ? "stopped\r\n" : "running\r\n");
    sh_puts("  task: ");
    if (shell_svc_runtime_task < 0) {
        sh_puts("none\r\n");
    } else {
        sh_putdec((uint32_t)shell_svc_runtime_task);
        sh_puts(" (");
        sh_puts(state_str(task_get_state(shell_svc_runtime_task)));
        sh_puts(")\r\n");
    }
    sh_puts("  period ticks: ");
    sh_putdec((uint32_t)shell_svc_runtime_period);
    sh_puts("\r\n");
    sh_puts("  runtime ticks: ");
    sh_putdec((uint32_t)shell_svc_runtime_ticks);
    sh_puts("\r\n");
    sh_puts("  mode: ");
    sh_puts(svc_runtime_mode_name(shell_svc_runtime_mode));
    sh_puts("\r\n");
    sh_puts("  sweeps: ");
    sh_putdec((uint32_t)shell_svc_runtime_sweeps);
    sh_puts("  checks: ");
    sh_putdec((uint32_t)shell_svc_runtime_checks);
    sh_puts("\r\n");
    sh_puts("  actions: ");
    sh_putdec((uint32_t)shell_svc_runtime_actions);
    sh_puts("\r\n");
    sh_puts("  last service: ");
    sh_puts(shell_svc_runtime_last_service);
    sh_puts("\r\n");
    sh_puts("  last health: ");
    sh_puts(cmd_svc_health_name(NULL, shell_svc_runtime_last_health));
    sh_puts("\r\n");
    sh_puts("  last action: ");
    sh_puts(shell_svc_runtime_last_action);
    sh_puts("\r\n");
}

static void cmd_svc_runtime(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        sh_puts("Usage: svc runtime <start|stop|status> [period-ticks] [tick|health|auto]\r\n");
        return;
    }

    if (strcmp(argv[2], "status") == 0) {
        if (argc != 3) {
            sh_puts("Usage: svc runtime status\r\n");
            return;
        }
        cmd_svc_runtime_status();
        return;
    }

    if (strcmp(argv[2], "start") == 0) {
        uint32_t period = SVC_RUNTIME_DEFAULT_PERIOD;
        svc_runtime_mode_t mode = SVC_RUNTIME_MODE_TICK;
        if (argc >= 4) {
            period = parse_dec(argv[3]);
            if (period == 0U || period > SVC_RUNTIME_MAX_PERIOD) {
                sh_puts("svc runtime: period must be 1..10000 ticks\r\n");
                return;
            }
        }
        if (argc == 5 &&
            svc_runtime_parse_mode(argv[4], &mode) != KERN_OK) {
            sh_puts("svc runtime: mode must be tick, health, or auto\r\n");
            return;
        }

        if (shell_svc_runtime_task >= 0) {
            sh_puts("svc runtime: already running\r\n");
            cmd_svc_runtime_status();
            return;
        }

        int err = svc_runtime_start_task(period, mode);
        if (err != KERN_OK) {
            sh_puts("svc runtime: start failed\r\n");
            return;
        }

        sh_puts("User service runtime started\r\n");
        cmd_svc_runtime_status();
        return;
    }

    if (strcmp(argv[2], "stop") == 0) {
        if (argc != 3) {
            sh_puts("Usage: svc runtime stop\r\n");
            return;
        }

        if (shell_svc_runtime_task < 0) {
            sh_puts("svc runtime: already stopped\r\n");
            cmd_svc_runtime_status();
            return;
        }

        int err = svc_runtime_stop_task();
        if (err != KERN_OK) {
            sh_puts("svc runtime: stop failed\r\n");
            return;
        }

        sh_puts("User service runtime stopped\r\n");
        cmd_svc_runtime_status();
        return;
    }

    sh_puts("Usage: svc runtime <start|stop|status> [period-ticks] [tick|health|auto]\r\n");
}

#if TEST_ENABLE
int shell_svc_runtime_selftest(void) {
    svc_runtime_mode_t mode = SVC_RUNTIME_MODE_TICK;
    if (svc_runtime_parse_mode("bad", &mode) == KERN_OK) {
        return KERN_ERR_PARAM;
    }

    if (shell_svc_runtime_task >= 0) {
        (void)svc_runtime_stop_task();
    }

    int err = svc_runtime_start_task(5U, SVC_RUNTIME_MODE_TICK);
    if (err != KERN_OK) {
        return err;
    }
    (void)task_delay(30U);
    if (shell_svc_runtime_ticks == 0U ||
        shell_svc_runtime_sweeps != 0U ||
        shell_svc_runtime_checks != 0U) {
        (void)svc_runtime_stop_task();
        return KERN_ERR_STATE;
    }
    err = svc_runtime_stop_task();
    if (err != KERN_OK) {
        return err;
    }

    err = svc_runtime_start_task(5U, SVC_RUNTIME_MODE_HEALTH);
    if (err != KERN_OK) {
        return err;
    }
    (void)task_delay(30U);
    if (shell_svc_runtime_ticks == 0U ||
        shell_svc_runtime_sweeps == 0U ||
        shell_svc_runtime_checks == 0U ||
        shell_svc_runtime_last_service == NULL) {
        (void)svc_runtime_stop_task();
        return KERN_ERR_STATE;
    }
    err = svc_runtime_stop_task();
    if (err != KERN_OK) {
        return err;
    }

    err = svc_runtime_start_task(5U, SVC_RUNTIME_MODE_AUTO);
    if (err != KERN_OK) {
        return err;
    }
    (void)task_delay(30U);
    if (shell_svc_runtime_ticks == 0U ||
        shell_svc_runtime_sweeps == 0U ||
        shell_svc_runtime_checks == 0U ||
        shell_svc_runtime_actions != 0U ||
        shell_svc_runtime_last_action == NULL) {
        (void)svc_runtime_stop_task();
        return KERN_ERR_STATE;
    }
    return svc_runtime_stop_task();
}
#endif

static void cmd_svc_restart(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc restart <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc restart: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_restart();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_restart();
        return;
    }
#endif

    sh_puts("svc restart: no restart handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_down(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc down <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc down: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_down();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_down();
        return;
    }
#endif

    sh_puts("svc down: no down handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_fault_service(supervisor_service_t *svc) {
#if DRIVER_ENABLE
    if (strcmp(supervisor_service_name(svc),
               DRIVER_SHELL_SERVICE_NAME) == 0) {
        cmd_driver_uart_fault();
        return;
    }
#endif
#if CAP_ENABLE
    if (strcmp(supervisor_service_name(svc),
               FS_SHELL_SERVICE_NAME) == 0) {
        cmd_fs_fault();
        return;
    }
#endif

    sh_puts("svc fault: no fault handler: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
}

static void cmd_svc_fault(int argc, char **argv) {
    if (argc != 3) {
        sh_puts("Usage: svc fault <service>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc fault: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    cmd_svc_fault_service(svc);
}

static void cmd_svc_stress(int argc, char **argv) {
    if (argc != 4) {
        sh_puts("Usage: svc stress <service> <loops>\r\n");
        return;
    }

    cmd_svc_register_defaults();

    supervisor_service_t *svc = supervisor_find_service(argv[2]);
    if (svc == NULL) {
        sh_puts("svc stress: service not found: ");
        sh_puts(argv[2]);
        sh_puts("\r\n");
        return;
    }

    uint32_t loops = parse_dec(argv[3]);
    if (loops == 0U || loops > 10U) {
        sh_puts("svc stress: loops must be 1..10\r\n");
        return;
    }

    sh_puts("User service stress\r\n");
    sh_puts("  service: ");
    sh_puts(supervisor_service_name(svc));
    sh_puts("\r\n");
    sh_puts("  loops: ");
    sh_putdec(loops);
    sh_puts("\r\n");

    for (uint32_t i = 0; i < loops; i++) {
        sh_puts("  loop: ");
        sh_putdec(i + 1U);
        sh_puts("\r\n");
        cmd_svc_fault_service(svc);
        cmd_svc_supervise_one(svc);
    }

    sh_puts("User service stress probe\r\n");
    cmd_svc_probe_service(svc);
    cmd_svc_stats(2, NULL);
}

static void cmd_svc(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "policy") == 0) {
        cmd_svc_policy(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        cmd_svc_reset(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "clear") == 0) {
        cmd_svc_clear(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "recover") == 0) {
        cmd_svc_recover(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "runtime") == 0) {
        cmd_svc_runtime(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "start") == 0) {
        cmd_svc_start(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "health") == 0) {
        cmd_svc_health(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "probe") == 0) {
        cmd_svc_probe(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "supervise") == 0) {
        cmd_svc_supervise(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "stats") == 0) {
        cmd_svc_stats(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "stress") == 0) {
        cmd_svc_stress(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "restart") == 0) {
        cmd_svc_restart(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "down") == 0) {
        cmd_svc_down(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        cmd_svc_down(argc, argv);
        return;
    }
    if (argc > 1 && strcmp(argv[1], "fault") == 0) {
        cmd_svc_fault(argc, argv);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "status") != 0) {
        sh_puts("Usage: svc [status|stats|runtime <start|stop|status> [period]|stress <service> <loops>|supervise [service]|health <service>|probe <service>|policy <service> <manual|auto> [max]|reset <service>|clear <service>|start <service>|recover <service>|restart <service>|down|stop <service>|fault <service>]\r\n");
        return;
    }

    cmd_svc_register_defaults();

    sh_puts("User services\r\n");
    uint32_t count = supervisor_service_count();
    for (uint32_t i = 0; i < count; i++) {
        supervisor_service_t *svc = supervisor_service_at(i);
        if (svc == NULL) {
            continue;
        }
#if DRIVER_ENABLE
        if (strcmp(supervisor_service_name(svc),
                   DRIVER_SHELL_SERVICE_NAME) == 0) {
            cmd_svc_print_row(svc, "driver",
                              shell_driver_uart_task,
                              driver_runtime_lookup_ready(
                                  DRIVER_SHELL_PROBE_TIMEOUT, NULL),
                              driver_error_name);
            continue;
        }
#endif
#if CAP_ENABLE
        if (strcmp(supervisor_service_name(svc),
                   FS_SHELL_SERVICE_NAME) == 0) {
            cmd_svc_print_row(svc, "fs",
                              shell_fs_task,
                              fs_runtime_lookup_ready(FS_SHELL_PROBE_TIMEOUT,
                                                      NULL),
                              fs_error_name);
            continue;
        }
#endif
        cmd_svc_print_generic(svc);
    }
}

#endif /* DRIVER_ENABLE || CAP_ENABLE */

/*============================================================================
 * 命令表
 *============================================================================*/

const shell_cmd_t cmd_table[] = {
    { "help",     "List commands",              cmd_help     },
    { "ls",       "[path] List directory",      cmd_ls       },
    { "cat",      "<file> Show file",           cmd_cat      },
    { "echo",     "<text> Print text",          cmd_echo     },
    { "clear",    "Clear screen",               cmd_clear    },
    { "ps",       "List tasks",                 cmd_ps       },
#if KERN_TASK_STATS
    { "top",      "Task CPU usage",             cmd_top      },
#endif
#if TRACE_ENABLE
    { "trace",    "[n] [event]|clear Trace",    cmd_trace    },
#endif
    { "crash",    "Last crash dump",            cmd_crash    },
    { "mem",      "Memory layout & stacks",     cmd_mem      },
#if KERN_TASK_STATS
    { "stats",    "[clear] Kernel statistics",  cmd_stats    },
#endif
#if DRIVER_ENABLE
    { "dev",      "List devices",               cmd_dev      },
    { "driver",   "[abi|status [svc]] Driver",  cmd_driver   },
#endif
#if CAP_ENABLE   /* Phase F3: fs 命令走 fs_server IPC,不依赖内核 VFS */
    { "fs",       "[up|down|probe|ls] FS",      cmd_fs       },
#endif
#if DRIVER_ENABLE || CAP_ENABLE
    { "svc",      "Service supervisor",         cmd_svc      },
#endif
    { "free",     "Memory usage",               cmd_free     },
    { "hexdump",  "<addr> <len> Hex dump",      cmd_hexdump  },
    { "version",  "Kernel version",             cmd_version  },
    { "reset",    "System reset",               cmd_reset    },
};

const int cmd_count = (int)(sizeof(cmd_table) / sizeof(cmd_table[0]));

/*============================================================================
 * 行编辑器
 *============================================================================*/

static int shell_read_line(char *buf, int max) {
    int i = 0;
    for (;;) {
        char c = uart_getc(SHELL_UART);

        if (c == '\r' || c == '\n') {
            sh_puts("\r\n");
            buf[i] = '\0';
            return i;
        }

        /* Backspace / DEL */
        if (c == '\b' || c == 127) {
            if (i > 0) {
                i--;
                sh_puts("\b \b");
            }
            continue;
        }

        /* 可打印字符 */
        if (c >= 0x20 && i < max - 1) {
            buf[i++] = c;
            sh_putc(c);
        }
    }
}

/*============================================================================
 * 命令解析 + 分发
 *============================================================================*/

int shell_split(char *line, char **argv, int max) {
    int argc = 0;
    while (*line) {
        while (*line == ' ') line++;
        if (*line == '\0') break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = '\0';
        if (argc >= max) break;
    }
    return argc;
}

void shell_exec(char *line) {
    char *argv[SHELL_ARGV_MAX];
    int argc = shell_split(line, argv, SHELL_ARGV_MAX);
    if (argc == 0) return;

    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(argv[0], cmd_table[i].name) == 0) {
            cmd_table[i].func(argc, argv);
            return;
        }
    }

    sh_puts("Unknown command: ");
    sh_puts(argv[0]);
    sh_puts("\r\nType 'help' for commands.\r\n");
}

/*============================================================================
 * Shell 任务
 *============================================================================*/

static char line_buf[SHELL_LINE_MAX];

static void shell_task(void *arg) {
    (void)arg;

    /* 清除 UART RX 缓冲区中的残留数据，防止噪声导致假输入 */
    while (uart_readable(SHELL_UART)) {
        uart_getc(SHELL_UART);
    }

    sh_puts("\r\n");
    sh_puts("========================================\r\n");
    sh_puts("  My-RTOS Shell v1.0\r\n");
    sh_puts("  Type 'help' for available commands.\r\n");
    sh_puts("========================================\r\n");
    sh_puts("\r\n");

    for (;;) {
        sh_puts(SHELL_PROMPT);
        int len = shell_read_line(line_buf, SHELL_LINE_MAX);
        if (len > 0)
            shell_exec(line_buf);
    }
}

/*============================================================================
 * 公共 API
 *============================================================================*/

void shell_start(void) {
    task_id_t tid = task_create("shell", shell_task, NULL,
                                SHELL_PRIORITY,
                                SHELL_STACK_SIZE);
    if (tid >= 0)
        task_start(tid);
}

#endif /* SHELL_ENABLE */
