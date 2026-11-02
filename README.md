# Linux 安装器（C + GTK3）

一个基于 C 语言和 GTK3 的 Linux 系统安装器前端。程序识别物理硬盘并以列表形式展示，选中后把目标盘路径 `/dev/<设备名>` 作为参数传给 shell 脚本执行，实时回显脚本输出，安装完成后弹窗确认重启。

## 文件结构

| 文件 | 说明 |
|------|------|
| `installer.c` | 主程序（C + GTK3），磁盘识别、界面、脚本调用 |
| `install.sh` | 安装脚本，接收目标磁盘作为第一个参数 |
| `installer` | 编译产物（可执行文件） |

## 编译

```bash
gcc installer.c -o installer $(pkg-config --cflags --libs gtk+-3.0)
```

> 需要安装 GTK3 开发库（`libgtk-3-dev`）和 `pkg-config`。

## 运行

```bash
./installer
```

> 重启（`reboot`）需要 root 权限，普通用户下以 `sudo ./installer` 运行才能使重启生效。

## 界面说明

- **左右固定布局**：左侧为物理硬盘列表，右侧为脚本运行日志。
- **左侧列表**：每项显示 `/dev/设备名`、容量、型号；当前系统所在盘会以红色「⚠ 系统盘」标签标记。
- **右上刷新按钮**：重新扫描并刷新硬盘列表（`view-refresh` 图标）。
- **开始安装**：点击后二次确认，若目标盘为系统盘则额外弹出红色警示；确认后才调用脚本。
- **右侧日志**：实时回显脚本 stdout/stderr，等宽字体置灰显示。
- **完成后弹窗**：脚本结束后弹出「安装完成，是否重启系统？」，选择「是」执行 `reboot`。

## 磁盘识别原理

不依赖 `lsblk` / `findmnt` 等外部命令，直接读取内核 `sysfs` 与 `procfs`，适用于精简的 buildroot 目标环境：

| 信息 | 来源 |
|------|------|
| 枚举物理硬盘 | 遍历 `/sys/block/`，只保留有 `device` 目录的真实硬盘（区分 `loop`/`ram`/`zram` 等虚拟设备） |
| 容量 | `/sys/block/<name>/size`（512 字节扇区数），换算为 GiB |
| 型号 | `/sys/block/<name>/device/model` |
| 系统盘识别 | 读取 `/proc/self/mounts` 找到根分区，再到 `/sys/block/<disk>/<分区>` 反查父盘 |

## install.sh 说明

当前 `install.sh` 为**测试版**，只打印安装流程，不对磁盘做任何实际破坏性操作：

1. 校验参数并检查目标是否为块设备
2. 打印磁盘信息（`lsblk`）
3. 分区（仅打印 `parted` 命令，未实际执行）
4. 格式化（仅打印 `mkfs.ext4`，未实际执行）
5. `sync` 同步数据
6. 安装引导（仅打印 `grub-install`，未实际执行）

### 将脚本改为真实安装

将脚本中的 `echo` 打印命令替换为实际执行即可，例如：

```sh
parted -s "$DISK" mklabel gpt
parted -s "$DISK" mkpart primary ext4 1MiB 100%
mkfs.ext4 "${DISK}p1"
sync
grub-install "$DISK"
```

> 注意：`install.sh` 中的 `lsblk` 在精简 buildroot 环境可能不存在，可将其替换为读取 `/sys/block` 的方式以保持无外部依赖。

## 已解决的问题

| 问题 | 根因 | 修复 |
|------|------|------|
| 硬盘型号乱码、偶发段错误 | `d->model` 复用了 `model` 指针后又 `g_free(model)`（use-after-free） | 所有权正确转移，仅在不使用时才释放 |
| 段错误（virtio 无 model 文件） | `g_strstrip("")` 写入只读字符串字面量 | 改为显式 if/else，仅对堆上非空字符串做 strip |
| 刷新区后磁盘不显示 | 窗口已显示，新插入 row 默认隐藏 | 插入后对每个 row 执行 `gtk_widget_show_all` |
| 型号非法 UTF-8 导致 Pango 报错 | sysfs 型号字段含非标准字节 | 读取后 `g_utf8_validate` + `g_utf8_make_valid` |