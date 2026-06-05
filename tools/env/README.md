# U-Boot Environment Tools

本目录包含用于在 Linux 系统中读写 U-Boot 环境变量的工具。

## 工具列表

| 文件 | 说明 |
|------|------|
| `fw_printenv` | 读取环境变量（与 fw_setenv 是同一二进制文件） |
| `fw_setenv` | 写入/删除环境变量（fw_printenv 的符号链接） |
| `fw_env_example` | C 语言示例程序，演示如何使用环境变量工具 |
| `fw_env.config` | 配置文件，定义 MTD 分区信息 |

## 编译

### 方法1：使用旧版工具链（推荐）

```bash
cd /home/zsD/Documents/uboot-imx-dof
make CROSS_COMPILE=/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- envtools
```

### 方法2：使用新版工具链 + 静态链接

```bash
cd /home/zsD/Documents/uboot-imx-dof
make CROSS_COMPILE=/opt/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- \
     HOSTLDFLAGS="-static" envtools
```

### 编译产物

```
tools/env/fw_printenv     - 读取/写入环境变量工具
tools/env/fw_env_example - C 语言示例程序
```

### 编译错误 "cannot find /lib/ld-linux-armhf.so.3"

**错误信息：**
```
/opt/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin/../lib/gcc/arm-linux-gnueabihf/7.5.0/../../../../arm-linux-gnueabihf/bin/ld: cannot find /lib/libc.so.6 inside .../arm-linux-gnueabihf/libc
```

**原因：**
- 动态链接需要运行时链接器 `ld-linux-armhf.so.3`
- 新版 Linaro 工具链 (7.5.0+) 的 sysroot 配置与编译环境不兼容

**解决方法：**
- 使用 `HOSTLDFLAGS="-static"` 静态链接（推荐，无需配置 sysroot）
- 或使用旧版工具链 4.9.4

## fw_env.config 配置

```
/dev/mtd6	0x0000		0x20000		0x20000		4
/dev/mtd7	0x0000		0x20000		0x20000		4
```

格式：`MTD设备 设备偏移 环境大小 Flash扇区大小 扇区数量`

## fw_printenv/fw_setenv 用法

```bash
# 读取所有环境变量
/root/fw_printenv -c /root/fw_env.config

# 读取单个变量
/root/fw_printenv -c /root/fw_env.config bootcmd

# 设置变量
/root/fw_setenv -c /root/fw_env.config bootdelay 5

# 删除变量（只给变量名，不给值）
/root/fw_setenv -c /root/fw_env.config bootdelay
```

## fw_env_example 用法

```bash
# 列出所有环境变量
/root/fw_env_example -c /root/fw_env.config list

# 获取单个变量
/root/fw_env_example -c /root/fw_env.config get bootcmd

# 设置变量
/root/fw_env_example -c /root/fw_env.config set ipaddr 192.168.1.100

# 删除变量
/root/fw_env_example -c /root/fw_env.config delete bootdelay

# 导出为 shell 脚本
/root/fw_env_example -c /root/fw_env.config export /root/env.sh
source /root/env.sh  # 加载到当前 shell
```

## 在 C 代码中使用

### 方法1：调用二进制工具

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int get_uenv(const char *name, char *value, size_t maxlen)
{
    FILE *fp;
    char cmd[256];
    char buf[512];

    snprintf(cmd, sizeof(cmd),
             "/root/fw_printenv -c /root/fw_env.config %s 2>/dev/null", name);

    fp = popen(cmd, "r");
    if (!fp) return -1;

    if (fgets(buf, sizeof(buf), fp)) {
        char *p = strchr(buf, '=');
        if (p) {
            strncpy(value, p + 1, maxlen - 1);
            value[maxlen - 1] = '\0';
            value[strcspn(value, "\n")] = '\0';
            pclose(fp);
            return 0;
        }
    }
    pclose(fp);
    return -1;
}

int set_uenv(const char *name, const char *value)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "/root/fw_setenv -c /root/fw_env.config %s %s", name, value);
    return system(cmd);
}
```

### 方法2：调用 fw_env_example

```c
#include <stdlib.h>

// 读取
system("/root/fw_env_example -c /root/fw_env.config get bootcmd");

// 设置
system("/root/fw_env_example -c /root/fw_env.config set ipaddr 192.168.1.100");

// 列出
system("/root/fw_env_example -c /root/fw_env.config list");
```

## 示例程序源码

参考 `fw_env_example.c`，它演示了如何：
- 使用 `popen()` 调用 fw_printenv/fw_setenv
- 解析和过滤输出
- 导出环境变量为 shell 脚本格式

## 分区信息

| MTD | 名称 | 大小 | 用途 |
|-----|------|------|------|
| mtd6 | env_a | 512KB | 环境变量 (4 个 erase block, 含坏块容差) |
| mtd7 | env_b | 512KB | 环境变量备份 (4 个 erase block, 含坏块容差) |

## 注意事项

1. **CRC 校验**：环境分区有 CRC 保护，损坏时会提示 "Bad CRC"
2. **权限**：需要 root 权限访问 /dev/mtd*
3. **锁文件**：默认使用 `/var/lock/` 下的锁文件防止并发访问
4. **备份分区**：建议同时更新 env 和 env_r 分区以保持一致

## 常见问题

### "Bad CRC" 警告
环境分区数据损坏。解决方法：
- 进入 U-Boot 执行 `saveenv` 重新保存
- 或重新烧写环境分区

### "No such device"
检查：
- MTD 设备是否正确
- `/dev/mtd6`, `/dev/mtd7` 是否存在
- 内核是否编译了 MTD 支持

### "cannot find /lib/ld-linux-armhf.so.3"
这是编译时的链接错误，**不影响目标板运行**。

原因和解决：
1. **使用静态链接编译**（推荐）：
   ```bash
   make  HOSTLDFLAGS="-static" envtools
   ```
2. **换用旧版工具链**：
   ```bash
   make CROSS_COMPILE=/opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- envtools
   ```

### 静态链接 vs 动态链接

| 类型 | 优点 | 缺点 |
|------|------|------|
| **静态链接** | 部署简单，不依赖目标库 | 文件较大 (~400KB vs ~25KB) |
| **动态链接** | 文件小 | 需要完整的 ARM libc 环境 |

目标板部署推荐**静态链接**。
