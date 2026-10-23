#ifndef AUDIOALGO_NODE_ABI_V1_H
#define AUDIOALGO_NODE_ABI_V1_H
/* Stable C ABI v1, Windows x64, IEEE float32 planar PCM. No C++/CRT objects cross
   the boundary. All structs use the platform's default packing (8 on Windows).
   Caller sets struct_size; incompatible version/size must fail before dereference.
   All UTF-8 strings have explicit byte lengths, exclude NUL, and are borrowed.
   API/manifest storage is immutable, module-owned until module release. All other
   pointers (parameters, names, buffers, errors) are caller-owned for this call only.
   create allocates inside DLL; ONLY destroy in that DLL frees the instance.
   One instance per node per run. prepare may allocate; process/reset may not allocate
   or do I/O. Calls for one instance are serialized, distinct instances may overlap.
   Never throw exceptions across any function below. Failure leaves error populated.
   Host retains the module through the last factory, instance and worker. Updates
   require process exit, DLL replacement, then restart; no hot unload/replacement.
   v1 uses full-block N-in/N-out at one common rate/channel count; consumed/produced
   MUST equal frames. EOS/FIFO/latency are not exposed or silently emulated by v1. */
#include <stdint.h>
#ifdef _WIN32
#define AA_CALL __cdecl
#define AA_EXPORT __declspec(dllexport)
#else
#define AA_CALL
#define AA_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define AA_NODE_ABI_V1 1u
#define AA_NODE_ENTRY "audioalgo_node_api_v1"
typedef void* aa_instance;
typedef struct aa_string { const char* data; uint32_t size; } aa_string;
/* DLL writes bounded NUL-terminated strings. Host copies these before next call. */
typedef struct aa_error { uint32_t struct_size; char code[64]; char message[512]; } aa_error;
typedef struct aa_prepare_v1 {
    uint32_t struct_size, sample_rate_hz, channels, maximum_block_frames;
    aa_string parameters_json;
} aa_prepare_v1;
typedef struct aa_input_v1 { aa_string id; uint32_t channels; const float* const* data; } aa_input_v1;
typedef struct aa_output_v1 { aa_string id; uint32_t channels; float* const* data; } aa_output_v1;
typedef struct aa_block_v1 {
    uint32_t struct_size, sample_rate_hz, frames, input_count, output_count;
    const aa_input_v1* inputs;
    const aa_output_v1* outputs;
} aa_block_v1;
typedef struct aa_result_v1 { uint32_t struct_size, consumed_frames, produced_frames; } aa_result_v1;
typedef struct aa_node_api_v1 {
    uint32_t struct_size, abi_version;
    aa_string manifest_json;
    uint32_t (AA_CALL *create)(aa_string type_id, aa_instance* instance, aa_error* error);
    uint32_t (AA_CALL *prepare)(aa_instance instance, const aa_prepare_v1*, aa_error*);
    uint32_t (AA_CALL *process)(aa_instance instance, const aa_block_v1*, aa_result_v1*, aa_error*);
    void (AA_CALL *reset)(aa_instance instance);
    void (AA_CALL *destroy)(aa_instance instance);
} aa_node_api_v1;
/* Returns 0 on success. Copies the function table into host storage; host_size and
   host_abi must match before writing. Unknown future ABI uses a NEW entry symbol. */
typedef uint32_t (AA_CALL *aa_get_api_v1)(uint32_t host_abi, uint32_t host_size, aa_node_api_v1*, aa_error*);
#ifdef __cplusplus
}
#endif
#endif
