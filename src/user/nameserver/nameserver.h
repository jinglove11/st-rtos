/**
 * @file nameserver.h
 * @brief User-space name-server IPC ABI
 */

#ifndef NAMESERVER_H
#define NAMESERVER_H

#include "kernel_types.h"
#include <stdint.h>

#define NS_MAGIC        0x4E535256U
#define NS_NAME_MAX     24U
#define NS_REGISTRY_MAX 16U

#define NS_OP_REGISTER  1U
#define NS_OP_LOOKUP    2U
#define NS_OP_UNREG     3U
#define NS_OP_PING      4U

#define NS_FLAG_NONE    0U

typedef struct {
    uint32_t magic;
    uint16_t opcode;
    uint16_t flags;
    uint32_t seq;
    int32_t status;
} ns_msg_hdr_t;

typedef struct {
    ns_msg_hdr_t hdr;
    char name[NS_NAME_MAX];
    uint32_t owner_badge;
} ns_name_msg_t;

typedef struct {
    uint8_t in_use;
    char name[NS_NAME_MAX];
    cap_id_t endpoint_cap;
    uint8_t rights;
    uint32_t owner_badge;
} ns_entry_t;

int nameserver_service_run(int ep_cap, uint32_t max_requests);
int nameserver_ping(int ns_ep_cap, uint32_t timeout);
int nameserver_register(int ns_ep_cap, const char *name,
                        cap_id_t service_ep_cap, uint32_t owner_badge,
                        uint32_t timeout);
int nameserver_unregister(int ns_ep_cap, const char *name,
                          uint32_t owner_badge, uint32_t timeout);
int nameserver_lookup_begin(int ns_ep_cap, const char *name,
                            cap_id_t inbox_cap, cap_id_t *out_service_cap,
                            uint32_t timeout);
int nameserver_lookup_ack(int inbox_cap);

static inline int ns_opcode_valid(uint16_t opcode) {
    return opcode >= NS_OP_REGISTER && opcode <= NS_OP_PING;
}

static inline void ns_msg_init(ns_msg_hdr_t *hdr, uint16_t opcode,
                               uint32_t seq) {
    if (hdr == NULL) {
        return;
    }
    hdr->magic = NS_MAGIC;
    hdr->opcode = opcode;
    hdr->flags = NS_FLAG_NONE;
    hdr->seq = seq;
    hdr->status = 0;
}

#endif /* NAMESERVER_H */
