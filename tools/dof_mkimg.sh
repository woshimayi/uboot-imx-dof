#!/bin/bash
###
 # @*************************************: 
 # @FilePath     : \kernel_timert:\linux\nfs\mkimg.sh
 # @version      : 
 # @Author       : dof
 # @Date         : 2024-12-10 09:42:49
 # @LastEditors  : dof
 # @LastEditTime : 2024-12-13 14:49:40
 # @Descripttion :  
 # @compile      :  
 # @**************************************: 
### 

# if [ "$1" ];then
#     echo $1
# else
#     echo "请传入根文件系统压缩包"
#     exit 1
# fi

# 目录与文件名
xdir='/home/zs/linux/nfs/rootfs_qemu'
zImage='/home/zs/linux/nfs/zImage_qemu'
dtb='/home/zs/linux/nfs/vexpress-v2p-ca9.dtb'

# xdir='/home/zs/linux/nfs/rootfs'
# zImage='/home/zs/linux/nfs/zImage'
# dtb='/home/zs/linux/nfs/imx6ull-14x14-evk-dof-nand.dtb'

p1='/tmp/p1'
p2='/tmp/p2'
p3='/tmp/p3'
p4='/tmp/p4'
rootfs='rootfs.ext4'

# 先删除
rm -f $rootfs
# rm -rf $xdir
rm -rf $p1
rm -rf $p2
rm -rf $p3
rm -rf $p4

mkdir -p $xdir $p1 $p2

# 根据实际情况指定文件 解压
# tar -xf $1 -C $xdir/
# 创建镜像 由于是 ext4 所以 bs*count 需要是2的n次方
# 大小 视 根文件系统大小而定
dd if=/dev/zero of="$rootfs" bs=1M count=256

# 分区 创建两个分区（一个用来存放kernel和设备树，另一个存放根文件系统）
sgdisk -n 0:0:+20M -c 0:kernel  $rootfs
sgdisk -n 0:0:+50M -c 0:rootfs  $rootfs
sgdisk -n 0:0:+50M -c 0:rootfsa $rootfs
sgdisk -n 0:0:+50M -c 0:rootfsb $rootfs

LOOPDEV=`losetup -f`   # 查找空闲的loop设备
echo $LOOPDEV
losetup $LOOPDEV  $rootfs
partprobe $LOOPDEV
losetup -l
ls /dev/loop*

# 格式化
mkfs.vfat ${LOOPDEV}p1           # uboot 下可以同时使用 fatls mmc 0:1 或 ls mmc 0:1 读取
# mkfs.ext4 ${LOOPDEV}p1         # uboot 下只能使用                     ls mmc 0:1 读取
mkfs.ext4 ${LOOPDEV}p2
mkfs.ext4 ${LOOPDEV}p3
mkfs.ext4 ${LOOPDEV}p4

# 挂载
mount -t vfat ${LOOPDEV}p1 $p1   # 存放kernel和设备树
# mount -t ext4 ${LOOPDEV}p1 $p1   # 存放kernel和设备树
mount -t ext4 ${LOOPDEV}p2 $p2   # 存放根文件系统
mount -t ext4 ${LOOPDEV}p3 $p3   # 存放根文件系统
mount -t ext4 ${LOOPDEV}p4 $p4   # 存放根文件系统
# 查看挂载情况
df -h


# 将 zImage 和 dtb 拷贝到 p1
# 之前编译内核的内核目录（linux-5.4.95 文件夹中）
cp $zImage $p1/
cp $dtb    $p1/
# 将 文件系统中的文件拷贝到 p2
cp $xdir/* $p2/ -arf

# 去掉 root 登录密码
sed -i 's/root:x:0:0:root:/root::0:0:root:/' $p2/etc/passwd

umount $p1 $p2 $p3 $p4
losetup -d $LOOPDEV
# rm -rf $xdir
rm -rf $p1
rm -rf $p2
rm -rf $p3
rm -rf $p4

printf "创建 %s/%s 成功\n\n" "$(pwd)" $rootfs
exit 0
