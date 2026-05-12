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
#define SHELL_UART       NUCLEO_DEFAULT_UART

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
 * 内置命令: ls
 *============================================================================*/

static void cmd_ls(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";

    inode_t *dir = vfs_lookup(path);
    if (!dir) {
        sh_puts("ls: ");
        sh_puts(path);
        sh_puts(": No such file or directory\r\n");
        return;
    }

    if (dir->type != INODE_TYPE_DIR) {
        /* 普通文件: 直接打印 */
        sh_putdec(dir->ino);
        sh_puts("  ");
        sh_puts(dir->name);
        sh_puts("\r\n");
        inode_put(dir);
        return;
    }

    if (!dir->dir_ops || !dir->dir_ops->readdir) {
        sh_puts("ls: readdir not supported\r\n");
        inode_put(dir);
        return;
    }
    inode_put(dir);

    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0) {
        sh_puts("ls: ");
        sh_puts(path);
        sh_puts(": open failed\r\n");
        return;
    }

    dirent_t entry;
    while (1) {
        if (vfs_readdir(fd, &entry) != KERN_OK)
            break;

        sh_putdec_width(entry.ino, 6);
        sh_puts("  ");

        /* 类型标记 */
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

    vfs_close(fd);
}

/*============================================================================
 * 内置命令: cat
 *============================================================================*/

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        sh_puts("Usage: cat <file>\r\n");
        return;
    }

    int fd = vfs_open(argv[1], O_RDONLY);
    if (fd < 0) {
        sh_puts("cat: ");
        sh_puts(argv[1]);
        sh_puts(": Cannot open\r\n");
        return;
    }

    char buf[64];
    int32_t n;
    while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        sh_puts(buf);
    }
    sh_puts("\r\n");

    vfs_close(fd);
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
        if ((c == '\b' || c == 127) && i > 0) {
            i--;
            sh_puts("\b \b");
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
