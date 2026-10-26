#ifndef AUDIOALGO_NODE_LOG_ABI_V1_H
#define AUDIOALGO_NODE_LOG_ABI_V1_H
#include "node_abi_v1.h"

/* Optional extension: the published node_abi_v1.h layout and entry never change.
   A host probes AA_LOG_ENTRY separately. Missing export means unavailable logging,
   not an invalid v1 processor. A present table must pass exact size/version and
   mandatory-function validation before the package is registered.

   UTF-8 has an explicit byte count, may contain embedded NUL, and is borrowed only
   for emit(). No pointer to message/event storage may escape the callback. A host
   sink must copy into preallocated bounded storage without allocation, blocking,
   file/console I/O or UI access. The producer observes the returned status; full,
   invalid and oversize messages are never silently treated as accepted.

   Calls for one instance are serialized, including set_sink/prepare/process/reset.
   No background producer is permitted by this extension. Different instances may
   overlap. set_sink copies the sink table, not its pointer; context remains host
   owned until serialized set_sink(instance, NULL, error) returns. Stop producers,
   reset/drain on a worker, detach, destroy the instance, then release its module.
   Closing a queue does not preempt an emit already in flight or native user code.
   Never throw C++ exceptions across these C function boundaries. */
#ifdef __cplusplus
extern "C" {
#endif
#define AA_LOG_ABI_V1 1u
#define AA_LOG_ENTRY "audioalgo_node_log_api_v1"
#define AA_LOG_MAX_MESSAGE_BYTES 1024u
#define AA_LOG_DEBUG 0u
#define AA_LOG_INFO 1u
#define AA_LOG_WARNING 2u
#define AA_LOG_ERROR 3u
#define AA_LOG_OK 0u
#define AA_LOG_QUEUE_FULL 1u
#define AA_LOG_INVALID_ARGUMENT 2u
#define AA_LOG_MESSAGE_TOO_LONG 3u
#define AA_LOG_INVALID_UTF8 4u
#define AA_LOG_CLOSED 5u

typedef struct aa_log_message_v1 {
    uint32_t struct_size, abi_version, level, message_bytes;
    const char* message;
} aa_log_message_v1;

typedef struct aa_log_sink_v1 {
    uint32_t struct_size, abi_version;
    void* context;
    uint32_t (AA_CALL *emit)(void* context, const aa_log_message_v1* message);
} aa_log_sink_v1;

typedef struct aa_node_log_api_v1 {
    uint32_t struct_size, abi_version;
    /* NULL sink detaches. Reject incompatible sink size/version or NULL emit.
       No borrowed table/message pointer may be kept after its call. */
    uint32_t (AA_CALL *set_sink)(aa_instance, const aa_log_sink_v1*, aa_error*);
} aa_node_log_api_v1;

/* Same exact-size/version negotiation rule as the independent v1 processor API.
   This declaration does not imply that a particular host implements logging. */
typedef uint32_t (AA_CALL *aa_get_log_api_v1)(uint32_t host_abi, uint32_t host_size,
                                             aa_node_log_api_v1*, aa_error*);
#ifdef __cplusplus
}
#endif
#endif
