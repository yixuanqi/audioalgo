#include "plugins/host/registration.h"
#include "audio_editor_application/EditorController.h"
#include "audio_run_service/RunService.h"
#include "audio_project/ProjectFileIO.h"
#include "EditorNodeInspector.h"
#include "EditorNodeLibrary.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <array>
#include <cmath>

namespace {
std::filesystem::path env(const char* name) {
    auto* value=std::getenv(name);
    if(!value) throw std::runtime_error(std::string("Required integration environment: ")+name);
    return std::filesystem::u8path(value);
}
template<typename T> T* child(juce::Component& c,const juce::String& id) {
    if(c.getComponentID()==id) return dynamic_cast<T*>(&c);
    for(auto* p:c.getChildren()) if(auto* found=child<T>(*p,id)) return found;
    return nullptr;
}
}

TEST_CASE("UPN external DLL drives generated controls controller and real run service","[UPN-02][UPN-03]") {
    juce::ScopedJuceInitialiser_GUI messages;
    audioalgo::NodeRegistry registry;
    auto found=audioalgo::register_host_nodes(registry,{env("AUDIOALGO_UPN_PACKAGE")});
    REQUIRE(found.diagnostics.empty());
    REQUIRE(found.loaded_plugin_ids==std::vector<std::string>{"example.external_stateful"});
    const auto* desc=registry.find("example.external_stateful");
    REQUIRE(desc!=nullptr); REQUIRE(desc->ports.size()==4); REQUIRE(desc->parameters.size()==2);
    const auto root=env("AUDIOALGO_UPN_OUTPUT");
    std::ofstream(root/"component-catalog.json")<<audioalgo::node_catalog_json(registry,found);
    auto entries=audioalgo::editor::build_node_library_entries(registry);
    REQUIRE(std::count_if(entries.begin(),entries.end(),[](auto& e){return e.type_id=="example.external_stateful";})==1);
    audioalgo::editor::EditorController controller(registry);
    audioalgo::ProjectModel project; project.project_id="external-plugin-component";project.name="External DLL two instances";
    // Existing M21 multi-source project is supported by Open/runtime; source creation
    // policy in the editor still allows only one interactive File node. This slice
    // loads the sources and tests adding/connecting the NEW external DLL nodes.
    project.nodes={{"a","builtin.file_source",{{"path",(root/"a.wav").string()}},10,10},
                   {"b","builtin.file_source",{{"path",(root/"b.wav").string()}},10,200}};
    controller.adopt_project(project);
    using Status=audioalgo::editor::ProjectEditStatus;
    REQUIRE(controller.add_node(desc->type_id,"first",200,10).status==Status::applied);
    REQUIRE(controller.add_node(desc->type_id,"second",400,10).status==Status::applied);
    for(auto c:std::vector<audioalgo::Connection>{{"a","audio","first","primary"},{"b","audio","first","reference"},{"first","main","second","primary"},{"b","audio","second","reference"}})
        REQUIRE(controller.connect(c).status==Status::applied);
    audioalgo::editor::NodeSelectionState selection;
    audioalgo::editor::NodeInspector inspector(controller,selection,[]{});
    inspector.set_descriptor_registry(registry);
    const auto set_control=[&](const std::string& node,const char* id,double value) {
        inspector.show_node(node);
        auto* slider=child<juce::Slider>(inspector,juce::String("parameter.")+id);
        REQUIRE(slider!=nullptr);
        slider->onDragStart(); slider->setValue(value,juce::sendNotificationSync); slider->onDragEnd();
        CHECK(std::abs(slider->getValue()-value)<1e-12);
        if(std::string(id)=="bias" && value>0)
            CHECK(slider->getTextFromValue(slider->getValue()).contains("0.00001"));
    };
    set_control("first","gain",1.5);set_control("first","bias",0.00001);
    set_control("second","gain",0.5);set_control("second","bias",-0.00002);
    REQUIRE(std::get<double>(controller.node_snapshot("first")->node.parameters.at("gain"))==1.5);
    REQUIRE(std::get<double>(controller.node_snapshot("second")->node.parameters.at("gain"))==0.5);
    audioalgo::RunService service(registry);
    for(auto port:{"main","side"}) {
        REQUIRE(controller.set_selected_output_port("second",port).status==Status::applied);
        const auto path=root/(std::string(port)+".aaproj");
        const auto saved=audioalgo::save_project_file(path,controller.project());
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&saved)) INFO(e->code+": "+e->message+" at "+path.string());
        REQUIRE(std::holds_alternative<std::monostate>(saved));
        auto outcome=service.run({controller.project(),path,root/(std::string("component-")+port),127});
        if(auto* e=std::get_if<audioalgo::Diagnostic>(&outcome)) INFO(e->code+": "+e->message);
        REQUIRE(std::holds_alternative<audioalgo::RunSuccess>(outcome));
    }
}

TEST_CASE("UPN instances retain their DLL after catalog destruction and isolate state","[UPN-05]") {
    std::unique_ptr<audioalgo::AudioNodeProcessor> a,b;
    {
        audioalgo::NodeRegistry registry;
        const auto found=audioalgo::load_native_plugins(registry,{env("AUDIOALGO_UPN_PACKAGE")});
        REQUIRE(found.diagnostics.empty());
        a=registry.create_processor("example.external_stateful","a").processor;
        b=registry.create_processor("example.external_stateful","b").processor;
        REQUIRE(a);REQUIRE(b);
    }
    // Registry, loader result, and factory leases are gone. Only active instances own DLL.
    std::array<bool,2> correct{};
    const auto worker=[&](int index,audioalgo::AudioNodeProcessor& p,double gain) {
        audioalgo::NodeParameters params{{"gain",gain},{"bias",0.01}};
        if(p.prepare({{48000,1},7,&params})) return;
        std::array<float,7> input{},reference{},output{},side{}; input.fill(0.125f);reference.fill(0.25f);
        const float* ap=input.data();const float* bp=reference.data();float* op=output.data();float* sp=side.data();
        std::array<audioalgo::AudioInputPortBlock,2> in={{{"primary",{&ap,1}},{"reference",{&bp,1}}}};
        std::array<audioalgo::AudioOutputPortBlock,2> out={{{"main",{&op,1}},{"side",{&sp,1}}}};
        for(int block=0;block<3;++block) {
            auto result=p.process_named_block({48000,7,in,out});
            if(!std::holds_alternative<audioalgo::AudioBlockResult>(result)) return;
            for(int i=0;i<7;++i) if(std::abs(output[i]-(gain*0.125+0.25+0.01*(block*7+i)))>1e-6) return;
        }
        p.reset();
        if(p.prepare({{48000,1},7,&params})) return;
        if(!std::holds_alternative<audioalgo::AudioBlockResult>(p.process_named_block({48000,7,in,out}))) return;
        correct[index]=std::abs(output[0]-(gain*0.125+0.25))<1e-6;
    };
    std::thread first(worker,0,std::ref(*a),1.5),second(worker,1,std::ref(*b),0.5);
    first.join();second.join();
    CHECK(correct[0]);CHECK(correct[1]);
    a.reset();b.reset(); // DLL-owned destroy runs before the last module lease is released.
}
