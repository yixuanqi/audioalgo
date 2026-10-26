#include <audioalgo/node_log_abi_v1.h>
#include "descriptor.h"
#include <cstring>
#include <new>
#include <array>
#include <cstdint>
#include <charconv>
#include <string>

namespace {
struct Instance {aa_log_sink_v1 sink{};bool bound=false;std::uint64_t remaining=0;std::uint32_t message_size=1024;std::array<char,1024> message{};};
uint32_t fail(aa_error* error,const char* code,const char* message) noexcept {
    if(error&&error->struct_size==sizeof(aa_error)){
        std::strncpy(error->code,code,sizeof(error->code)-1);error->code[sizeof(error->code)-1]=0;
        std::strncpy(error->message,message,sizeof(error->message)-1);error->message[sizeof(error->message)-1]=0;
    }
    return 1;
}
void emit(Instance& instance,const char* text,uint32_t bytes) noexcept {
    if(instance.bound){const aa_log_message_v1 message{sizeof(message),AA_LOG_ABI_V1,AA_LOG_INFO,bytes,text};(void)instance.sink.emit(instance.sink.context,&message);}
}
uint32_t AA_CALL create(aa_string id,aa_instance* instance,aa_error* error) noexcept {
    constexpr char type[]="example.sdk_logging";
    if(!instance||!id.data||id.size!=sizeof(type)-1||std::memcmp(id.data,type,id.size))return fail(error,"type_invalid","unexpected type");
    *instance=new(std::nothrow) Instance;return *instance?0:fail(error,"allocation_failed","instance allocation failed");
}
uint32_t AA_CALL prepare(aa_instance opaque,const aa_prepare_v1* context,aa_error* error) noexcept {
    if(!opaque||!context||context->struct_size!=sizeof(*context))return fail(error,"prepare_invalid","invalid prepare");
    auto& instance=*static_cast<Instance*>(opaque);instance.message.fill('F');
    const std::string json(context->parameters_json.data,context->parameters_json.size);auto at=json.find("log_bytes");
    if(at==std::string::npos)return fail(error,"parameter_missing","log_bytes missing");at=json.find(':',at);if(at==std::string::npos)return fail(error,"parameter_missing","log_bytes missing value");
    ++at;while(at<json.size()&&(json[at]==' '||json[at]=='\t'||json[at]=='\r'||json[at]=='\n'))++at;
    const auto parsed=std::from_chars(json.data()+at,json.data()+json.size(),instance.remaining);if(parsed.ec!=std::errc{}||instance.remaining>67108864)return fail(error,"parameter_invalid","log_bytes out of range");
    instance.message_size=1024;
    if(json.find("euro")!=std::string::npos){if(instance.remaining%3)return fail(error,"parameter_invalid","euro bytes must be multiple of3");instance.message_size=1023;for(unsigned i=0;i<1023;i+=3){instance.message[i]=char(0xe2);instance.message[i+1]=char(0x82);instance.message[i+2]=char(0xac);}}
    else if(json.find("nul")!=std::string::npos)instance.message.fill('\0');
    return 0;
}
uint32_t AA_CALL process(aa_instance opaque,const aa_block_v1* block,aa_result_v1* result,aa_error* error) noexcept {
    if(!opaque||!block||block->struct_size!=sizeof(*block)||!result||result->struct_size!=sizeof(*result)||block->input_count!=1||block->output_count!=1)return fail(error,"block_invalid","invalid block");
    const auto& input=block->inputs[0];const auto& output=block->outputs[0];if(input.channels!=output.channels)return fail(error,"channels_invalid","invalid channels");
    for(uint32_t c=0;c<input.channels;++c)for(uint32_t n=0;n<block->frames;++n)output.data[c][n]=input.data[c][n]*2.f;
    auto& instance=*static_cast<Instance*>(opaque);
    for(unsigned n=0;n<64&&instance.remaining;++n){const auto bytes=static_cast<uint32_t>(instance.remaining<instance.message_size?instance.remaining:instance.message_size);emit(instance,instance.message.data(),bytes);instance.remaining-=bytes;}
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
