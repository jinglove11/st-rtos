# Phase 4: 设备驱动框架 — Device Driver Framework

> 目标硬件: STM32F767ZI Nucleo (Cortex-M7)
> 创建日期: 2026-05-08
> 前置: Phase 3 已完成 (329 tests, 0 failures, 含 shell 测试)

---

## 一、设计目标

| 维度 | 当前状态 | Phase 4 目标 |
|------|---------|-------------|
| 设备抽象 | 无。驱动是裸函数 | `device_t` 统一设备描述符 |
| 驱动注册 | `devfs_register_device(name, dev_ops_t*)` 手动调用 | `device_register()` 自动绑定到 devfs |
| private_data | 指向 `dev_ops_t*` (循环引用) | 指向 `device_t*` (正确解耦) |
| UART 访问 | 直接调用 `uart_getc()/uart_putc()` | `open("/dev/uart0") → read/write` |
| GPIO 访问 | 直接调用 `gpio_init()/gpio_set()` | `open("/dev/gpio") → ioctl` |
| 驱动初始化 | `system_init.c` 阶段 4 是 TODO | 板级 `board_init_drivers()` 自动注册 |
| IRQ 集成 | IRQ/BH 基础设施存在但驱动未使用 | 设备可绑定 IRQ → BH → 驱动回调 |

---

## 二、架构总览

```
用户态 syscall
  open("/dev/uart0", O_RDWR)
  read(fd, buf, 100)
  write(fd, data, len)
  ioctl(fd, cmd, arg)
       │
       ▼
┌─────────────────────────────────────────────┐
│  VFS 层 (vfs.c)                             │
│  路径解析 → inode 查找 → fd 分配 → 能力校验  │
└──────────────────┬──────────────────────────┘
                   │ inode->type == CHRDEV
                   ▼
┌─────────────────────────────────────────────┐
│  devfs 层 (devfs.c)                         │
│  cdev_shared_fops → 二级分发                 │
│  inode->private_data → device_t*            │
│  device->ops->read(device->priv, ...)       │
└──────────────────┬──────────────────────────┘
                   │
       ┌───────────┼───────────┐
       ▼           ▼           ▼
  ┌─────────┐ ┌─────────┐ ┌─────────┐
  │ null_dev│ │ uart_dev│ │ gpio_dev│
  │ dev_ops │ │ dev_ops │ │ dev_ops │
  └─────────┘ └────┬────┘ └────┬────┘
                   │           │
                   ▼           ▼
              USART3 寄存器  GPIO 寄存器
```

### 关键分发链

```
vfs_read(fd, buf, size)
  → cap_resolve(fd) → fd_entry → inode
  → inode->ops_u.cdev_ops->read(inode, buf, offset, size)
    → cdev_read()
      → device_t *dev = inode->private_data    // Phase 4: device_t*
      → dev->ops->read(dev->priv, buf, offset, size)
        → uart_dev_read(priv, buf, offset, size)
          → uart_getc(USART3) 逐字节读取
```

---

## 三、设备描述符 device_t

### 3.1 结构定义

```c
/* src/kernel/dev/device.h */

#define DEVICE_NAME_LEN  16

typedef enum {
    DEVICE_TYPE_CHAR  = 0,    /* 字符设备 (UART, GPIO, SPI, I2C) */
    DEVICE_TYPE_BLOCK = 1,    /* 块设备 (预留) */
} device_type_t;

typedef struct {
    char            name[DEVICE_NAME_LEN];   /* 设备名 ("uart0", "gpio") */
    device_type_t   type;                     /* 设备类型 */
    dev_ops_t      *ops;                      /* 设备操作表 */
    void           *priv;                     /* 驱动私有数据 (寄存器基址等) */
    uint32_t        irq_num;                  /* 硬件 IRQ 号 (0 = 无中断) */
    uint8_t         in_use;                   /* 是否已注册 */
} device_t;
```

### 3.2 与现有 dev_ops_t 的关系

```c
/* 已有 (inode.h) — 不修改 */
typedef struct dev_ops {
    kern_err_t (*open)(void *priv, uint32_t flags);
    kern_err_t (*close)(void *priv);
    int32_t    (*read)(void *priv, void *buf, uint32_t offset, uint32_t size);
    int32_t    (*write)(void *priv, const void *buf, uint32_t offset, uint32_t size);
    kern_err_t (*ioctl)(void *priv, uint32_t cmd, void *arg);
} dev_ops_t;
```

**关键变更**: `devfs.c` 中 `cdev_shared_fops` 的分发逻辑从:
```c
// 旧: private_data 指向 dev_ops_t* (循环引用)
dev_ops_t *ops = (dev_ops_t *)inode->private_data;
ops->read(inode->private_data, buf, offset, size);  // priv = ops 自身!
```
改为:
```c
// 新: private_data 指向 device_t* (正确解耦)
device_t *dev = (device_t *)inode->private_data;
dev->ops->read(dev->priv, buf, offset, size);  // priv = 驱动私有数据
```

### 3.3 device_t vs dev_ops_t 职责划分

| 字段 | device_t 负责 | dev_ops_t 负责 |
|------|:---:|:---:|
| 设备名称 | ✅ | |
| 设备类型 | ✅ | |
| 操作函数指针 | | ✅ |
| 驱动私有数据 | ✅ | |
| IRQ 号 | ✅ | |
| 注册状态 | ✅ | |
| 硬件操作逻辑 | | ✅ |

---

## 四、设备注册表

### 4.1 静态池

```c
/* src/kernel/dev/device.c */

#define DEVICE_MAX  8

static device_t device_pool[DEVICE_MAX];
```

### 4.2 API

```c
/* src/kernel/dev/device.h */

void     device_init(void);
device_t *device_alloc(const char *name, device_type_t type);
void     device_free(device_t *dev);
device_t *device_find(const char *name);
```

### 4.3 注册流程

```
device_register("uart0", &uart_ops, uart_priv, USART3_IRQ)
  │
  ├── 1. device_alloc("uart0", DEVICE_TYPE_CHAR)
  │      └── 扫描 device_pool[] 找空闲槽
  │      └── 填充 name, type, ops, priv, irq_num
  │      └── in_use = 1
  │
  ├── 2. devfs_register_device("uart0", device_t*)
  │      └── 创建 CHRDEV inode
  │      └── inode->private_data = device_t*  (不再是 dev_ops_t*)
  │      └── inode->ops_u.cdev_ops = &cdev_shared_fops
  │
  └── return KERN_OK
```

---

## 五、devfs.c 修改

### 5.1 cdev_shared_fops 分发修改

```c
/* 修改前 (Phase 3) */
static int32_t cdev_read(struct inode *inode, void *buf,
                         uint32_t offset, uint32_t size) {
    dev_ops_t *ops = (dev_ops_t *)inode->private_data;
    if (!ops || !ops->read) return KERN_ERR;
    return ops->read(inode->private_data, buf, offset, size);
    //                   ^^^^^^^^^^^^^^^^^
    //                   priv = dev_ops_t* (循环!)
}

/* 修改后 (Phase 4) */
static int32_t cdev_read(struct inode *inode, void *buf,
                         uint32_t offset, uint32_t size) {
    device_t *dev = (device_t *)inode->private_data;
    if (!dev || !dev->ops || !dev->ops->read) return KERN_ERR;
    return dev->ops->read(dev->priv, buf, offset, size);
    //                     ^^^^^^^^^
    //                     priv = 驱动私有数据 (正确!)
}
```

### 5.2 devfs_register_device 签名修改

```c
/* 修改前 */
kern_err_t devfs_register_device(const char *name, dev_ops_t *ops);

/* 修改后 */
kern_err_t devfs_register_device(const char *name, device_t *dev);
```

### 5.3 /dev/null 适配

```c
/* /dev/null 使用独立的 device_t */
static dev_ops_t null_ops = { ... };
static device_t  null_dev = {
    .name = "null",
    .type = DEVICE_TYPE_CHAR,
    .ops  = &null_ops,
    .priv = NULL,
};

/* devfs_init 中 */
devfs_register_device("null", &null_dev);
```

---

## 六、UART 设备驱动

### 6.1 驱动架构

```
┌──────────────────────────────────────┐
│  uart_dev.c — UART 设备驱动          │
│                                      │
│  dev_ops_t uart_dev_ops = {          │
│    .open  = uart_dev_open,           │
│    .close = uart_dev_close,          │
│    .read  = uart_dev_read,           │
│    .write = uart_dev_write,          │
│    .ioctl = uart_dev_ioctl,          │
│  };                                  │
│                                      │
│  device_t uart0_dev = {              │
│    .name = "uart0",                  │
│    .ops  = &uart_dev_ops,            │
│    .priv = USART3,                   │
│    .irq_num = USART3_IRQn,           │
│  };                                  │
└──────────────────────────────────────┘
        │ 调用
        ▼
┌──────────────────────────────────────┐
│  uart_stm32.c — 硬件驱动 (已有)      │
│                                      │
│  uart_init(usart, baudrate)          │
│  uart_putc(usart, ch)                │
│  uart_getc(usart) → char             │
│  uart_puts(usart, str)               │
│  uart_readable(usart) → bool         │
│  uart_puthex/putdec(usart, val)      │
└──────────────────────────────────────┘
```

### 6.2 dev_ops 实现

```c
static kern_err_t uart_dev_open(void *priv, uint32_t flags) {
    (void)priv; (void)flags;
    return KERN_OK;  /* UART 已在 system_init 中初始化 */
}

static kern_err_t uart_dev_close(void *priv) {
    (void)priv;
    return KERN_OK;
}

static int32_t uart_dev_read(void *priv, void *buf,
                             uint32_t offset, uint32_t size) {
    (void)offset;
    usart_t *usart = (usart_t *)priv;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < size; i++) {
        p[i] = (uint8_t)uart_getc(usart);  /* 轮询读取 */
    }
    return (int32_t)size;
}

static int32_t uart_dev_write(void *priv, const void *buf,
                              uint32_t offset, uint32_t size) {
    (void)offset;
    usart_t *usart = (usart_t *)priv;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < size; i++) {
        uart_putc(usart, (char)p[i]);
    }
    return (int32_t)size;
}

static kern_err_t uart_dev_ioctl(void *priv, uint32_t cmd, void *arg) {
    (void)priv; (void)cmd; (void)arg;
    return KERN_ERR;  /* 暂不支持 ioctl */
}
```

### 6.3 注册

```c
/* board_init_drivers() 中 */
static device_t uart0_dev;
static dev_ops_t uart_dev_ops = {
    .open  = uart_dev_open,
    .close = uart_dev_close,
    .read  = uart_dev_read,
    .write = uart_dev_write,
    .ioctl = uart_dev_ioctl,
};

void board_init_drivers(void) {
    uart0_dev.ops  = &uart_dev_ops;
    uart0_dev.priv = (void *)NUCLEO_DEFAULT_UART;
    strncpy(uart0_dev.name, "uart0", DEVICE_NAME_LEN);
    uart0_dev.type = DEVICE_TYPE_CHAR;
    devfs_register_device("uart0", &uart0_dev);
}
```

---

## 七、GPIO 设备驱动

### 7.1 驱动架构

GPIO 通过 `ioctl` 操作 (非标准 read/write):

```c
/* ioctl 命令定义 */
#define GPIO_CMD_SET_PIN    0x01
#define GPIO_CMD_GET_PIN    0x02
#define GPIO_CMD_SET_DIR    0x03
#define GPIO_CMD_TOGGLE     0x04

typedef struct {
    uint32_t port;   /* GPIO 基址 */
    uint32_t pin;    /* 引脚号 */
    uint32_t value;  /* 值 */
} gpio_arg_t;
```

### 7.2 dev_ops 实现

```c
static kern_err_t gpio_dev_ioctl(void *priv, uint32_t cmd, void *arg) {
    gpio_arg_t *g = (gpio_arg_t *)arg;
    if (!g) return KERN_ERR_PARAM;

    switch (cmd) {
    case GPIO_CMD_SET_PIN:
        gpio_set((gpio_t *)g->port, g->pin, g->value);
        return KERN_OK;
    case GPIO_CMD_GET_PIN:
        g->value = gpio_get((gpio_t *)g->port, g->pin);
        return KERN_OK;
    case GPIO_CMD_TOGGLE:
        gpio_toggle((gpio_t *)g->port, g->pin);
        return KERN_OK;
    default:
        return KERN_ERR;
    }
}
```

### 7.3 注册

```c
void board_init_drivers(void) {
    /* ... UART 注册 ... */

    static device_t gpio_dev;
    static dev_ops_t gpio_ops = { NULL, NULL, NULL, NULL, gpio_dev_ioctl };
    gpio_dev.ops = &gpio_ops;
    strncpy(gpio_dev.name, "gpio", DEVICE_NAME_LEN);
    gpio_dev.type = DEVICE_TYPE_CHAR;
    devfs_register_device("gpio", &gpio_dev);
}
```

---

## 八、IRQ 集成 (基础)

### 8.1 设备 IRQ 绑定

```c
/* device.h */
kern_err_t device_bind_irq(device_t *dev, void (*handler)(void *), void *arg);
```

### 8.2 绑定流程

```
device_bind_irq(&uart0_dev, uart_isr_handler, NULL)
  │
  ├── 1. irq_register(dev->irq_num, handler, arg)
  │      └── 注册 ISR 到 IRQ 向量表
  │
  └── 2. 可选: bh_create(uart_bh, ...) 创建底半部
         └── ISR 中 bh_schedule(uart_bh)
         └── BH 在任务上下文中执行驱动逻辑
```

### 8.3 Phase 4 范围

Phase 4 实现 **基础设施**:
- `device_bind_irq()` API 声明 + 基本实现
- UART 驱动可选使用 IRQ (不强制)
- GPIO 驱动不使用 IRQ (轮询)

Phase 5+ 实现 **完整异步 I/O**:
- UART 中断驱动 RX ring buffer
- `read()` 阻塞等待数据 (semaphore)
- BH/线程化 IRQ 完整集成

---

## 九、板级初始化

### 9.1 system_init.c 阶段 4

```c
/* system_init.c — 阶段 4: 驱动初始化 */
static void system_init_drivers(const system_config_t *config) {
    (void)config;

#if VFS_ENABLE
    board_init_drivers();  /* 板级驱动注册 */
#endif
}
```

### 9.2 board_init_drivers()

```c
/* src/board/stm32f767/board_drivers.c (新增) */
#include "device.h"
#include "devfs.h"
#include "uart.h"
#include "gpio.h"
#include "nucleo_f767.h"

void board_init_drivers(void) {
    /* UART0 — USART3 (ST-Link VCP) */
    static dev_ops_t uart_ops = { ... };
    static device_t  uart0_dev;
    uart0_dev.ops  = &uart_ops;
    uart0_dev.priv = (void *)NUCLEO_DEFAULT_UART;
    strncpy(uart0_dev.name, "uart0", DEVICE_NAME_LEN);
    uart0_dev.type = DEVICE_TYPE_CHAR;
    devfs_register_device("uart0", &uart0_dev);

    /* GPIO — LED 控制 */
    static dev_ops_t gpio_ops = { ... };
    static device_t  gpio_dev;
    gpio_dev.ops = &gpio_ops;
    strncpy(gpio_dev.name, "gpio", DEVICE_NAME_LEN);
    gpio_dev.type = DEVICE_TYPE_CHAR;
    devfs_register_device("gpio", &gpio_dev);
}
```

---

## 十、文件变更

### 新增文件 (4)

| # | 文件 | 说明 |
|---|------|------|
| 1 | `src/kernel/dev/device.h` | device_t, device_type_t, device_alloc/free/find API |
| 2 | `src/kernel/dev/device.c` | 设备注册表 (静态池), device_init |
| 3 | `src/drivers/uart_dev.c` | UART 设备驱动 (dev_ops_t 包装 uart_stm32.c) |
| 4 | `src/board/stm32f767/board_drivers.c` | 板级驱动注册 (board_init_drivers) |

### 修改文件 (7)

| # | 文件 | 变更 |
|---|------|------|
| 1 | `src/kernel/vfs/devfs.c` | cdev_shared_fops 分发改为 device_t*; devfs_register_device 签名改为 device_t*; /dev/null 适配 |
| 2 | `src/kernel/vfs/devfs.h` | devfs_register_device 签名更新 |
| 3 | `src/kernel/system_init.c` | 阶段 4 调用 board_init_drivers() |
| 4 | `src/kernel/kernel.c` | kern_init() 调用 device_init() |
| 5 | `src/tests/test_vfs.c` | 适配 devfs_register_device 新签名 |
| 6 | `Kconfig` | DRIVER_ENABLE, DRIVER_MAX_DEVICES |
| 7 | `Makefile` | KERN_SOURCES += dev/device.c; APP_SOURCES += drivers/uart_dev.c + board_drivers.c |

### 新增测试 (1)

| # | 文件 | 说明 |
|---|------|------|
| 1 | `src/tests/test_driver.c` | 设备框架测试: device_alloc/free, device_register, UART read/write via VFS, GPIO ioctl |

---

## 十一、实施步骤 (8 步)

### Step 1: 配置
- Kconfig: Driver Configuration menu (DRIVER_ENABLE, DRIVER_MAX_DEVICES=8)
- .config: DRIVER_ENABLE=y
- kernel_config.h: DRIVER_ENABLE, DRIVER_MAX_DEVICES

### Step 2: device.h + device.c
- device_t 结构定义
- device_init() — 池清零
- device_alloc() — 扫描池找空闲槽
- device_free() — 释放槽位
- device_find() — 按名查找

### Step 3: devfs.c 修改
- cdev_shared_fops 全部 5 个函数改为 device_t* 分发
- devfs_register_device 签名改为 (name, device_t*)
- /dev/null 适配为 device_t

### Step 4: UART 设备驱动
- src/drivers/uart_dev.c — dev_ops_t 实现
- 封装 uart_getc/putc/puts 为 dev_ops 接口

### Step 5: 板级驱动注册
- src/board/stm32f767/board_drivers.c — board_init_drivers()
- 注册 uart0 + gpio 设备

### Step 6: 内核集成
- kernel.c: device_init() 调用
- system_init.c: 阶段 4 调用 board_init_drivers()
- Makefile: 新增源文件

### Step 7: 测试
- test_driver.c: device_t 生命周期, UART VFS 读写, GPIO ioctl
- test_vfs.c 适配: devfs_register_device 新签名

### Step 8: 硬件验证
- make BOARD=stm32f767 -j8 → 0 warnings
- Flash → 全部测试通过
- shell 中 `cat /dev/uart0` (echo 模式) 可用

---

## 十二、关键设计决策

| # | 决策 | 理由 |
|---|------|------|
| 1 | **device_t 而非 driver_t** | 嵌入式 RTOS 无动态加载, 驱动编译时静态链接, device_t 足够 |
| 2 | **静态设备池** | 避免 kmalloc, 与 inode_pool 模式一致, 可预测内存使用 |
| 3 | **private_data → device_t*** | 修复 Phase 3 的循环引用, 驱动拿到正确的设备上下文 |
| 4 | **GPIO 用 ioctl** | GPIO 操作 (set/get/toggle) 不适合 read/write 语义 |
| 5 | **board_drivers.c 在 board/ 目录** | 板级差异隔离, 新板只需实现自己的 board_drivers.c |
| 6 | **保留现有 uart_stm32.c** | 不重写硬件驱动, uart_dev.c 只是 dev_ops_t 包装层 |
| 7 | **IRQ 绑定基础实现** | Phase 4 只提供 API 和基础设施, 完整异步 I/O 留给 Phase 5 |
| 8 | **不引入 bus 抽象** | 嵌入式 RTOS 设备数量少 (<8), 无需总线枚举机制 |

---

## 十三、验证

1. `make BOARD=stm32f767 -j8` — 0 warnings
2. Flash STM32F767 — 全部测试通过 (329+ 新增)
3. `/dev/uart0` 存在, open/read/write 工作
4. `/dev/gpio` 存在, ioctl 可控 LED
5. `/dev/null` 行为不变 (回归)
6. shell `ls /dev` 显示 null, uart0, gpio
7. shell `echo hello > /dev/uart0` 输出到串口
8. Phase 1-3 + shell 全部回归通过
