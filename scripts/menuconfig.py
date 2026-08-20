#!/usr/bin/env python3
"""
My-RTOS 配置工具

提供 menuconfig 交互式配置界面。
"""

import os
import sys
import re
import curses
from pathlib import Path
from typing import Dict, List, Optional, Tuple


class ConfigOption:
    """配置选项"""
    def __init__(self, name: str, opt_type: str, default: str = "",
                 prompt: str = "", help_text: str = "",
                 depends: List[str] = None, select: List[str] = None,
                 range_str: str = ""):
        self.name = name
        self.type = opt_type
        self.default = default
        self.prompt = prompt
        self.help_text = help_text
        self.depends = depends or []
        self.select = select or []
        self.range_str = range_str
        self.value = None
        self.visible = True


class ConfigMenu:
    """配置菜单"""
    def __init__(self, title: str):
        self.title = title
        self.items: List[Tuple[str, object]] = []  # (type, option/menu)
        self.parent: Optional['ConfigMenu'] = None


class KconfigParser:
    """Kconfig 解析器"""

    def __init__(self, filename: str):
        self.filename = filename
        self.root_menu = ConfigMenu("Main Menu")
        self.current_menu = self.root_menu
        self.options: Dict[str, ConfigOption] = {}

    def parse(self):
        """解析 Kconfig 文件"""
        with open(self.filename, 'r', encoding='utf-8') as f:
            lines = f.readlines()

        i = 0
        while i < len(lines):
            line = lines[i].strip()

            # 跳过空行和注释
            if not line or line.startswith('#'):
                i += 1
                continue

            # 解析菜单
            if line.startswith('menu '):
                title = line[5:].strip('"')
                new_menu = ConfigMenu(title)
                new_menu.parent = self.current_menu
                self.current_menu.items.append(('menu', new_menu))
                self.current_menu = new_menu

            elif line == 'endmenu':
                self.current_menu = self.current_menu.parent or self.root_menu

            # 解析配置项
            elif line.startswith('config '):
                name = line[7:].strip()
                # _parse_option returns i pointing AT the next keyword line
                # (next config/menu/endmenu/choice/endchoice). Decrement by 1
                # so the trailing `i += 1` lands on that keyword — otherwise
                # we'd silently skip the next config after a multi-line help
                # block.
                option, next_i = self._parse_option(name, lines, i)
                self.options[name] = option
                self.current_menu.items.append(('option', option))
                i = next_i - 1

            # 解析 choice
            elif line.startswith('choice'):
                i = self._parse_choice(lines, i)

            # mainmenu
            elif line.startswith('mainmenu '):
                self.root_menu.title = line[9:].strip('"')

            i += 1

    def _parse_option(self, name: str, lines: List[str], start: int) -> Tuple[ConfigOption, int]:
        """解析单个配置选项"""
        opt_type = ""
        prompt = ""
        default = ""
        help_text = ""
        depends = []
        range_str = ""

        i = start + 1
        while i < len(lines):
            line = lines[i].strip()

            if not line:
                i += 1
                continue

            if line.startswith(('config ', 'menu ', 'endmenu', 'choice', 'endchoice')):
                break

            if line.startswith('bool'):
                opt_type = 'bool'
                prompt = line[4:].strip().strip('"')
            elif line.startswith('int'):
                opt_type = 'int'
                match = re.match(r'int\s+"([^"]*)"', line)
                if match:
                    prompt = match.group(1)
            elif line.startswith('string'):
                opt_type = 'string'
                match = re.match(r'string\s+"([^"]*)"', line)
                if match:
                    prompt = match.group(1)
            elif line.startswith('default'):
                default = line[7:].strip().split()[0]
            elif line.startswith('range'):
                range_str = line[5:].strip()
            elif line.startswith('depends on'):
                depends.append(line[10:].strip())
            elif line.startswith('help'):
                # 收集帮助文本
                # Consume indented lines until we hit a non-indented non-empty
                # line (the next keyword). Leave i pointing AT that line so the
                # outer loop recognizes it; we `continue` to skip the trailing
                # `i += 1` which would otherwise skip the next config keyword.
                i += 1
                help_lines = []
                while i < len(lines):
                    hline = lines[i].rstrip()
                    if hline and not hline[0].isspace() and hline.strip():
                        break
                    if hline.strip():
                        help_lines.append(hline.strip())
                    i += 1
                help_text = '\n'.join(help_lines)
                continue

            i += 1

        return ConfigOption(name, opt_type, default, prompt, help_text, depends, range_str=range_str), i

    def _parse_choice(self, lines: List[str], start: int) -> int:
        """解析 choice 块"""
        i = start + 1
        choice_prompt = ""
        choice_default = ""
        choice_options = []

        while i < len(lines):
            line = lines[i].strip()

            if line == 'endchoice':
                break

            if line.startswith('prompt'):
                choice_prompt = line[7:].strip().strip('"')
            elif line.startswith('default'):
                choice_default = line[7:].strip()
            elif line.startswith('config '):
                name = line[7:].strip()
                # Same off-by-one fix as in parse(): _parse_option returns i
                # pointing at the terminating keyword; subtract 1 so the
                # outer `i += 1` lands on it.
                option, next_i = self._parse_option(name, lines, i)
                self.options[name] = option
                choice_options.append(option)
                self.current_menu.items.append(('option', option))
                i = next_i - 1

            i += 1

        return i


class ConfigFile:
    """配置文件处理"""

    def __init__(self, filename: str):
        self.filename = filename
        self.config: Dict[str, str] = {}

    def load(self):
        """加载配置文件"""
        if not os.path.exists(self.filename):
            return

        with open(self.filename, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                if '=' in line:
                    key, value = line.split('=', 1)
                    self.config[key.strip()] = value.strip()

    def save(self):
        """保存配置文件"""
        with open(self.filename, 'w') as f:
            f.write("#\n")
            f.write("# My-RTOS Configuration\n")
            f.write("# Generated by menuconfig\n")
            f.write("#\n\n")

            for key, value in sorted(self.config.items()):
                f.write(f"{key}={value}\n")

    def set(self, key: str, value: str):
        """设置配置值"""
        self.config[key] = value

    def get(self, key: str, default: str = "") -> str:
        """获取配置值"""
        return self.config.get(key, default)

    def is_enabled(self, key: str) -> bool:
        """检查布尔选项是否启用"""
        return self.config.get(key, 'n') == 'y'


def single_dep_ok(dep: str, config: ConfigFile) -> bool:
    """单个依赖表达式判定:'SYMBOL'、'!SYMBOL'、'SYMBOL = val'、'!= '"""
    if ' ' in dep:
        parts = dep.split()
        var, op, val = parts[0], parts[1], parts[2]
        current = config.get(var, '')
        if op == '=':
            return current == val
        return current != val
    if dep.startswith('!'):
        return not config.is_enabled(dep[1:])
    return config.is_enabled(dep)


def deps_satisfied(depends: List[str], config: ConfigFile) -> bool:
    """依赖列表判定(&& 连接;Kconfig 语义中列表各项均需满足)"""
    for dep in depends:
        if '&&' in dep:
            if not all(single_dep_ok(p.strip(), config) for p in dep.split('&&')):
                return False
        elif not single_dep_ok(dep.strip(), config):
            return False
    return True


def collapse_depends(parser: 'KconfigParser', config: ConfigFile) -> int:
    """P0-8: genconfig 输出前收缩违依赖符号。

    defconfig 携带依赖不满足的符号(如 TEST_ENABLE 关闭却写 TEST_MODULE_x=y)
    时不再原样写进 kernel_config.h——此前会把测试代码链进 TEST-off 镜像。
    迭代到不动点以覆盖依赖链(A 丢弃导致 B 的依赖也不满足)。返回丢弃数。"""
    dropped = 0
    changed = True
    while changed:
        changed = False
        for name, option in parser.options.items():
            if config.config.get(name, 'n') == 'n':
                continue
            if not deps_satisfied(option.depends, config):
                print(f"genconfig: drop {name} (unsatisfied depends: "
                      f"{' && '.join(option.depends) or '-'})", file=sys.stderr)
                del config.config[name]
                dropped += 1
                changed = True
    return dropped


def clamp_ranges(parser: 'KconfigParser', config: ConfigFile) -> int:
    """P0-8: int 符号越 range 时钳到边界(带告警)。

    Kconfig range 此前仅在交互 UI 提示,genconfig 原样输出 .config 的越界值
    (tiny preset 的 IPC_EP_MSG_SIZE=32 装不下 ns_name_msg_t 即此类问题)。"""
    clamped = 0
    for name, option in parser.options.items():
        if option.type != 'int' or not option.range_str:
            continue
        value = config.config.get(name, '')
        if not value.isdigit():
            continue
        bounds = option.range_str.split()
        if len(bounds) != 2 or not all(b.lstrip('-').isdigit() for b in bounds):
            continue
        lo, hi = int(bounds[0]), int(bounds[1])
        iv = int(value)
        if iv < lo or iv > hi:
            clamped_v = max(lo, min(hi, iv))
            print(f"genconfig: clamp {name}={value} -> {clamped_v} "
                  f"(range {lo}..{hi})", file=sys.stderr)
            config.config[name] = str(clamped_v)
            clamped += 1
    return clamped


class MenuConfigUI:
    """menuconfig 用户界面"""
    def __init__(self, parser: KconfigParser, config: ConfigFile):
        self.parser = parser
        self.config = config
        self.current_menu = parser.root_menu
        self.selected = 0
        self.scroll_offset = 0
        self.help_mode = False
        self.input_mode = False
        self.input_value = ""
        self.input_option = None

    def run(self):
        """运行 menuconfig"""
        curses.wrapper(self._main_loop)

    def _main_loop(self, stdscr):
        """主循环"""
        curses.curs_set(0)
        stdscr.keypad(True)

        while True:
            self._draw(stdscr)

            key = stdscr.getch()

            if self.input_mode:
                self._handle_input(key, stdscr)
            elif self.help_mode:
                if key in (ord('q'), ord('Q'), 27, curses.KEY_ENTER, ord('\n')):
                    self.help_mode = False
            else:
                result = self._handle_key(key)
                if result == 'quit':
                    return
                elif result == 'save_quit':
                    self.config.save()
                    return

    def _draw(self, stdscr):
        """绘制界面"""
        stdscr.clear()
        height, width = stdscr.getmaxyx()

        # 标题栏
        title = f" My-RTOS Configuration - {self.current_menu.title} "
        stdscr.addstr(0, 0, title.center(width), curses.A_REVERSE)

        # 获取可见项
        visible_items = self._get_visible_items()

        # 计算滚动
        if self.selected < self.scroll_offset:
            self.scroll_offset = self.selected
        elif self.selected >= self.scroll_offset + height - 4:
            self.scroll_offset = self.selected - height + 5

        # 绘制菜单项
        y = 2
        for i, (item_type, item) in enumerate(visible_items):
            if i < self.scroll_offset:
                continue
            if y >= height - 2:
                break

            # 检查是否选中
            is_selected = (i == self.selected)
            attr = curses.A_REVERSE if is_selected else 0

            if item_type == 'menu':
                # 子菜单
                text = f"  ---> {item.title}"
                stdscr.addstr(y, 0, text[:width-1], attr)
            else:
                # 配置选项
                option: ConfigOption = item
                current_value = self.config.get(option.name, option.default)

                if option.type == 'bool':
                    marker = "[*]" if current_value == 'y' else "[ ]"
                    text = f"  {marker} {option.prompt}"
                elif option.type == 'int':
                    text = f"  ({current_value}) {option.prompt}"
                elif option.type == 'string':
                    text = f"  ({current_value}) {option.prompt}"
                else:
                    text = f"  {option.prompt}"

                stdscr.addstr(y, 0, text[:width-1], attr)

            y += 1

        # 底部帮助栏
        help_bar = " ↑/↓: Move | Enter: Select | Space: Toggle | h: Help | s: Save | q: Quit "
        stdscr.addstr(height - 1, 0, help_bar[:width], curses.A_REVERSE)

        stdscr.refresh()

    def _get_visible_items(self) -> List[Tuple[str, object]]:
        """获取可见的菜单项"""
        items = []
        for item_type, item in self.current_menu.items:
            if item_type == 'option':
                option: ConfigOption = item
                # 检查依赖
                if self._check_depends(option):
                    items.append((item_type, item))
            else:
                items.append((item_type, item))
        return items

    def _check_depends(self, option: ConfigOption) -> bool:
        """检查选项依赖是否满足(与 genconfig 收缩共用 single_dep_ok)"""
        return deps_satisfied(option.depends, self.config)

    def _handle_key(self, key) -> Optional[str]:
        """处理按键"""
        visible_items = self._get_visible_items()

        if key == curses.KEY_UP:
            if self.selected > 0:
                self.selected -= 1

        elif key == curses.KEY_DOWN:
            if self.selected < len(visible_items) - 1:
                self.selected += 1

        elif key == curses.KEY_ENTER or key == ord('\n'):
            if visible_items:
                item_type, item = visible_items[self.selected]
                if item_type == 'menu':
                    self.current_menu = item
                    self.selected = 0
                    self.scroll_offset = 0
                else:
                    option: ConfigOption = item
                    if option.type == 'int' or option.type == 'string':
                        self.input_mode = True
                        self.input_value = self.config.get(option.name, option.default)
                        self.input_option = option

        elif key == ord(' '):
            if visible_items:
                item_type, item = visible_items[self.selected]
                if item_type == 'option':
                    option: ConfigOption = item
                    if option.type == 'bool':
                        current = self.config.get(option.name, option.default)
                        new_val = 'n' if current == 'y' else 'y'
                        self.config.set(option.name, new_val)

        elif key in (ord('h'), ord('H')):
            self.help_mode = True

        elif key in (ord('s'), ord('S')):
            self.config.save()

        elif key in (ord('q'), ord('Q')):
            return 'quit'

        elif key == 27:  # ESC
            if self.current_menu.parent:
                self.current_menu = self.current_menu.parent
                self.selected = 0

        return None

    def _handle_input(self, key, stdscr):
        """处理输入模式"""
        if key == curses.KEY_ENTER or key == ord('\n'):
            # 确认输入
            if self.input_option:
                self.config.set(self.input_option.name, self.input_value)
            self.input_mode = False
            self.input_option = None
        elif key == 27:  # ESC
            self.input_mode = False
            self.input_option = None
        elif key == curses.KEY_BACKSPACE or key == 127:
            self.input_value = self.input_value[:-1]
        elif key >= 32 and key < 127:
            self.input_value += chr(key)


def generate_config_header(config: ConfigFile, output: str):
    """生成配置头文件"""
    with open(output, 'w') as f:
        f.write("/**\n")
        f.write(" * @file kernel_config.h\n")
        f.write(" * @brief 内核配置（自动生成）\n")
        f.write(" *\n")
        f.write(" * 由 menuconfig 生成，请勿手动修改。\n")
        f.write(" */\n\n")
        f.write("#ifndef KERNEL_CONFIG_H\n")
        f.write("#define KERNEL_CONFIG_H\n\n")

        f.write("/* 自动生成的配置 */\n")
        for key, value in sorted(config.config.items()):
            # 转换为宏名称
            macro = key.upper().replace('-', '_')

            if value == 'y':
                f.write(f"#define {macro} 1\n")
            elif value == 'n':
                f.write(f"// #define {macro} 0\n")
            elif value.isdigit():
                f.write(f"#define {macro} ({value})\n")
            else:
                # 字符串值，去掉多余的引号
                str_val = value.strip('"')
                f.write(f'#define {macro} "{str_val}"\n')

        # 添加兼容性别名
        f.write("\n/*============================================================================\n")
        f.write(" * 兼容性别名（旧宏名 -> 新宏名）\n")
        f.write(" *============================================================================*/\n\n")

        f.write("/* 内核配置 */\n")
        f.write("#define KERN_MAX_TASKS            KERNEL_MAX_TASKS\n")
        f.write("#define KERN_MAX_PRIORITY         KERNEL_MAX_PRIORITIES\n")
        f.write("#define KERN_TICK_RATE_HZ         KERNEL_TICK_RATE\n")
        f.write("#define KERN_TICK_US              (1000000UL / KERNEL_TICK_RATE)\n")
        f.write("#define KERN_DEFAULT_STACK_SIZE   KERNEL_TASK_STACK_SIZE\n")
        f.write("#define KERN_IDLE_STACK_SIZE      KERNEL_IDLE_STACK_SIZE\n")
        f.write("#define KERN_IDLE_PRIORITY        KERNEL_IDLE_PRIORITY\n")
        f.write("#define KERN_HEAP_SIZE            MEM_HEAP_SIZE\n\n")

        f.write("/* IPC 配置 */\n")
        f.write("#define KERN_MAX_SEMAPHORES       IPC_SEMAPHORE_MAX\n")
        f.write("#define KERN_MAX_MUTEXES          IPC_MUTEX_MAX\n")
        f.write("#define KERN_MAX_MQUEUES          IPC_MQUEUE_MAX\n")
        f.write("#define KERN_MAX_EVENTS           IPC_EVENT_MAX\n")
        f.write("#define KERN_MAX_ENDPOINTS        IPC_ENDPOINT_MAX\n")
        f.write("#define KERN_MAX_CHANNELS         IPC_CHANNEL_MAX\n")
        f.write("#define KERN_EP_MSG_SIZE          IPC_EP_MSG_SIZE\n")
        f.write("#define KERN_EP_MAX_PENDING       IPC_EP_MAX_PENDING\n")
        f.write("#define KERN_CH_MSG_SIZE          IPC_CH_MSG_SIZE\n")
        f.write("#define KERN_MUTEX_PI             1  /* 优先级继承 */\n\n")

        f.write("/* 消息队列配置 */\n")
        f.write("#define KERN_MQUEUE_DEPTH         16\n")
        f.write("#define KERN_MSG_MAX_SIZE         64\n\n")

        f.write("/* 栈配置 */\n")
        f.write("#define KERN_STACK_ALIGN          8\n")
        f.write("#define STACK_MAGIC_BYTE          0xAB\n\n")

        f.write("/* 调试配置 */\n")
        f.write("#define KERN_DEBUG_ENABLE         DEBUG_ENABLE\n\n")

        f.write("/* 调度器配置 */\n")
        f.write("#define SCHED_CRITICAL_PRIORITY   2\n")
        f.write("#define PENDSV_PRIORITY           15\n")
        f.write("#define SYSTICK_PRIORITY          15\n")

        f.write("\n#endif /* KERNEL_CONFIG_H */\n")


def main():
    """主函数"""
    # 路径设置
    kconfig_file = "Kconfig"
    config_file = ".config"
    header_file = "include/kernel/kernel_config.h"

    # 解析 Kconfig
    parser = KconfigParser(kconfig_file)
    parser.parse()

    # 加载配置
    config = ConfigFile(config_file)
    config.load()

    # 如果没有配置文件，使用默认值
    if not config.config:
        for name, option in parser.options.items():
            if option.default:
                config.set(name, option.default)

    # 检查命令行参数
    if len(sys.argv) > 1:
        if sys.argv[1] == 'genconfig':
            # P0-8: 输出前收缩违依赖符号 + int range 钳制
            collapse_depends(parser, config)
            clamp_ranges(parser, config)
            # 仅生成配置头文件
            generate_config_header(config, header_file)
            print(f"Generated: {header_file}")
            return
        elif sys.argv[1] == 'defconfig':
            # 保存默认配置
            config.save()
            print(f"Saved default config: {config_file}")
            return

    # 运行 menuconfig
    ui = MenuConfigUI(parser, config)
    ui.run()

    # 保存配置
    config.save()
    print(f"\nConfiguration saved to: {config_file}")

    # 生成头文件
    generate_config_header(config, header_file)
    print(f"Generated: {header_file}")


if __name__ == "__main__":
    main()
