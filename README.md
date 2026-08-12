# XJ380 XAPI — Linux 内核补丁（开源版）

把 XJ380 系统的 XAPI 兼容层（非 GUI 部分）直接内置到 Linux 内核的补丁。
基于 **linux-7.1.8** 内核源码。

## 背景

XJ380 XACT 平台二进制通过自定义 syscall（号段 7380–7470，`enter_syscall`）
调用 XAPI。此补丁让 Linux 内核原生识别并处理其中的**非 GUI** API（I/O、
文件系统、进程、时间、内存、用户、电源等），替代原先的独立用户态
兼容层；GUI 部分（窗口/绘图/控件）由 xdesktop 侧内置处理。

## 修改内容

| 文件 | 变更 |
|------|------|
| `arch/x86/kernel/xapi.c` | **新增** — XAPI 非 GUI syscall 处理（7380–7470） |
| `arch/x86/kernel/Makefile` | `obj-y += xapi.o` |
| `arch/x86/entry/syscall_64.c` | `do_syscall_64` 中拦截 XAPI 号段 |
| `arch/x86/include/asm/xapi.h` | **新增** — `xapi_handle_syscall` 原型 |

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

内核启动后，XAPI 号段（7380–7470）由内核处理；GUI 号段由 xdesktop
进程内的兼容层（xapi_gui）处理。

## 许可证

补丁与新增代码：GPL-2.0（与 Linux 内核一致）。
