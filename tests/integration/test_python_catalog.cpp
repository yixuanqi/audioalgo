#include "plugins/host/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <fstream>

namespace {
juce::File file(const std::filesystem::path& p) {
    const auto u=p.u8string();return juce::File(juce::String::fromUTF8(reinterpret_cast<const char*>(u.data()),static_cast<int>(u.size())));
}
void save(const std::filesystem::path& p,const juce::var& value) { REQUIRE(file(p).replaceWithText(juce::JSON::toString(value))); }
juce::var fixture() {return juce::JSON::parse(file(std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"tests/fixtures/python_manifest_contract.json"));}
std::filesystem::path root() {
    const auto p=std::filesystem::path(AUDIOALGO_SOURCE_DIR)/"build/runs"/std::filesystem::u8path("Python 中文 catalog "+juce::Uuid().toString().toStdString());
    REQUIRE(std::filesystem::create_directories(p));return p;
}
void edit(juce::var& value,const juce::Array<juce::var>& path,int i,const juce::var& change) {
    if(i+1==path.size()) {
        REQUIRE(value.isObject());auto* object=value.getDynamicObject();const juce::Identifier key(path[i].toString());
        if(static_cast<bool>(change["remove"]))object->removeProperty(key);else object->setProperty(key,change["value"]);return;
    }
    if(path[i].isInt()){auto* a=value.getArray();REQUIRE(a);edit(a->getReference(static_cast<int>(path[i])),path,i+1,change);}
    else {const juce::Identifier key(path[i].toString());auto child=value[key];edit(child,path,i+1,change);value.getDynamicObject()->setProperty(key,child);}
}
}

#if AUDIOALGO_TEST_PYTHON_ENABLED
TEST_CASE("PY-01 C++ and Python share manifest acceptance fixtures","[PY-01][catalog]") {
    const auto contract=fixture();const auto p=root();const auto baseline=juce::JSON::toString(contract["base"]);
    for(const auto& c:*contract["cases"].getArray()) {
        INFO(c["name"].toString().toStdString());auto manifest=juce::JSON::parse(baseline);
        for(const auto& e:*c["edits"].getArray())edit(manifest,*e["path"].getArray(),0,e);
        save(p/"manifest.json",manifest);audioalgo::NodeRegistry registry;
        const auto result=audioalgo::register_host_nodes(registry,{p});const bool accepted=static_cast<bool>(c["valid"]);
        CHECK(result.diagnostics.empty()==accepted);CHECK((registry.find(manifest["nodes"][0]["type_id"].toString().toStdString())!=nullptr)==accepted);
        if(accepted){const auto* d=registry.find("contract.node");REQUIRE(d);REQUIRE(d->parameters.size()==6);CHECK(d->parameters.back().input_resource);}
        CHECK(registry.find("builtin.gain")!=nullptr);
    }
}

TEST_CASE("PY-01 host package registration is atomic and duplicates preserve existing factories","[PY-01][catalog]") {
    const auto p=root();auto manifest=fixture()["base"];save(p/"manifest.json",manifest);
    audioalgo::NodeRegistry registry;auto discovered=audioalgo::register_host_nodes(registry,{p});REQUIRE(discovered.diagnostics.empty());
    const auto before=audioalgo::node_catalog_json(registry,discovered);
    // A fresh package whose last node conflicts must not leak its first descriptor/factory.
    auto second=juce::JSON::parse(juce::JSON::toString(manifest));second.getDynamicObject()->setProperty("plugin_id","contract.second");
    auto fresh=juce::JSON::parse(juce::JSON::toString(manifest["nodes"][0]));fresh.getDynamicObject()->setProperty("type_id","contract.must_not_leak");
    second["nodes"].getArray()->insert(0,fresh);save(p/"manifest.json",second);
    const auto rejected=audioalgo::load_native_plugins(registry,{p});REQUIRE(rejected.diagnostics.size()==1);
    CHECK(rejected.diagnostics[0].code=="python_node_duplicate");CHECK(registry.find("contract.must_not_leak")==nullptr);
    CHECK(registry.create_processor("contract.must_not_leak","missing").processor==nullptr);
    CHECK(audioalgo::node_catalog_json(registry,discovered)==before);
    // Descriptor changed after scan: execution rejects it before importing/starting Python.
    auto processor=registry.create_processor("contract.node","instance");REQUIRE(processor.processor);
    audioalgo::NodeExecutionContext context;context.node_id="instance";audioalgo::NodeParameters parameters;audioalgo::AudioPorts inputs;
    const auto outcome=processor.processor->process({parameters,inputs,context});REQUIRE(std::holds_alternative<audioalgo::Diagnostic>(outcome));
    CHECK(std::get<audioalgo::Diagnostic>(outcome).code=="python_manifest_changed");
    // Duplicate plugin IDs in one deterministic scan cannot add the second package.
    const auto a=p/"01",b=p/"02";REQUIRE(std::filesystem::create_directory(a));REQUIRE(std::filesystem::create_directory(b));
    save(a/"manifest.json",manifest);auto duplicate=juce::JSON::parse(juce::JSON::toString(manifest));duplicate["nodes"][0].getDynamicObject()->setProperty("type_id","contract.duplicate");save(b/"manifest.json",duplicate);
    std::filesystem::remove(p/"manifest.json");audioalgo::NodeRegistry clean;const auto duplicates=audioalgo::register_host_nodes(clean,{p});
    REQUIRE(duplicates.diagnostics.size()==1);CHECK(duplicates.diagnostics[0].code=="plugin_id_duplicate");CHECK(clean.find("contract.duplicate")==nullptr);CHECK(clean.find("contract.node")!=nullptr);
}
#else
TEST_CASE("BLD-03 Python OFF rejects packages without executable half registration and retains native Gain","[BLD-03][PY-01][catalog]") {
    const auto p=root();save(p/"manifest.json",fixture()["base"]);audioalgo::NodeRegistry registry;
    const auto result=audioalgo::register_host_nodes(registry,{p});REQUIRE(result.diagnostics.size()==1);
    CHECK(result.diagnostics[0].code=="python_runtime_disabled");CHECK(result.loaded_plugin_ids.empty());CHECK(registry.find("contract.node")==nullptr);
    CHECK(registry.create_processor("contract.node","python").processor==nullptr);CHECK(registry.create_processor("builtin.gain","native").processor!=nullptr);
    juce::ChildProcess cli;REQUIRE(cli.start(juce::StringArray{AUDIOALGO_CLI_EXE,"--plugin-dir",file(p).getFullPathName(),"--list-nodes"}));
    const auto output=cli.readAllProcessOutput();REQUIRE(cli.waitForProcessToFinish(5000));CHECK(output.contains("python_runtime_disabled"));CHECK_FALSE(output.contains("contract.node"));
}
#endif
