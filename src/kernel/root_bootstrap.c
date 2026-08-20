/**
 * @file root_bootstrap.c
 * @brief Initial root/init capability bootstrap state
 */

#include "root_bootstrap.h"
#include "capability.h"
#include "endpoint.h"
#include "factory.h"
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
    root_bootstrap.factory_cap = KERN_INVALID_ID;
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
                                       task_obj_for_cap(root_task->id),
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

    cap_id_t factory_cap = factory_create_root_cap(
        root_task, factory_supported_mask(), CAP_FULL);
    if (factory_cap < 0) {
        cap_delete(task_cap);
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }
    root_bootstrap.factory_cap = factory_cap;

    ep_id_t ep_id = endpoint_create("root", KERN_EP_MSG_SIZE, 4);
    if (ep_id < 0) {
        cap_delete(factory_cap);
        cap_delete(task_cap);
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }

    cap_id_t ep_cap = cap_create_for(root_task, endpoint_obj_for_cap(ep_id), CAP_OBJ_ENDPOINT, CAP_FULL);
    if (ep_cap < 0) {
        (void)endpoint_delete(ep_id);
        cap_delete(factory_cap);
        cap_delete(task_cap);
        root_bootstrap_init();
        return KERN_ERR_RESOURCE;
    }

    root_bootstrap.root_endpoint = ep_id;
    err = root_bootstrap_add_cap(ep_cap, CAP_OBJ_ENDPOINT, CAP_FULL);
    if (err != KERN_OK) {
        cap_delete(ep_cap);
        cap_delete(factory_cap);
        cap_delete(task_cap);
        root_bootstrap_init();
        return err;
    }

    err = root_bootstrap_add_cap(factory_cap, CAP_OBJ_FACTORY, CAP_FULL);
    if (err != KERN_OK) {
        cap_delete(ep_cap);
        (void)endpoint_delete(ep_id);
        cap_delete(factory_cap);
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

    factory_create_request_t request;
    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_TASK;
    request.rights = CAP_FULL;
    request.entry = (uint32_t)(uintptr_t)entry;
    request.arg = (uint32_t)(uintptr_t)arg;
    request.param0 = priority;
    request.param1 = stack_size;
    if (name != NULL) {
        strncpy(request.name, name, sizeof(request.name) - 1U);
    }

    cap_id_t cap = factory_create_for(root_task, root_bootstrap.factory_cap,
                                      &request);
    if (cap < 0) {
        return (kern_err_t)cap;
    }
    void *task_obj = cap_lookup_for(root_task, cap, CAP_OBJ_TASK, CAP_MANAGE);
    if (task_obj == NULL) {
        cap_delete(cap);
        return KERN_ERR_CAP;
    }
    task_id_t tid = task_id_from_obj(task_obj);

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

    task_id_t tid = task_id_from_obj(obj);
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

    task_id_t service_id = task_id_from_obj(obj);
    if (service_id == root_bootstrap.task_id) {
        return KERN_ERR_PARAM;
    }

    tcb_t *service_task = task_get_tcb(service_id);
    if (service_task == NULL || (service_task->attrs & TASK_ATTR_USER) == 0) {
        return KERN_ERR_NOEXIST;
    }

    factory_create_request_t request;
    memset(&request, 0, sizeof(request));
    request.obj_type = CAP_OBJ_ENDPOINT;
    request.rights = CAP_FULL;
    request.param0 = msg_size;
    request.param1 = max_pending;
    if (name != NULL) {
        strncpy(request.name, name, sizeof(request.name) - 1U);
    }

    cap_id_t root_cap = factory_create_for(
        root_task, root_bootstrap.factory_cap, &request);
    if (root_cap < 0) {
        return (kern_err_t)root_cap;
    }
    void *endpoint_obj = cap_lookup_for(root_task, root_cap,
                                        CAP_OBJ_ENDPOINT, CAP_MANAGE);
    if (endpoint_obj == NULL) {
        cap_delete(root_cap);
        return KERN_ERR_CAP;
    }
    ep_id_t ep_id = endpoint_id_from_obj(endpoint_obj);

    cap_id_t service_cap = cap_copy_to(root_task, root_cap, service_task,
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
        out->factory_cap = KERN_INVALID_ID;
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

#if FAULT_ENDPOINT && SUPERVISOR
kern_err_t root_bootstrap_spawn_supervisor(void) {
    if (!root_bootstrap.active) {
        return KERN_ERR_STATE;
    }
    extern void supervisor_monitor_loop(void *arg);
    extern ep_id_t kern_fault_ep;

    task_id_t sup_tid = KERN_INVALID_ID;
    cap_id_t sup_cap = KERN_INVALID_ID;
    kern_err_t err = root_bootstrap_create_service(
        "supervisor", supervisor_monitor_loop, NULL,
        6 /* priority */, 2048 /* stack */, &sup_tid, &sup_cap);
    if (err != KERN_OK) {
        return err;
    }

    tcb_t *sup = task_get_tcb(sup_tid);
    void *fault_obj = (kern_fault_ep >= 0)
        ? endpoint_obj_for_cap(kern_fault_ep) : NULL;
    if (sup == NULL || fault_obj == NULL) {
        return KERN_ERR_STATE;
    }

    /* 铸入 fault ep 的 READ|WRITE cap(无 MANAGE:supervisor 不能删内核
     * fault endpoint)。supervisor 以 self_slot(EP,0) 发现它 —— 它是
     * 该任务 cspace 里唯一的 endpoint cap。 */
    cap_id_t fe = cap_create_for(sup, fault_obj, CAP_OBJ_ENDPOINT,
                                 CAP_READ | CAP_WRITE);
    if (fe < 0) {
        (void)task_delete(sup_tid);
        return (kern_err_t)fe;
    }

    /* P2-1: crashy_app 的重启拉起走 factory(仅 TASK 位,最小权限;
     * 用户直呼 sys_task_create 已封)。 */
    cap_id_t sf = factory_create_root_cap(
        sup, FACTORY_OBJECT_BIT(CAP_OBJ_TASK), CAP_READ | CAP_WRITE);
    if (sf < 0) {
        (void)task_delete(sup_tid);
        return (kern_err_t)sf;
    }

    return root_bootstrap_start_service(sup_cap);
}
#endif /* FAULT_ENDPOINT && SUPERVISOR */

#endif /* CAP_ENABLE */
