/**
 * @file usercopy.h
 * @brief User/kernel boundary pointer validation and bounded copy helpers
 */

#ifndef USERCOPY_H
#define USERCOPY_H

#include <stdint.h>
#include "kernel_types.h"
#include "kernel_config.h"

#if SYSCALL_ENABLE

#define USER_ACCESS_READ   0x01U
#define USER_ACCESS_WRITE  0x02U

int user_access_ok(const void *ptr, uint32_t len, uint32_t access);
kern_err_t copy_from_user(void *dst, const void *user_src, uint32_t len);
kern_err_t copy_to_user(void *user_dst, const void *src, uint32_t len);
kern_err_t strncpy_from_user(char *dst, const char *user_src, uint32_t max_len);

#endif /* SYSCALL_ENABLE */
#endif /* USERCOPY_H */
