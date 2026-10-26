#include <audioalgo/node_log_abi_v1.h>
#include "descriptor.h"
#include <cstring>
#include <new>
#include <array>
#include <string>

namespace {
struct Instance {aa_log_sink_v1 sink{};bool bound=false,fired=false;unsigned mode=0;std::array<char,1025> payload{};};
uint32_t fail(aa_error* error,const char* code,const char* message) noexcept {
    if(error&&error->struct_size==sizeof(aa_error)){
        std::strncpy(error->code,code,sizeof(error->code)-1);error->code[sizeof(error->code)-1]=0;
        std::strncpy(error->message,message,sizeof(error->message)-1);error->message[sizeof(error->message)-1]=0;
    }
    return 1;
}
uint32_t emit(Instance& instance,const char* text,uint32_t bytes) noexcept {
    if(!instance.bound)return AA_LOG_OK;const aa_log_message_v1 message{sizeof(message),AA_LOG_ABI_V1,AA_LOG_INFO,bytes,text};return instance.sink.emit(instance.sink.context,&message);
}
uint32_t AA_CALL create(aa_string id,aa_instance* instance,aa_error* error) noexcept {
    constexpr char type[]="example.sdk_logging";
    if(!instance||!id.data||id.size!=sizeof(type)-1||std::memcmp(id.data,type,id.size))return fail(error,"type_invalid","unexpected type");
    *instance=new(std::nothrow) Instance;return *instance?0:fail(error,"allocation_failed","instance allocation failed");
}
uint32_t AA_CALL prepare(aa_instance opaque,const aa_prepare_v1* context,aa_error* error) noexcept {
    if(!opaque||!context||context->struct_size!=sizeof(*context))return fail(error,"prepare_invalid","invalid prepare");
    auto& instance=*static_cast<Instance*>(opaque);instance.fired=false;instance.payload.fill('F');
    const std::string json(context->parameters_json.data,context->parameters_json.size);
    constexpr const char* modes[]={"ok","empty","full","utf8","length","size","version","level","null"};bool found=false;
    for(unsigned n=0;n<9;++n)if(json.find(std::string("\"")+modes[n]+"\"")!=std::string::npos){instance.mode=n;found=true;break;}
    return found?0:fail(error,"mode_invalid","unknown mode");
}
uint32_t AA_CALL process(aa_instance opaque,const aa_block_v1* block,aa_result_v1* result,aa_error* error) noexcept {
    if(!opaque||!block||block->struct_size!=sizeof(*block)||!result||result->struct_size!=sizeof(*result)||block->input_count!=1||block->output_count!=1)return fail(error,"block_invalid","invalid block");
    const auto& input=block->inputs[0];const auto& output=block->outputs[0];if(input.channels!=output.channels)return fail(error,"channels_invalid","invalid channels");
    for(uint32_t c=0;c<input.channels;++c)for(uint32_t n=0;n<block->frames;++n)output.data[c][n]=input.data[c][n]*2.f;
    auto& instance=*static_cast<Instance*>(opaque);
    if(!instance.fired){instance.fired=true;constexpr char prefix[]="fault-prefix\n";
        if(emit(instance,prefix,sizeof(prefix)-1)!=AA_LOG_OK)return fail(error,"sink_status_mismatch","prefix was rejected");
        aa_log_message_v1 message{sizeof(message),AA_LOG_ABI_V1,AA_LOG_INFO,1,instance.payload.data()};uint32_t expected=AA_LOG_OK,status=AA_LOG_OK;
        if(instance.mode==1){message.message=nullptr;message.message_bytes=0;}
        if(instance.mode==2){for(unsigned n=0;n<63;++n)if(emit(instance,instance.payload.data(),1024)!=AA_LOG_OK)return fail(error,"sink_status_mismatch","queue filled early");message.message_bytes=1024;expected=AA_LOG_QUEUE_FULL;}
        constexpr char invalid[]={static_cast<char>(0xc3),'('};
        if(instance.mode==3){message.message=invalid;message.message_bytes=2;expected=AA_LOG_INVALID_UTF8;}
        if(instance.mode==4){message.message_bytes=1025;expected=AA_LOG_MESSAGE_TOO_LONG;}
        if(instance.mode==5){--message.struct_size;expected=AA_LOG_INVALID_ARGUMENT;}
        if(instance.mode==6){++message.abi_version;expected=AA_LOG_INVALID_ARGUMENT;}
        if(instance.mode==7){message.level=AA_LOG_ERROR+1;expected=AA_LOG_INVALID_ARGUMENT;}
        if(instance.mode==8){message.message=nullptr;expected=AA_LOG_INVALID_ARGUMENT;}
        if(instance.mode&&instance.bound)status=instance.sink.emit(instance.sink.context,&message);
        if(instance.bound&&status!=expected)return fail(error,"sink_status_mismatch","unexpected SDK status");
        if(instance.mode!=2){constexpr const char* statuses[]={"fault-status=0\n","fault-status=1\n","fault-status=2\n","fault-status=3\n","fault-status=4\n","fault-status=5\n"};if(emit(instance,statuses[status],15)!=AA_LOG_OK)return fail(error,"sink_status_mismatch","status log rejected");}
    }
    result->consumed_frames=result->produced_frames=block->frames;return 0;
}
void AA_CALL reset(aa_instance) noexcept {}
void AA_CALL destroy(aa_instance opaque) noexcept {delete static_cast<Instance*>(opaque);}
uint32_t AA_CALL set_sink(aa_instance opaque,const aa_log_sink_v1* sink,aa_error* error) noexcept {
    if(!opaque)return fail(error,"instance_invalid","invalid instance");auto& instance=*static_cast<Instance*>(opaque);
    if(!sink){instance.bound=false;instance.sink={};return 0;}
    if(sink->struct_size!=sizeof(*sink)||sink->abi_version!=AA_LOG_ABI_V1||!sink->emit)return fail(error,"sink_invalid","invalid sink");
    instance.sink=*sink;instance.bound=true;return 0;
}
}
extern "C" AA_EXPORT uint32_t AA_CALL audioalgo_node_api_v1(uint32_t abi,uint32_t size,aa_node_api_v1* out,aa_error* error) noexcept {
    if(abi!=AA_NODE_ABI_V1||size!=sizeof(*out)||!out)return fail(error,"abi_invalid","invalid host ABI");
    *out={sizeof(*out),AA_NODE_ABI_V1,{manifest_json,sizeof(manifest_json)-1},create,prepare,process,reset,destroy};return 0;
}
#if LOG_MODE != 5
extern "C" AA_EXPORT uint32_t AA_CALL audioalgo_node_log_api_v1(uint32_t abi,uint32_t size,aa_node_log_api_v1* out,aa_error* error) noexcept {
    if(abi!=AA_LOG_ABI_V1||size!=sizeof(*out)||!out)return fail(error,"log_abi_invalid","invalid host logging ABI");
    if(LOG_MODE==4)return fail(error,"controlled_log_rejection","controlled negotiation rejection");
    *out={LOG_MODE==1?sizeof(*out)-1:sizeof(*out),LOG_MODE==2?AA_LOG_ABI_V1+1:AA_LOG_ABI_V1,LOG_MODE==3?nullptr:set_sink};return 0;
}
#endif
