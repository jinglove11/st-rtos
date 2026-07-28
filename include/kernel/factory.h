/**
 * @file factory.h
 * @brief M2 object Factory capability
 *
 * A Factory cap's badge is an object-type authorization mask.  Normal
 * derive/copy preserves that mask; mint may only reduce it.  CAP_WRITE is
 * required to create an object and the resulting object cap is installed in
 * the caller's CSpace.
 */

#ifndef FACTORY_H
#define FACTORY_H

#include "capability.h"
#include "abi.h"

#if CAP_ENABLE

#ifndef KERN_MAX_FACTORIES
#define KERN_MAX_FACTORIES 4
#endif

#define FACTORY_OBJECT_BIT(type) (UINT32_C(1) << (type))

#define FACTORY_SUPPORTED_MASK                                           \
    (FACTORY_OBJECT_BIT(CAP_OBJ_SEMAPHORE) |                             \
     FACTORY_OBJECT_BIT(CAP_OBJ_MUTEX) |                                 \
     FACTORY_OBJECT_BIT(CAP_OBJ_MQUEUE) |                                \
     FACTORY_OBJECT_BIT(CAP_OBJ_EVENT) |                                 \
     FACTORY_OBJECT_BIT(CAP_OBJ_TIMER) |                                 \
     FACTORY_OBJECT_BIT(CAP_OBJ_TASK) |                                  \
     FACTORY_OBJECT_BIT(CAP_OBJ_ENDPOINT) |                              \
     FACTORY_OBJECT_BIT(CAP_OBJ_CHANNEL) |                               \
     FACTORY_OBJECT_BIT(CAP_OBJ_FRAME))

/* Fixed-size syscall request.  Pointer fields are represented as 32-bit
 * target addresses so this structure remains an explicit Cortex-M ABI.
 * M3-Step2: 以 abi_header_t 开头 (version + size),内核用 ABI_CHECK 校验。 */
typedef struct {
    abi_header_t hdr;         /* M3-Step2: version=0, size=sizeof(this) */
    uint8_t  obj_type;
    uint8_t  rights;       /* zero selects CAP_FULL */
    uint16_t flags;        /* reserved; must be zero */
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
    uint32_t entry;
    uint32_t arg;
    char     name[KERN_TASK_NAME_LEN];
} factory_create_request_t;

_Static_assert(sizeof(factory_create_request_t) ==
               sizeof(abi_header_t) + 4 + 12 + 8 + KERN_TASK_NAME_LEN,
               "factory_create_request_t layout guard");

/* Request parameter mapping:
 * TASK:      entry, arg, param0=priority, param1=stack size
 * SEMAPHORE: param0=initial count, param1=max count
 * MUTEX:     no parameters
 * MQUEUE:    param0=message size, param1=capacity
 * EVENT:     param0=initial flags
 * TIMER:     entry=callback (privileged only), arg, param0=one-shot
 * ENDPOINT:  name, param0=message size, param1=max pending
 * CHANNEL:   param0=message size, param1=SHM size
 * FRAME:     param0=backing size in bytes
 */

void       factory_init(void);
uint32_t   factory_supported_mask(void);
cap_id_t   factory_create_root_cap(tcb_t *owner, uint32_t allowed_mask,
                                   uint8_t rights);
cap_id_t   factory_create_for(tcb_t *caller, cap_id_t factory_cap,
                              const factory_create_request_t *request);

#endif /* CAP_ENABLE */

#endif /* FACTORY_H */
