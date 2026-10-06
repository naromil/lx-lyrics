[English](README.md) | [简体中文](README.zh-CN.md)

# lx-lyrics

独立的桌面歌词显示程序，移植自 [lx-music-desktop](https://github.com/lyswhut/lx-music-desktop) 的桌面歌词窗口：一个自包含的歌词应用，外加若干在进程内驱动它的播放器适配器。

## 状态

七个项目——显示应用加六个适配器——**不共享任何 C++ 源码**；唯一的契约是 `docs/protocol.md` **v2**：播放器侧适配器与显示应用之间基于 stdin/stdout 的 player feed。

- **`lyrics-app/`**——独立的 Qt6 / C++23 桌面歌词窗口，支持同步滚动与当前行高亮渲染。负责歌词获取（同名 `.lrc` 文件 + 内嵌标签）、解析、选择与渲染。完整测试套件通过（6 个套件、171 个 QTest 用例）。支持宿主驱动（`--player-feed`）、自喂数据（`--demo`），或作为惰性窗口运行。
- **`plugins/fooyin/`**——Fooyin 适配器（>= 0.11.1）：把 `lx-lyrics-app --player-feed` 作为直接子进程启动，并通过 feed 推送播放上下文（路径、元数据、状态、位置、频谱）。插件元数据及 `Plugin`/`CorePlugin`/`GuiPlugin` 接口已针对已安装的 Fooyin 0.12.6 完成加载验证。
- **`plugins/deadbeef/`**——DeaDBeeF C 适配器（`ddb_lxlyrics.so`），API 下限 1.16（DeaDBeeF >= 1.9.3）；当播放器暴露其分析器时响应频谱请求。
- **`plugins/rhythmbox/`**——Rhythmbox libpeas Python 适配器；Rhythmbox 没有分析器 API，因此声明 `spectrum: false`。
- **`plugins/audacious/`**——Audacious 通用插件（`lxlyrics.so`，≥ 4.6.1；纯 POSIX C 传输层 + C++17 胶水层）：启动子进程并推送播放上下文，并通过 Audacious 公开的 `Visualizer` API 响应分析器请求（`spectrum: true`）。
- **`plugins/quodlibet/`**——Quod Libet 事件插件（单个 Python 模块 `lxlyrics.py`，≥ 4.4.0）：在 GLib 主循环上启动子进程，推送播放上下文以及 `set_fullscreen`；Quod Libet 不暴露分析器，因此声明 `spectrum: false`。
- **`plugins/vlc/`**——VLC 3.0.x 接口模块（`liblxlyrics_plugin.so`，通过 `--extraintf=lxlyrics` 加载）：基于轮询的采样器为子进程供数；从插件中访问不到分析器，因此 `spectrum: false`；不支持 VLC 4。

## 播放器支持

适配器覆盖 Fooyin、DeaDBeeF、Rhythmbox、Audacious、Quod Libet 与 VLC。**Strawberry、Clementine、Elisa 与 Tauon 已放弃**——它们没有可承载进程内适配器的扩展机制，且按既定决策不提供 MPRIS 兜底，也不做分支（fork）。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `lyrics-app/` | 独立的 Qt6 歌词显示程序——见 `lyrics-app/README.md` |
| `plugins/fooyin/` | Fooyin 适配器——见 `plugins/fooyin/README.md` |
| `plugins/deadbeef/` | DeaDBeeF 适配器——见 `plugins/deadbeef/README.md` |
| `plugins/rhythmbox/` | Rhythmbox 适配器——见 `plugins/rhythmbox/README.md` |
| `plugins/audacious/` | Audacious 适配器——见 `plugins/audacious/README.md` |
| `plugins/quodlibet/` | Quod Libet 适配器——见 `plugins/quodlibet/README.md` |
| `plugins/vlc/` | VLC 适配器——见 `plugins/vlc/README.md` |
| `docs/` | 架构、协议与研究笔记 |
| `references/` | lx-music-desktop v2.12.2 源码（已 gitignore；只读参考） |
| `tools/` | 构建/安装辅助脚本——`tools/install.sh` 构建并安装应用以及六个适配器中的任意若干（`--player NAME`、`--player all`）；`tools/lint.sh` 运行 clang-format + clang-tidy 门禁 |

## 文档

- `lyrics-app/README.md`——构建、运行模式、配置、测试
- `plugins/fooyin/README.md`——构建、安装、使用、故障排查
- `plugins/deadbeef/README.md`、`plugins/rhythmbox/README.md`、`plugins/audacious/README.md`、`plugins/quodlibet/README.md`、`plugins/vlc/README.md`——各适配器的构建/安装/测试说明
- `docs/architecture.md`——组件设计、所有权不变式与解耦边界
- `docs/protocol.md`——v2 player feed（stdin/stdout JSON 行；共享契约）
- `docs/research/`——为本次移植整理的工程研究笔记

## 快速开始

```sh
./tools/install.sh                        # 应用 + 本机已安装其播放器的所有适配器
./tools/install.sh --player all           # 应用 + 全部六个适配器，无论是否检测到
./tools/install.sh --player vlc --player audacious
```

不带 `--player` 参数时，安装脚本会安装所有能在这台机器上找到对应播放器的适配器；指定名称可显式选择适配器，或用 `--player all` 安装全部六个。每次运行都会以 Release 模式构建应用及所选适配器，把每个适配器安装到各自播放器的插件目录，并写入该适配器的配置，使其能找到已安装的 `lx-lyrics-app`（Fooyin `AppPath`、DeaDBeeF `lxlyrics.app_path`、Rhythmbox `app-path`、Audacious `[lx-lyrics] app_path`、Quod Libet `lxlyrics_app_path`、VLC `lxlyrics-app-path`）。`--prefix DIR` 只把应用移到 `DIR/bin`，不移动其他任何东西——插件只随其自身的 `--fooyin-plugin-dir`/`--audacious-plugin-dir`/`--vlc-plugin-dir` 移动；`--no-autospawn` 会写入各播放器“不要自行启动歌词会话”的状态。`./tools/install.sh --help` 列出每个适配器的头文件/目标目录覆盖项。安装到 root 所有目录（发行版打包的 Audacious、系统级 VLC 插件目录）的适配器会打印确切的 `sudo install` 命令并以非零码退出。

手动安装（等价的手工步骤）：

```sh
cd lyrics-app && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build        # 1. 构建显示应用
cd ../plugins/fooyin && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build # 2. 构建 Fooyin 适配器
cp build/fyplugin_lxlyrics.so ~/.local/lib/fooyin/plugins/                                       # 3. 安装，重启 Fooyin
# 4. 让插件指向应用二进制（设置 -> 歌词 -> LX Lyrics -> AppPath），或把
#    lyrics-app/build/lx-lyrics-app 放进 PATH，让自动检测找到它
```

各适配器的前置条件一览（这些正是直接跑 `--player all` 时缺失会报告的内容）：

- **Fooyin**——需以 `INSTALL_HEADERS=ON` 构建的 Fooyin；产物 `build/fyplugin_lxlyrics.so` → `<prefix>/lib/fooyin/plugins`（或 `~/.local/lib/fooyin/plugins`）。
- **DeaDBeeF**——需要其头文件（`deadbeef/deadbeef.h`：已安装的 SDK，例如发行版包提供的 `/usr/include/deadbeef/`，或一份源码检出；当 CMake 的 `find_path` 两者都找不到时用 `--deadbeef-include DIR` 覆盖），API 下限 1.16（DeaDBeeF >= 1.9.3）；`ddb_lxlyrics.so` → `~/.local/lib/deadbeef/`，然后重启播放器。
- **Rhythmbox**——无需构建：`lxlyrics.py` + `lxlyrics.plugin` → `~/.local/share/rhythmbox/plugins/lxlyrics/`；需要支持 Python 插件的 Rhythmbox（libpeas python3 loader），否则插件无法加载。
- **Audacious**——需要 Audacious ≥ 4.6.1 的头文件（`pkg-config audacious`，或 `--audacious-include DIR`）；`lxlyrics.so` → `<prefix>/lib/audacious/General/`（没有每用户插件目录，因此这个需要 root），然后重启 Audacious。
- **Quod Libet**——无需构建：`lxlyrics.py`（是模块，不是目录）→ `~/.config/quodlibet/plugins/`，重启后在“文件 → 插件”中启用它（插件项就在那里；Quod Libet 没有“音乐”菜单）。
- **VLC**——需要 VLC 3.0.x 的模块头文件（`pkg-config vlc-plugin`，或 `--vlc-include DIR`）；`liblxlyrics_plugin.so` → `VLC_PLUGIN_PATH` 上的某个目录（默认 `~/.local/lib/vlc/plugins`，系统扫描目录可写时也可用它），然后带 `--extraintf=lxlyrics` 重启 VLC。

各适配器的 README 中有完整的构建、安装与测试命令。

## 许可证

`lyrics-app/` 采用 Apache-2.0——其歌词解析/渲染逻辑移植自 lx-music-desktop（Apache-2.0）并保留署名。`plugins/` 下的六个适配器（`fooyin/`、`deadbeef/`、`rhythmbox/`、`audacious/`、`quodlibet/`、`vlc/`）按仓库决策采用 GPL-3.0-only，各宿主均允许：Fooyin 是 GPL-3.0；Rhythmbox、Quod Libet 与 VLC 是 GPL-2.0-or-later（其 “or later” 条款允许 GPL-3.0 插件）；DeaDBeeF 的插件 API 头文件为 zlib 许可；Audacious 的 libaudcore 为 BSD-2-Clause。本仓库的条款见 `LICENSE`。
