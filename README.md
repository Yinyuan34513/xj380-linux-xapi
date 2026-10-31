# XJ380 XAPI — Linux 内核补丁（开源版）

把 XJ380 系统的 XAPI 兼容层（非 GUI 部分）直接内置到 Linux 内核的补丁。
基于 **linux-7.1.8** 内核源码。

## 背景

XJ380 XACT 平台二进制通过自定义 syscall（号段 7380–7470，`enter_syscall`）
调用 XAPI。此补丁让 Linux 内核原生识别并处理其中的**非 GUI** API（I/O、
文件系统、进程、时间、内存、用户、电源等），替代原先的独立用户态
兼容层；图形号段（7392–7463）现也由内核直接处理（见下文"图形 XAPI"）。

## 修改内容

| 文件 | 变更 |
|------|------|
| `arch/x86/kernel/xapi.c` | **新增** — XAPI 非 GUI syscall 处理（7380–7470） |
| `arch/x86/kernel/xapi_gui.c` | **新增** — XAPI **图形** subset（7392–7463，fbdev 窗口/绘图/消息） |
| `arch/x86/kernel/Makefile` | `obj-y += xapi.o xapi_gui.o` |
| `arch/x86/entry/syscall_64.c` | `do_syscall_64` 中拦截 XAPI 号段 |
| `arch/x86/include/asm/xapi.h` | **新增** — `xapi_handle_syscall` / `xapi_gui_handle_syscall` 原型 |

## 图形 XAPI（v2 新增）

`xapi_gui.c` 把 XJ380 的 GUI 号段（P4.1–P4.5）移植为**内核侧 fbdev 窗口系统**：

- **窗口**：`XAPI_CREATE_WINDOW`（`XWINDOW`：宽/高/标题/`sets`，类型位
  `XWIN_NORMAL/FULL_SCR/...`）、`XAPI_SET_WINDOW_TITLE`、`XAPI_CLOSE_WINDOW`、
  `XAPI_GET_WIN_SZIE`。窗口为软件 sheet（后台缓冲 `0xRRGGBBAA`），
  `XWIN_NORMAL` 自带 12/27/20 边框标题栏（与 XJ380 内容偏移一致）。
- **绘图**：`XAPI_DRAW_POINT/LINE/RECT/RECT_FILL/CIRCLE/CIRCLE_FILL`，
  `XAPI_DRAW_TEXT/_L/_SW`（内核控制台位图字体按 `size/16` 最近邻缩放），
  `XAPI_CALC_TEXT_WIDTH`，`XAPI_DRAW_BMP/PICTURE`（免压缩 24bpp BMP 最近邻缩放）。
- **缓冲**：`XAPI_READBUFFER/WRITEBUFFER`（RGB 三元组）与
  `XAPI_READBUFFERA/WRITEBUFFERA`（RGBA），坐标相对内容区、跨距为窗口宽。
- **刷新**：`XAPI_REFRESH_WINDOW` / `XAPI_REFRESH_PART_WINDOW` —— 逐行
  painter's algorithm 合成（alpha 混合）后经 `cfb_imageblit` 上屏，兼容
  DRM fbdev 的 dirty 跟踪。**要求 32bpp framebuffer**（QEMU 默认即满足）。
- **消息**：`XAPI_SET_MSH_PROCOR` 登记回调；键盘事件经
  `register_keyboard_notifier` 进入每窗口队列并 `SIGUSR2` 唤醒属主进程。
  注意：Linux 内核态无法直接调用用户态函数指针，回调本体由 xdesktop 侧
  trampoline 在收到信号后调用（XJ380 原生内核可直接调用，此为移植差异）。

**尚未实现**（返回 `XAPI_ERR`）：PNG/SVG/FA 解码、`XAPI_LOAD_PICTURE`、
`XAPI_GET_PIC_SIZE`、P4.6 控件（Button/Switch/ScrollBar/TextInputBox）。

### 已实现的 XAPI 非 GUI API（节选）

- I/O：`XAPI_OUTPUT` `XAPI_PRINTLINE` `XAPI_ENDLINE` `XAPI_GETCH` `XAPI_INPUT`
- 文件：`XAPI_OPEN_FILE` `XAPI_CLOSE_FILE` `XAPI_READ_FILE` `XAPI_WRITE_FILE`
  `XAPI_MAKEDIR` `XAPI_REMOVEDIR` `XAPI_DELETE_FILE` `XAPI_RENAME_FILE`
- 进程：`XAPI_KILL_PROCESS` `XAPI_GET_TASK_LIST`
- 时间/系统：`XAPI_GET_TIME` `XAPI_GET_TIME_NANO` `XAPI_GET_TIME_X`
  `XAPI_GET_VERSION` `XAPI_GET_CPU_MODEL` `XAPI_GET_MEMORY_SIZE`
  `XAPI_GET_CURRENT_USER` `XAPI_SLEEP`
- 用户/电源：`XAPI_USER_LOGIN` `XAPI_USER_LIST` `XAPI_POWER_ACTION`

完整实现见 `kernel/arch/x86/kernel/xapi.c`（约 500 行，含每个 API 的注释）。

## 应用补丁

在 linux-7.1.8 源码树根目录执行：

```sh
patch -p1 < patches/syscall_64.c.patch
patch -p1 < patches/Makefile.patch
# 然后复制新增文件：
cp kernel/arch/x86/kernel/xapi.c        arch/x86/kernel/
cp kernel/arch/x86/include/asm/xapi.h   arch/x86/include/asm/
```

或直接使用 `kernel/` 目录下的修改后完整文件覆盖对应路径。

## 编译

```sh
make olddefconfig
make -j$(nproc) bzImage
```

内核启动后，XAPI 号段（7380–7470）全部由内核处理：非 GUI 号段走
`xapi.c`，图形号段（7392–7463）走 `xapi_gui.c`（fbdev 窗口系统）。

## 许可证

补丁与新增代码：GPL-2.0（与 Linux 内核一致）。
