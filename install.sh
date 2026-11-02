#!/bin/sh
# 测试安装脚本：接收目标硬盘设备作为第一个参数。
# 本脚本仅打印流程，不对磁盘做任何实际破坏性操作。
set -e

DISK="$1"

if [ -z "$DISK" ]; then
    echo "用法: $0 <目标硬盘设备，如 /dev/sda>" >&2
    exit 1
fi

if [ ! -b "$DISK" ]; then
    echo "错误: $DISK 不是有效的块设备。" >&2
    exit 1
fi

echo "======================================"
echo " 安装目标磁盘: $DISK"
echo "======================================"
sleep 3
echo ""
echo "[1] 磁盘信息:"
lsblk -o NAME,SIZE,MODEL,TYPE,MOUNTPOINT "$DISK"
sleep 3
echo ""
echo "[2] 分区 (测试，未实际执行):"
echo "    parted $DISK mklabel gpt"
echo "    parted $DISK mkpart primary 1MiB 100%"
sleep 3
echo ""
echo "[3] 格式化 (测试，未实际执行):"
echo "    mkfs.ext4 ${DISK}p1"
sleep 3
echo ""
echo "[4] sync 同步数据:"
sync
echo "    sync 完成"
sleep 3
echo ""
echo "[5] 安装引导 (测试，未实际执行):"
echo "    grub-install $DISK"
sleep 3
echo ""
echo "======================================"
echo " 安装完成 (测试流程)"
echo "======================================"
exit 0