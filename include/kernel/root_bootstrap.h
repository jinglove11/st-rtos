/**
 * @file root_bootstrap.h
 * @brief Root/init bootstrap authority record
 */

#ifndef ROOT_BOOTSTRAP_H
#define ROOT_BOOTSTRAP_H

#include "kernel_types.h"
#include "kernel_config.h"

#if CAP_ENABLE

#define ROOT_BOOTSTRAP_CAP_MAX 8

typedef struct {
    cap_id_t cap;
    uint8_t  obj_type;
    uint8_t  rights;
} root_bootstrap_cap_t;

typedef struct {
    uint8_t active;
    uint8_t started;
    task_id_t task_id;
    ep_id_t root_endpoint;
    cap_id_t factory_cap;
    uint8_t cap_count;
    root_bootstrap_cap_t caps[ROOT_BOOTSTRAP_CAP_MAX];
} root_bootstrap_info_t;

void root_bootstrap_init(void);
kern_err_t root_bootstrap_create(const char *name,
                                 task_func_t entry,
                                 void *arg,
                                 uint8_t priority,
                                 uint32_t stack_size,
                                 task_id_t *out_task);
kern_err_t root_bootstrap_start(void);
kern_err_t root_bootstrap_create_service(const char *name,
                                         task_func_t entry,
                                         void *arg,
                                         uint8_t priority,
                                         uint32_t stack_size,
                                         task_id_t *out_task,
                                         cap_id_t *out_task_cap);
kern_err_t root_bootstrap_start_service(cap_id_t task_cap);
kern_err_t root_bootstrap_create_service_endpoint(cap_id_t service_task_cap,
                                                  const char *name,
                                                  uint16_t msg_size,
                                                  uint16_t max_pending,
                                                  ep_id_t *out_endpoint,
                                                  cap_id_t *out_root_cap,
                                                  cap_id_t *out_service_cap);
kern_err_t root_bootstrap_prepare(tcb_t *root_task);
kern_err_t root_bootstrap_get_info(root_bootstrap_info_t *out);
void root_bootstrap_cleanup_task(tcb_t *task);

#if FAULT_ENDPOINT && SUPERVISOR
/* H1 修复:supervisor 由 root bootstrap 直接创建,并铸入 kern_fault_ep 的
 * READ|WRITE cap(取代 sys_fault_subscribe 的用户态路径 —— 该 syscall
 * 拒绝用户任务,supervisor 只能用这里的初始授权拿到 fault ep)。
 * supervisor 用 sys_cap_self_slot(CAP_OBJ_ENDPOINT, 0) 发现该 cap。 */
kern_err_t root_bootstrap_spawn_supervisor(void);
#endif

#endif /* CAP_ENABLE */

#endif /* ROOT_BOOTSTRAP_H */
