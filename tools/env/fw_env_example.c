/*
 * fw_env_example.c - 示例程序：如何在 Linux 应用中使用环境变量
 *
 * 编译：make CROSS_COMPILE=... envtools
 *
 * 使用：./fw_env_example -c /root/fw_env.config list
 *       ./fw_env_example -c /root/fw_env.config get bootcmd
 *       ./fw_env_example -c /root/fw_env.config set ipaddr 192.168.1.100
 *       ./fw_env_example -c /root/fw_env.config delete testvar
 *
 * 注意：本程序直接调用 fw_printenv/fw_setenv 二进制工具
 *       这些工具位于 /root/ 目录下
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

/* 命令类型 */
typedef enum {
    CMD_GET,
    CMD_SET,
    CMD_LIST,
    CMD_DELETE,
    CMD_EXPORT
} cmd_type_t;

/* 打印帮助 */
static void print_usage(const char *prog)
{
    printf("Usage: %s -c <config> [command] [args]\n\n", prog);
    printf("Commands:\n");
    printf("  get <name>              Get environment variable\n");
    printf("  set <name> <value>     Set environment variable\n");
    printf("  list                   List all environment variables\n");
    printf("  delete <name>          Delete environment variable\n");
    printf("  export <file>          Export to shell script\n\n");
    printf("Options:\n");
    printf("  -c <config>            Configuration file\n");
    printf("  -h                     Show this help\n\n");
    printf("Example:\n");
    printf("  %s -c /root/fw_env.config list\n", prog);
    printf("  %s -c /root/fw_env.config get bootcmd\n", prog);
    printf("  %s -c /root/fw_env.config set ipaddr 192.168.1.100\n", prog);
}

/* 过滤并执行命令，返回 FILE* 供读取使用 */
static FILE* popen_filtered(const char *cmd, int *exit_code)
{
    FILE *fp = popen(cmd, "r");
    *exit_code = 0;
    return fp;
}

/* 获取环境变量 */
static int do_getenv(const char *config_file, const char *name)
{
    char cmd[512];
    char line[1024];
    FILE *fp;
    int found = 0;
    int exit_code;

    snprintf(cmd, sizeof(cmd), "/root/fw_printenv -c %s %s 2>&1", config_file, name);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to execute fw_printenv: %s\n", strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* 跳过 Warning 行 */
        if (strstr(line, "Warning") || strstr(line, "Bad CRC")) {
            continue;
        }
        /* 找到 name=value */
        if (strstr(line, "=")) {
            printf("%s", line);
            found = 1;
        }
    }

    exit_code = pclose(fp);

    if (!found) {
        printf("Variable '%s' not found\n", name);
        return 1;
    }

    return WEXITSTATUS(exit_code);
}

/* 设置环境变量 */
static int do_setenv(const char *config_file, const char *name, const char *value)
{
    char cmd[768];
    char line[512];
    FILE *fp;
    int exit_code;
    int success = 0;

    snprintf(cmd, sizeof(cmd), "/root/fw_setenv -c %s %s %s 2>&1", config_file, name, value);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to execute fw_setenv: %s\n", strerror(errno));
        return -1;
    }

    /* 读取任何错误输出 */
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Error") || strstr(line, "Failed")) {
            fprintf(stderr, "%s", line);
            success = 0;
        } else if (!strstr(line, "Warning") && !strstr(line, "Bad CRC")) {
            success = 1;
        }
    }

    exit_code = pclose(fp);

    if (exit_code == 0 || success) {
        printf("Set %s=%s\n", name, value);
        return 0;
    }

    return exit_code;
}

/* 删除环境变量 */
static int do_delete(const char *config_file, const char *name)
{
    char cmd[512];
    char line[512];
    FILE *fp;
    int exit_code;

    snprintf(cmd, sizeof(cmd), "/root/fw_setenv -c %s %s 2>&1", config_file, name);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to execute fw_setenv: %s\n", strerror(errno));
        return -1;
    }

    /* 读取输出（删除时只有变量名，无值） */
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Error") || strstr(line, "Failed")) {
            fprintf(stderr, "%s", line);
        }
    }

    exit_code = pclose(fp);
    printf("Deleted %s\n", name);
    return exit_code;
}

/* 列出所有环境变量 */
static int do_list(const char *config_file)
{
    char cmd[512];
    char line[1024];
    FILE *fp;
    int exit_code;

    snprintf(cmd, sizeof(cmd), "/root/fw_printenv -c %s 2>&1", config_file);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to execute fw_printenv: %s\n", strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* 跳过 Warning 行 */
        if (strstr(line, "Warning") || strstr(line, "Bad CRC")) {
            continue;
        }
        printf("%s", line);
    }

    exit_code = pclose(fp);
    return WEXITSTATUS(exit_code);
}

/* 导出环境变量到 shell 脚本格式 */
static int do_export(const char *config_file, const char *filename)
{
    FILE *fp_out;
    FILE *fp;
    char line[1024];
    char cmd[512];
    int exit_code;

    fp_out = fopen(filename, "w");
    if (!fp_out) {
        fprintf(stderr, "Failed to open output file: %s\n", filename);
        return -1;
    }

    fprintf(fp_out, "#!/bin/sh\n");
    fprintf(fp_out, "# U-Boot environment variables (exported by fw_env_example)\n");
    fprintf(fp_out, "# Source this file: source %s\n\n", filename);

    snprintf(cmd, sizeof(cmd), "/root/fw_printenv -c %s 2>/dev/null", config_file);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Failed to execute fw_printenv\n");
        fclose(fp_out);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* 跳过 Warning 行 */
        if (strstr(line, "Warning") || strstr(line, "Bad CRC")) {
            continue;
        }

        /* 解析 name=value 格式 */
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char *name = line;
            char *value = eq + 1;

            /* 去掉末尾换行 */
            name[strcspn(name, "\n\r")] = '\0';
            value[strcspn(value, "\n\r")] = '\0';

            if (strlen(name) > 0) {
                fprintf(fp_out, "export %s='%s'\n", name, value);
            }
        }
    }

    pclose(fp);
    fclose(fp_out);

    printf("Exported to %s (source %s to load)\n", filename, filename);
    return 0;
}

int main(int argc, char *argv[])
{
    int c;
    cmd_type_t cmd = CMD_LIST;
    char *name = NULL;
    char *value = NULL;
    char *outfile = NULL;
    char *config_file = "/root/fw_env.config";

    /* 解析命令行参数 */
    while ((c = getopt(argc, argv, "c:h")) != -1) {
        switch (c) {
        case 'c':
            config_file = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }

    /* 解析命令 */
    if (optind < argc) {
        if (strcmp(argv[optind], "get") == 0) {
            cmd = CMD_GET;
            name = argv[++optind];
        } else if (strcmp(argv[optind], "set") == 0) {
            cmd = CMD_SET;
            name = argv[++optind];
            value = argv[++optind];
        } else if (strcmp(argv[optind], "list") == 0) {
            cmd = CMD_LIST;
        } else if (strcmp(argv[optind], "delete") == 0) {
            cmd = CMD_DELETE;
            name = argv[++optind];
        } else if (strcmp(argv[optind], "export") == 0) {
            cmd = CMD_EXPORT;
            outfile = argv[++optind];
        } else {
            fprintf(stderr, "Unknown command: %s\n", argv[optind]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* 执行命令 */
    switch (cmd) {
    case CMD_GET:
        if (!name) {
            fprintf(stderr, "Missing variable name\n");
            return 1;
        }
        return do_getenv(config_file, name);

    case CMD_SET:
        if (!name || !value) {
            fprintf(stderr, "Missing name or value\n");
            return 1;
        }
        return do_setenv(config_file, name, value);

    case CMD_LIST:
        return do_list(config_file);

    case CMD_DELETE:
        if (!name) {
            fprintf(stderr, "Missing variable name\n");
            return 1;
        }
        return do_delete(config_file, name);

    case CMD_EXPORT:
        if (!outfile) {
            fprintf(stderr, "Missing output filename\n");
            return 1;
        }
        return do_export(config_file, outfile);
    }

    return 0;
}
