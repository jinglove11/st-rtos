# 通用 AI 嵌入式硬件代理

本目录是一套独立的嵌入式开发代理，提供：

- 独占并持续读取 UART，自动重连并保存日志
- 发送目标 shell/monitor 命令并等待 prompt 或正则匹配
- 调用配置中的编译、烧录、校验和复位命令
- 通过标准 MCP 向任意兼容 AI 客户端暴露结构化工具

## 1. 创建项目配置

复制模板并修改：

```bash
cp tools/embedded-agent/config.example.json /path/to/project/board-agent.json
```

主要字段：

- `port`、`baud`：串口设备和波特率
- `prompt`：命令完成标志的正则表达式
- `working_directory`：执行板卡操作的目录；相对路径以配置文件所在目录为基准
- `actions`：`verify`、`flash`、`reset` 对应的参数数组
- `socket`、`log`：本地 API 和 UART 日志位置

命令必须写成参数数组，不经过 shell。例如：

```json
"flash": ["make", "flash"]
```

## 2. 启动通用代理

在任意项目中执行：

```bash
python3 /home/five/my-rtos/tools/embedded-agent/embedded_agent.py daemon \
  --config /path/to/project/board-agent.json
```

本项目已经提供 `my-rtos.json`，可以直接使用快捷命令：

在项目根目录执行：

```bash
make agent-start PORT=/dev/ttyACM0 BAUD=921600
make agent-status
```

My-RTOS 配置的运行文件：

- `/tmp/my-rtos-agent.sock`: local JSON API
- `/tmp/my-rtos-uart.log`: complete raw UART capture
- `/tmp/my-rtos-agent.out`: daemon diagnostics
- `/tmp/my-rtos-agent.pid`: background process ID

代理必须独占串口。运行期间不要同时启动 `serial_debug.py`、
`uart_tee.py`、picocom 或 minicom。

停止代理：

```bash
make agent-stop
```

## 3. 命令行使用

```bash
AGENT="python3 tools/embedded-agent/embedded_agent.py ctl \
  --config tools/embedded-agent/my-rtos.json"

$AGENT status
$AGENT tail --max-bytes 16000
$AGENT command "svc stats" --timeout 10
$AGENT wait \
  --pattern "All tests PASSED" --timeout 120
$AGENT verify --timeout 180
$AGENT flash --timeout 240
$AGENT reset --timeout 30
```

其他程序需要结构化输出时加 `--json`。响应同时包含 UTF-8 文本和无损
Base64 原始数据。

## 4. 接入任意 MCP 客户端

MCP 服务使用标准 stdio JSON-RPC，不依赖 Codex。启动命令统一为：

```bash
python3 /home/five/my-rtos/tools/embedded-agent/embedded_mcp_server.py \
  --config /home/five/my-rtos/tools/embedded-agent/my-rtos.json
```

### Codex

首次使用时注册 MCP：

```bash
codex mcp add my-rtos-board -- \
  python3 /home/five/my-rtos/tools/embedded-agent/embedded_mcp_server.py \
  --config /home/five/my-rtos/tools/embedded-agent/my-rtos.json
```

### Claude Desktop / Cursor 等客户端

在客户端的 MCP 配置中添加等价的 stdio server：

```json
{
  "mcpServers": {
    "embedded-board": {
      "command": "python3",
      "args": [
        "/home/five/my-rtos/tools/embedded-agent/embedded_mcp_server.py",
        "--config",
        "/home/five/my-rtos/tools/embedded-agent/my-rtos.json"
      ]
    }
  }
}
```

不同客户端的配置文件位置不同，但 `command` 和 `args` 完全相同。注册后可使用：

- `board_status`
- `serial_tail`
- `serial_command`
- `serial_wait`
- `board_verify`
- `board_flash`
- `board_reset`

MCP 进程不直接打开硬件，只连接已运行的代理，因此重启 AI 会话不会丢失
串口日志。

删除 MCP 配置：

```bash
codex mcp remove my-rtos-board
```

## 5. 直接 JSON API

Unix socket 上每行一个 JSON 请求和响应：

```json
{"op":"command","command":"svc stats","timeout":10}
```

支持 `status`、`tail`、`write`、`command`、`wait`、`verify`、`flash` 和
`reset`。板卡操作使用固定白名单，不能借此执行任意主机命令。

## 6. 自动测试

```bash
make agent-test
```

测试使用伪终端，不连接开发板也能运行。

## 7. 迁移到其他项目

只需要复制整个 `tools/embedded-agent/` 目录，然后：

1. 从 `config.example.json` 创建项目配置。
2. 修改串口、prompt、工作目录和 action 命令。
3. 启动 daemon。
4. 在所用 AI 客户端中注册 MCP stdio 命令。

action 进程会自动收到 `PORT`、`BAUD`、`EMBEDDED_AGENT_PORT` 和
`EMBEDDED_AGENT_BAUD` 环境变量。核心代码不依赖 My-RTOS、Pico SDK、
OpenOCD 或特定 AI 产品；这些都只是配置。

## 8. 常见问题

- `serial port is not connected`：检查 `/dev/ttyACM0` 和用户串口权限。
- 输出乱码：确认固件与代理都使用 `921600 8N1`。
- `embedded agent is already running`：执行 `make agent-stop` 后重试。
- MCP 工具连接失败：先执行 `make agent-status`，确认 daemon 已启动。
- 烧录失败：直接运行 `make flash` 检查 OpenOCD/DAPLink 输出。
