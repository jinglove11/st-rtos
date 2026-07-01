/**
 * @file root_bootstrap.c
 * @brief Initial root/init capability bootstrap state
 */

#include "root_bootstrap.h"
#include "capability.h"
#include "endpoint.h"
#include "task.h"
#include <string.h>

#if CAP_ENABLE

#define ROOT_BOOTSTRAP_SERVICE_MAX 4

typedef struct {
    uint8_t in_use;
    task_id_t task_id;
    ep_id_t endpoint;
} root_service_record_t;

static root_bootstrap_info_t root_bootstrap;
static root_service_record_t root_services[ROOT_BOOTSTRAP_SERVICE_MAX];

void root_bootstrap_init(void) {
    memset(&root_bootstrap, 0, sizeof(root_bootstrap));
    memset(root_services, 0, sizeof(root_services));
    root_bootstrap.task_id = KERN_INVALID_ID;
    root_bootstrap.root_endpoint = KERN_INVALID_ID;
    for (int i = 0; i < ROOT_BOOTSTRAP_SERVICE_MAX; i++) {
        root_services[i].task_id = KERN_INVALID_ID;
        root_services[i].endpoint = KERN_INVALID_ID;
    }
}

static kern_err_t root_bootstrap_add_cap(cap_id_t cap,
                                         uint8_t obj_type,
                                         uint8_t rights) {
    if (root_bootstrap.cap_count >= ROOT_BOOTSTRAP_CAP_MAX) {
        return KERN_ERR_RESOURCE;
    }

    root_bootstrap_cap_t *entry =
        &root_bootstrap.caps[root_bootstrap.cap_count++];
    entry->cap = cap;
    entry->obj_type = obj_type;
    entry->rights = rights;
    return KERN_OK;
}

static kern_err_t root_bootstrap_track_service_endpoint(task_id_t task_id,
                                                        ep_id_t endpoint) {
    for (int i = 0; i < ROOT_BOOTSTRAP_SERVICE_MAX; i++) {
        if (!root_services[i].in_use) {
            root_services[i].in_use = 1;
            root_services[i].task_id = task_id;
            root_services[i].endpoint = endpoint;
            return KERN_OK;
        }
    }
    return KERN_ERR_RESOURCE;
}

static void root_bootstrap_cleanup_service_endpoint(task_id_t task_id) {
    for (int i = 0; i < ROOT_BOOTSTRAP_SERVICE_MAX; i++) {
        if (root_services[i].in_use && root_services[i].task_id == task_id) {
            if (root_services[i].endpoint >= 0) {
                (void)endpoint_delete(root_services[i].endpoint);
            }
            root_services[i].in_use = 0;
            root_services[i].task_id = KERN_INVALID_ID;
            root_services[i].endpoint = KERN_INVALID_ID;
        }
    }
}

static void root_bootstrap_cleanup_all_service_endpoints(void) {
    for (int i = 0; i < ROOT_BOOTSTRAP_SERVICE_MAX; i++) {
        if (root_services[i].in_use) {
            if (root_services[i].endpoint >= 0) {
                (void)endpoint_delete(root_services[i].endpoint);
            }
            root_services[i].in_use = 0;
            root_services[i].task_id = KERN_INVALID_ID;
            root_services[i].endpoint = KERN_INVALID_ID;
        }
    }
}

kern_err_t root_bootstrap_prepare(tcb_t *root_task) {
    if (root_task == NULL || root_task->id < 0) {
        return KERN_ERR_PARAM;
    }
    if ((root_task->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_PERM;
    }
    if (root_bootstrap.active) {
        return KERN_ERR_BUSY;
    }

    root_bootstrap_init();

    cap_id_t task_cap = cap_create_for(root_task,
                                       (void *)(uintptr_t)(root_task->id + 1),
                                       CAP_OBJ_TASK, CAP_FULL);
    if (task_cap < 0) {
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }

    root_bootstrap.active = 1;
    root_bootstrap.task_id = root_task->id;
    kern_err_t err = root_bootstrap_add_cap(task_cap, CAP_OBJ_TASK, CAP_FULL);
    if (err != KERN_OK) {
        cap_delete(task_cap);
        root_bootstrap_init();
        return err;
    }

    ep_id_t ep_id = endpoint_create("root", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        cap_delete(task_cap);
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }

    cap_id_t ep_cap = cap_create_for(root_task,
                                     (void *)(uintptr_t)(ep_id + 1),
                                     CAP_OBJ_ENDPOINT, CAP_FULL);
    if (ep_cap < 0) {
        (void)endpoint_delete(ep_id);
        cap_delete(task_cap);
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }

    root_bootstrap.root_endpoint = ep_id;
    err = root_bootstrap_add_cap(ep_cap, CAP_OBJ_ENDPOINT, CAP_FULL);
    if (err != KERN_OK) {
        cap_delete(ep_cap);
        cap_delete(task_cap);
        root_bootstrap_init();
        return err;
    }

    return KERN_OK;
}

kern_err_t root_bootstrap_create(const char *name,
                                 task_func_t entry,
                                 void *arg,
                                 uint8_t priority,
                                 uint32_t stack_size,
                                 task_id_t *out_task) {
    if (entry == NULL) {
        return KERN_ERR_PARAM;
    }
    if (root_bootstrap.active) {
        return KERN_ERR_BUSY;
    }

    task_id_t tid = task_create_user(name ? name : "root_init", entry, arg,
                                     priority, stack_size);
    if (tid < 0) {
        return KERN_ERR_RESOURCE;
    }

    tcb_t *root_task = task_get_tcb(tid);
    kern_err_t err = root_bootstrap_prepare(root_task);
    if (err != KERN_OK) {
        (void)task_delete(tid);
        return err;
    }

    if (out_task != NULL) {
        *out_task = tid;
    }
    return KERN_OK;
}

kern_err_t root_bootstrap_start(void) {
    if (!root_bootstrap.active) {
        return KERN_ERR_NOEXIST;
    }
    if (root_bootstrap.started) {
        return KERN_ERR_STATE;
    }

    kern_err_t err = task_start(root_bootstrap.task_id);
    if (err == KERN_OK) {
        root_bootstrap.started = 1;
    }
    return err;
}

kern_err_t root_bootstrap_create_service(const char *name,
                                         task_func_t entry,
                                         void *arg,
                                         uint8_t priority,
                                         uint32_t stack_size,
                                         task_id_t *out_task,
                                         cap_id_t *out_task_cap) {
    if (entry == NULL) {
        return KERN_ERR_PARAM;
    }
    if (!root_bootstrap.active) {
        return KERN_ERR_NOEXIST;
    }

    tcb_t *root_task = task_get_tcb(root_bootstrap.task_id);
    if (root_task == NULL) {
        return KERN_ERR_NOEXIST;
    }

    task_id_t tid = task_create_user(name ? name : "service", entry, arg,
                                     priority, stack_size);
    if (tid < 0) {
        return KERN_ERR_RESOURCE;
    }

    cap_id_t cap = cap_create_for(root_task,
                                  (void *)(uintptr_t)(tid + 1),
                                  CAP_OBJ_TASK, CAP_FULL);
    if (cap < 0) {
        (void)task_delete(tid);
        return KERN_ERR_RESOURCE;
    }

    if (out_task != NULL) {
        *out_task = tid;
    }
    if (out_task_cap != NULL) {
        *out_task_cap = cap;
    }
    return KERN_OK;
}

kern_err_t root_bootstrap_start_service(cap_id_t task_cap) {
    if (!root_bootstrap.active) {
        return KERN_ERR_NOEXIST;
    }

    tcb_t *root_task = task_get_tcb(root_bootstrap.task_id);
    if (root_task == NULL) {
        return KERN_ERR_NOEXIST;
    }

    void *obj = cap_lookup_for(root_task, task_cap,
                               CAP_OBJ_TASK, CAP_MANAGE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }

    task_id_t tid = (task_id_t)((uintptr_t)obj - 1U);
    if (tid == root_bootstrap.task_id) {
        return KERN_ERR_PARAM;
    }

    return task_start(tid);
}

kern_err_t root_bootstrap_create_service_endpoint(cap_id_t service_task_cap,
                                                  const char *name,
                                                  uint16_t msg_size,
                                                  uint16_t max_pending,
                                                  ep_id_t *out_endpoint,
                                                  cap_id_t *out_root_cap,
                                                  cap_id_t *out_service_cap) {
    if (!root_bootstrap.active) {
        return KERN_ERR_NOEXIST;
    }

    tcb_t *root_task = task_get_tcb(root_bootstrap.task_id);
    if (root_task == NULL) {
        return KERN_ERR_NOEXIST;
    }

    void *obj = cap_lookup_for(root_task, service_task_cap,
                               CAP_OBJ_TASK, CAP_MANAGE);
    if (obj == NULL) {
        return KERN_ERR_CAP;
    }

    task_id_t service_id = (task_id_t)((uintptr_t)obj - 1U);
    if (service_id == root_bootstrap.task_id) {
        return KERN_ERR_PARAM;
    }

    tcb_t *service_task = task_get_tcb(service_id);
    if (service_task == NULL || (service_task->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_NOEXIST;
    }

    ep_id_t ep_id = endpoint_create(name ? name : "service",
                                    msg_size, max_pending);
    if (ep_id < 0) {
        return KERN_ERR_RESOURCE;
    }

    cap_id_t root_cap = cap_create_for(root_task,
                                       (void *)(uintptr_t)(ep_id + 1),
                                       CAP_OBJ_ENDPOINT, CAP_FULL);
    if (root_cap < 0) {
        (void)endpoint_delete(ep_id);
        return KERN_ERR_RESOURCE;
    }

    cap_id_t service_cap = cap_create_for(service_task,
                                          (void *)(uintptr_t)(ep_id + 1),
                                          CAP_OBJ_ENDPOINT,
                                          CAP_READ | CAP_WRITE);
    if (service_cap < 0) {
        cap_delete(root_cap);
        (void)endpoint_delete(ep_id);
        return KERN_ERR_RESOURCE;
    }

    kern_err_t err = task_set_initial_arg(service_id,
                                          (void *)(uintptr_t)service_cap);
    if (err != KERN_OK) {
        cap_delete(service_cap);
        cap_delete(root_cap);
        (void)endpoint_delete(ep_id);
        return err;
    }

    err = root_bootstrap_track_service_endpoint(service_id, ep_id);
    if (err != KERN_OK) {
        cap_delete(service_cap);
        cap_delete(root_cap);
        (void)endpoint_delete(ep_id);
        return err;
    }

    if (out_endpoint != NULL) {
        *out_endpoint = ep_id;
    }
    if (out_root_cap != NULL) {
        *out_root_cap = root_cap;
    }
    if (out_service_cap != NULL) {
        *out_service_cap = service_cap;
    }
    return KERN_OK;
}

kern_err_t root_bootstrap_get_info(root_bootstrap_info_t *out) {
    if (out == NULL) {
        return KERN_ERR_PARAM;
    }
    if (!root_bootstrap.active) {
        memset(out, 0, sizeof(*out));
        out->task_id = KERN_INVALID_ID;
        out->root_endpoint = KERN_INVALID_ID;
        return KERN_ERR_NOEXIST;
    }

    *out = root_bootstrap;
    return KERN_OK;
}

void root_bootstrap_cleanup_task(tcb_t *task) {
    if (task == NULL || !root_bootstrap.active) {
        return;
    }
    if (task->id == root_bootstrap.task_id) {
        root_bootstrap_cleanup_all_service_endpoints();
        if (root_bootstrap.root_endpoint >= 0) {
            (void)endpoint_delete(root_bootstrap.root_endpoint);
        }
        root_bootstrap_init();
    } else {
        root_bootstrap_cleanup_service_endpoint(task->id);
    }
}

#endif /* CAP_ENABLE */
