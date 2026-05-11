/**
 * @file shell.h
 * @brief 交互式 Shell
 */

#ifndef SHELL_H
#define SHELL_H

#include "kernel_config.h"

#if SHELL_ENABLE

void shell_start(void);

/* 测试接口 */
int  shell_split(char *line, char **argv, int max);
void shell_exec(char *line);

#endif /* SHELL_ENABLE */
#endif /* SHELL_H */
