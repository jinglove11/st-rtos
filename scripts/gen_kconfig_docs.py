#!/usr/bin/env python3
"""
Generate per-CONFIG_* documentation from Kconfig.

Reads the main Kconfig, emits one Markdown file per config option to
docs/kconfig/<CONFIG_NAME>.md, and a docs/kconfig/INDEX.md table of contents.

Why a generator instead of hand-written docs:
  - 123 CONFIG_* entries and growing; manual docs would drift immediately.
  - Phase 2-9 roadmap will add ~30 more entries; CI reruns this script.
  - Keeps the help text + dependencies + defaults in sync with Kconfig.

Usage:
    python3 scripts/gen_kconfig_docs.py          # emit all docs
    python3 scripts/gen_kconfig_docs.py --check  # exit non-zero if docs stale
"""

import os
import re
import sys
from pathlib import Path

# Reuse the parser from menuconfig.py
sys.path.insert(0, str(Path(__file__).parent))
from menuconfig import KconfigParser  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
KCONFIG = ROOT / "Kconfig"
OUT_DIR = ROOT / "docs" / "kconfig"


def slugify(name: str) -> str:
    """CONFIG_BOARD_RP2350 -> CONFIG_BOARD_RP2350 (filename = same as name)."""
    return name


def render_option_md(opt, menu_path: list) -> str:
    """Render one CONFIG_* as a Markdown page."""
    lines = []
    lines.append(f"# `{opt.name}`")
    lines.append("")

    if opt.prompt:
        lines.append(f"**{opt.prompt}**")
        lines.append("")

    # Type + default + range
    lines.append("| Property | Value |")
    lines.append("|---|---|")
    lines.append(f"| Type | `{opt.type}` |")
    if opt.default:
        lines.append(f"| Default | `{opt.default}` |")
    if opt.range_str:
        lines.append(f"| Range | `{opt.range_str}` |")
    if menu_path:
        lines.append(f"| Menu path | `{' > '.join(menu_path)}` |")
    lines.append("")

    # Dependencies
    if opt.depends:
        lines.append("## Dependencies")
        lines.append("")
        for d in opt.depends:
            # Pull out CONFIG_FOO tokens and link them
            tokens = re.findall(r"[A-Z_][A-Z0-9_]+", d)
            linked = d
            for tok in tokens:
                if tok == opt.name:
                    continue
                linked = linked.replace(tok, f"[{tok}]({tok}.md)")
            lines.append(f"- `{linked}`")
        lines.append("")
    else:
        lines.append("_No dependencies._")
        lines.append("")

    # Help
    if opt.help_text:
        lines.append("## Help")
        lines.append("")
        lines.append("```")
        lines.append(opt.help_text)
        lines.append("```")
        lines.append("")
    else:
        lines.append("_No help text yet — consider documenting the rationale, "
                     "size cost, and phase in [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)._")
        lines.append("")

    # Cross-links
    lines.append("---")
    lines.append("")
    lines.append("- Back to [INDEX](INDEX.md)")
    lines.append("- Top-level [Kconfig](../../Kconfig)")
    lines.append("- Project roadmap: [MICROKERNEL_OS_ROADMAP.md](../../MICROKERNEL_OS_ROADMAP.md)")
    lines.append("")

    return "\n".join(lines)


def walk_menu(menu, path=None):
    """Yield (option, menu_path) for every option in the tree."""
    if path is None:
        path = []
    for kind, item in menu.items:
        if kind == "menu":
            yield from walk_menu(item, path + [item.title])
        elif kind == "option":
            yield item, path


def main():
    check_only = "--check" in sys.argv

    parser = KconfigParser(str(KCONFIG))
    parser.parse()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    # Pass 1: render per-option docs
    written = []
    for opt, menu_path in walk_menu(parser.root_menu):
        if not opt.name:
            continue
        md = render_option_md(opt, menu_path)
        out_path = OUT_DIR / f"{slugify(opt.name)}.md"
        if check_only:
            existing = out_path.read_text() if out_path.exists() else ""
            if existing != md:
                print(f"STALE: {out_path.name} (rerun gen_kconfig_docs.py)", file=sys.stderr)
                return 1
        else:
            out_path.write_text(md)
        written.append((opt, menu_path))

    # Pass 2: INDEX.md
    grouped = {}
    for opt, menu_path in written:
        group = menu_path[0] if menu_path else "(top-level)"
        grouped.setdefault(group, []).append(opt)

    idx = []
    idx.append("# Kconfig Index")
    idx.append("")
    idx.append(f"Auto-generated from [`Kconfig`](../../Kconfig) by "
               f"[`scripts/gen_kconfig_docs.py`](../../scripts/gen_kconfig_docs.py).")
    idx.append("")
    idx.append(f"**{len(written)} options** across **{len(grouped)}** menu groups.")
    idx.append("")
    idx.append("To refresh this page after editing Kconfig:")
    idx.append("")
    idx.append("```bash")
    idx.append("python3 scripts/gen_kconfig_docs.py")
    idx.append("```")
    idx.append("")

    for group in sorted(grouped):
        opts = sorted(grouped[group], key=lambda o: o.name)
        idx.append(f"## {group}")
        idx.append("")
        idx.append("| Option | Type | Default | Description |")
        idx.append("|---|---|---|---|")
        for opt in opts:
            prompt = (opt.prompt or "").replace("|", "\\|")
            default = opt.default or "—"
            idx.append(f"| [`{opt.name}`]({opt.name}.md) | `{opt.type}` | `{default}` | {prompt} |")
        idx.append("")

    idx.append("## Menu presets")
    idx.append("")
    idx.append("| Preset | File | Use case |")
    idx.append("|---|---|---|")
    idx.append("| Tiny | [`configs/tiny_defconfig`](../../configs/tiny_defconfig) | <32KB flash, single-purpose |")
    idx.append("| Default | [`configs/default_defconfig`](../../configs/default_defconfig) | Dev/test baseline (2867/2867 PASS) |")
    idx.append("| Release | [`configs/release_defconfig`](../../configs/release_defconfig) | Production (no tests/shell) |")
    idx.append("| Full | [`configs/full_defconfig`](../../configs/full_defconfig) | Every subsystem on (compile-test) |")
    idx.append("")

    idx_path = OUT_DIR / "INDEX.md"
    if check_only:
        existing = idx_path.read_text() if idx_path.exists() else ""
        if existing != "\n".join(idx):
            print(f"STALE: INDEX.md", file=sys.stderr)
            return 1
    else:
        idx_path.write_text("\n".join(idx))

    if not check_only:
        print(f"Generated {len(written)} option docs + INDEX.md under {OUT_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
