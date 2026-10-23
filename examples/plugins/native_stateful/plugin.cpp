#include <audioalgo/node_abi_v1.h>
#include "descriptor.h"
#include <charconv>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

namespace {
struct State { double gain = 1, bias = 0; uint64_t position = 0; uint32_t channels = 0, rate = 0, maximum = 0; };
uint32_t fail(aa_error* e, const char* code, const char* message) noexcept {
    if (e && e->struct_size >= sizeof(aa_error)) {
        std::strncpy(e->code, code, sizeof(e->code)-1);
        std::strncpy(e->message, message, sizeof(e->message)-1);
    }
    return 1;
}
bool number(std::string_view json, std::string_view key, double& value) {
    auto pos = json.find(key);
    if (pos == json.npos || (pos = json.find(':', pos + key.size())) == json.npos) return false;
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return std::from_chars(json.data()+pos, json.data()+json.size(), value).ec == std::errc{};
}
uint32_t AA_CALL create(aa_string id, aa_instance* output, aa_error* e) {
    if (!output || std::string_view(id.data, id.size) != "example.external_stateful")
        return fail(e, "example_type", "Unknown node type");
    *output = new(std::nothrow) State;
    return *output ? 0 : fail(e, "example_memory", "Allocation failed");
}
uint32_t AA_CALL prepare(aa_instance p, const aa_prepare_v1* c, aa_error* e) {
    if (!p || !c || c->struct_size != sizeof(*c)) return fail(e,"example_prepare","Invalid prepare");
    auto& s = *static_cast<State*>(p);
    std::string_view json(c->parameters_json.data, c->parameters_json.size);
    if (!number(json,"\"gain\"",s.gain) || !number(json,"\"bias\"",s.bias))
        return fail(e,"example_parameters","Expected gain and bias numbers");
    s.position=0; s.channels=c->channels; s.rate=c->sample_rate_hz; s.maximum=c->maximum_block_frames;
    return 0;
}
uint32_t AA_CALL process(aa_instance p, const aa_block_v1* b, aa_result_v1* r, aa_error* e) {
    if (!p || !b || !r || b->struct_size!=sizeof(*b) || r->struct_size!=sizeof(*r))
        return fail(e,"example_block","Invalid block");
    auto& s = *static_cast<State*>(p);
    if (b->input_count!=2 || b->output_count!=2 || b->sample_rate_hz!=s.rate || b->frames>s.maximum)
        return fail(e,"example_shape","Expected two inputs and outputs");
    const aa_input_v1 *a=nullptr, *ref=nullptr;
    const aa_output_v1 *main=nullptr, *side=nullptr;
    for(uint32_t i=0;i<b->input_count;++i) {
        auto& port=b->inputs[i];
        if(port.channels!=s.channels) return fail(e,"example_channels","Input channels changed");
        auto id=std::string_view(port.id.data,port.id.size);
        if(id=="primary") a=&port;
        if(id=="reference") ref=&port;
    }
    for(uint32_t i=0;i<b->output_count;++i) {
        auto& port=b->outputs[i];
        if(port.channels!=s.channels) return fail(e,"example_channels","Output channels changed");
        auto id=std::string_view(port.id.data,port.id.size);
        if(id=="main") main=&port;
        if(id=="side") side=&port;
    }
    if(!a || !ref || !main || !side) return fail(e,"example_ports","Missing named port");
    for(uint32_t c=0;c<s.channels;++c) for(uint32_t i=0;i<b->frames;++i) {
        main->data[c][i]=static_cast<float>(PLUGIN_REVISION*s.gain*a->data[c][i]+ref->data[c][i]+s.bias*(s.position+i));
        side->data[c][i]=a->data[c][i]-ref->data[c][i];
    }
    s.position+=b->frames;
    r->consumed_frames=r->produced_frames=b->frames;
    return 0;
}
void AA_CALL reset(aa_instance p) { static_cast<State*>(p)->position=0; }
void AA_CALL destroy(aa_instance p) { delete static_cast<State*>(p); }
}
extern "C" AA_EXPORT uint32_t AA_CALL audioalgo_node_api_v1(uint32_t abi, uint32_t size, aa_node_api_v1* api, aa_error* e) {
    if(abi!=AA_NODE_ABI_V1 || size!=sizeof(*api) || !api) return fail(e,"example_abi","Unsupported ABI or table size");
    *api={sizeof(*api),AA_NODE_ABI_V1,{plugin_manifest,sizeof(plugin_manifest)-1},create,prepare,process,reset,destroy};
    return 0;
}
