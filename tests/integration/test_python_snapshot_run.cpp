#include "audio_run_service/RunService.h"
#include "audio_io/WavFile.h"
#include "plugins/host/registration.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <juce_gui_basics/juce_gui_basics.h>
#include <future>
#include <cmath>
#include <cstdlib>

namespace {
struct Mutation {std::function<void()> before;int prepares=0;};
class MutatingNative final:public audioalgo::AudioNodeProcessor {
public:
    explicit MutatingNative(std::shared_ptr<Mutation> m):mutation_(std::move(m)){}
    audioalgo::NodeProcessOutcome process(const audioalgo::NodeProcessRequest&) override{return audioalgo::Diagnostic{"unsupported","block only"};}
    bool supports_block_processing() const noexcept override{return true;}
    std::optional<audioalgo::Diagnostic> prepare(const audioalgo::NodePrepareContext&) override{++mutation_->prepares;if(mutation_->before){mutation_->before();mutation_->before={};}return {};}
    audioalgo::AudioBlockOutcome process_block(const audioalgo::AudioBlockView& b) override {
        for(std::size_t c=0;c<b.channel_count;++c)for(std::size_t i=0;i<b.frame_count;++i)b.output_channels[c][i]=b.input_channels[c][i]*2;
        return audioalgo::AudioBlockResult{b.frame_count};
    }
private:std::shared_ptr<Mutation> mutation_;
};
audioalgo::RunOutcome run_worker(const audioalgo::RunService& service,const audioalgo::RunRequest& r) {
    juce::ScopedJuceInitialiser_GUI messages;
    auto task=std::async(std::launch::async,[&]{auto result=service.run(r);juce::MessageManager::callAsync([]{juce::MessageManager::getInstance()->stopDispatchLoop();});return result;});
    juce::MessageManager::getInstance()->runDispatchLoop();return task.get();
}
}
TEST_CASE("P3-04 RunService freezes all Python instances before native execution and next request restarts","[P3-04]") {
    using namespace audioalgo;const std::string mutation=GENERATE("replace","delete","environment");INFO(mutation);
    struct Environment {std::string previous=juce::SystemStats::getEnvironmentVariable("AUDIOALGO_PYTHON_EXECUTABLE",{}).toStdString();~Environment(){_putenv_s("AUDIOALGO_PYTHON_EXECUTABLE",previous.c_str());}} environment;
    const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    const auto root=source/"build/runs"/("mixed-freeze-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    std::filesystem::copy(example/"plugin",root/"plugin",std::filesystem::copy_options::recursive);
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
    const auto script=root/"plugin/stateful.py";const auto original=juce::File(script.string()).loadFileAsString();
    const auto modified=original.replace("result=value.samples*np.float32(context.parameters['scale'])","result=value.samples*np.float32(context.parameters['scale'])*np.float32(2)");REQUIRE(original!=modified);
    auto state=std::make_shared<Mutation>();state->before=[&]{if(mutation=="replace")juce::File(script.string()).replaceWithText(modified);else if(mutation=="delete")std::filesystem::remove(script);else _putenv_s("AUDIOALGO_PYTHON_EXECUTABLE",(root/"absent-interpreter.exe").string().c_str());};
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{root/"plugin"}).diagnostics.empty());
    auto descriptor=*registry.find("builtin.gain");descriptor.type_id="fixture.mutate_code";descriptor.parameters.clear();REQUIRE(registry.register_node(descriptor,[state]{return std::make_unique<MutatingNative>(state);}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=descriptor.type_id;project.nodes[1].parameters.clear();
    project.nodes.push_back({"second","example.python_stateful",{{"scale",.25},{"step",.00002}}});project.connections[2].from_node="second";project.connections.push_back({"python","main","second","audio_in"});
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));RunService service(registry);
    const auto input=std::get<AudioBuffer>(read_wav_file(root/"input.wav"));std::vector<juce::var> reports;
    for(int version=1;version<=2;++version) {
        if(version==2){_putenv_s("AUDIOALGO_PYTHON_EXECUTABLE",environment.previous.c_str());REQUIRE(juce::File(script.string()).replaceWithText(modified));}
        const auto run=root/("v"+std::to_string(version));const auto result=run_worker(service,{project,root/"mixed.aaproj",run,127});
        const auto* error=std::get_if<Diagnostic>(&result);INFO((error?error->code:"success"));INFO((error?error->message:""));REQUIRE(std::holds_alternative<RunSuccess>(result));
        const auto output=std::get<AudioBuffer>(read_wav_file(std::get<RunSuccess>(result).output_file));
        for(std::size_t c=0;c<2;++c)for(std::size_t i=0;i<1025;++i) {
            const float factor=version==1?1.f:2.f,position=static_cast<float>(i);
            const float first=static_cast<float>(static_cast<float>(input.channel(c)[i]*2)*.75f*factor)+static_cast<float>(position*.00001f);
            const float second=static_cast<float>(first*.25f*factor)+static_cast<float>(position*.00002f),expected=second*.5f;
            CHECK(std::abs(output.channel(c)[i]-expected)<=2e-6+1e-5*std::abs(expected));
        }
        reports.push_back(juce::JSON::parse(juce::File((run/"run.json").string())));
        REQUIRE(reports.back()["task_artifacts"].size()==2);
        const auto a=reports.back()["task_artifacts"][0]["provenance"],b=reports.back()["task_artifacts"][1]["provenance"];
        CHECK(a["code_sha256"].toString()==b["code_sha256"].toString());
    }
    CHECK(state->prepares==2);
    const auto first=reports[0]["task_artifacts"][0]["provenance"],next=reports[1]["task_artifacts"][0]["provenance"];
    CHECK(first["code_sha256"].toString()!=next["code_sha256"].toString());
    CHECK(static_cast<juce::int64>(first["process_id"])>0);CHECK(first["process_id"]!=next["process_id"]);
}

TEST_CASE("P3-04 relocated replay consumes fixed plugin code and resources after originals disappear","[P3-04][replay]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    const auto root=source/"build/runs"/("mixed-replay-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(!root.lexically_relative(source/"build/runs").empty());
    std::filesystem::copy(example/"plugin",root/"plugin",std::filesystem::copy_options::recursive);REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{root/"plugin"}).diagnostics.empty());
    const auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));RunService service(registry);
    const auto result=run_worker(service,{project,root/"mixed.aaproj",root/"first",127});REQUIRE(std::holds_alternative<RunSuccess>(result));
    const auto report=juce::JSON::parse(juce::File((root/"first/run.json").string()));REQUIRE(report["frozen_python_packages"].isArray());REQUIRE(report["frozen_python_packages"].size()==1);
    const auto relative=std::filesystem::u8path(report["frozen_python_packages"][0].toString().toStdString());CHECK_FALSE(relative.is_absolute());
    const auto expected=std::get<AudioBuffer>(read_wav_file(root/"first/output.wav"));
    // All mutation targets are freshly-created children of the verified private run root.
    const auto resolved=std::filesystem::weakly_canonical(root/"plugin").lexically_relative(std::filesystem::weakly_canonical(source/"build/runs"));
    REQUIRE(!resolved.empty());REQUIRE(*resolved.begin()!="..");REQUIRE(root.is_absolute());
    std::filesystem::remove_all(root/"plugin");std::filesystem::remove(root/"input.wav");std::filesystem::remove(root/"mixed.aaproj");
    std::filesystem::rename(root/"first",root/"relocated");
    const auto artifact=report["task_artifacts"][0];CHECK(artifact["path_base"].toString()=="run_root");CHECK_FALSE(std::filesystem::u8path(artifact["directory"].toString().toStdString()).is_absolute());
    juce::ChildProcess cli;REQUIRE(cli.start(juce::StringArray{AUDIOALGO_CLI_EXE,"--plugin-dir",(root/"relocated"/relative).string(),"run",(root/"relocated/replay.aaproj").string(),(root/"replayed").string(),"127"}));
    const auto output=cli.readAllProcessOutput();REQUIRE(cli.waitForProcessToFinish(10000));INFO(output.toStdString());REQUIRE(cli.getExitCode()==0);
    const auto replay=std::get<AudioBuffer>(read_wav_file(root/"replayed/output.wav"));REQUIRE(replay.frame_count()==expected.frame_count());
    for(std::size_t c=0;c<2;++c)CHECK(std::equal(expected.channel(c).begin(),expected.channel(c).end(),replay.channel(c).begin()));
}

TEST_CASE("P3-04 changed fixed source is rejected without replacing the previous success","[P3-04][snapshot-integrity]") {
    using namespace audioalgo;const auto source=std::filesystem::path(AUDIOALGO_SOURCE_DIR),example=source/"examples/python_stateful_demo";
    const auto root=source/"build/runs"/("mixed-integrity-"+juce::Uuid().toString().toStdString());REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::copy_file(example/"input.wav",root/"input.wav"));
    NodeRegistry registry;REQUIRE(register_host_nodes(registry,{example/"plugin"}).diagnostics.empty());auto state=std::make_shared<Mutation>();
    auto descriptor=*registry.find("builtin.gain");descriptor.type_id="fixture.snapshot_mutation";descriptor.parameters.clear();REQUIRE(registry.register_node(descriptor,[state]{return std::make_unique<MutatingNative>(state);}).registered);
    auto project=std::get<ProjectModel>(load_project_file(example/"mixed.aaproj"));project.nodes[1].type_id=descriptor.type_id;project.nodes[1].parameters.clear();
    REQUIRE(std::holds_alternative<std::monostate>(save_project_file(root/"mixed.aaproj",project)));RunService service(registry);
    const auto baseline=run_worker(service,{project,root/"mixed.aaproj",root/"baseline",127});REQUIRE(std::holds_alternative<RunSuccess>(baseline));const auto hash=std::get<RunSuccess>(baseline).output_sha256;
    state->before=[&]{for(const auto& folder:std::filesystem::directory_iterator(root/"tampered/python-packages")){const auto script=folder.path()/"stateful.py";auto value=juce::File(script.string()).loadFileAsString();juce::File(script.string()).replaceWithText(value+"\n# changed fixed code\n");}};
    const auto rejected=run_worker(service,{project,root/"mixed.aaproj",root/"tampered",127});REQUIRE(std::holds_alternative<Diagnostic>(rejected));CHECK(std::get<Diagnostic>(rejected).code=="python_snapshot_modified");
    CHECK_FALSE(std::filesystem::exists(root/"tampered/complete.json"));CHECK(std::get<RunSuccess>(baseline).output_sha256==hash);CHECK(std::filesystem::exists(root/"baseline/complete.json"));
    const auto recovered=run_worker(service,{project,root/"mixed.aaproj",root/"recovery",127});REQUIRE(std::holds_alternative<RunSuccess>(recovered));CHECK(std::get<RunSuccess>(recovered).output_sha256==hash);
}
