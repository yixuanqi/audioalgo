#include <audioalgo/node_log_abi_v1.h>
#include <stddef.h>
_Static_assert(sizeof(void*) == 8, "consumer profile is x64");
_Static_assert(sizeof(aa_string) == 16, "published v1 string layout");
_Static_assert(sizeof(aa_node_api_v1) == 64, "published v1 API layout");
_Static_assert(offsetof(aa_node_api_v1, create) == 24, "published v1 function offset");
_Static_assert(sizeof(aa_prepare_v1) == 32, "published v1 prepare layout");
_Static_assert(sizeof(aa_block_v1) == 40, "published v1 block layout");
_Static_assert(sizeof(aa_log_message_v1) == 24, "logging message layout");
_Static_assert(sizeof(aa_log_sink_v1) == 24, "logging sink layout");
_Static_assert(sizeof(aa_node_log_api_v1) == 16, "logging API layout");
int audioalgo_c_log_header_check(void) { return AA_NODE_ABI_V1 == 1u && AA_LOG_ABI_V1 == 1u; }
