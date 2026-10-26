#include "EditorWorkspace.h"
#include "EditorHostServices.h"
#include "audio_node_authoring_runtime/PythonAuthoring.h"
#include "plugins/builtin/registration.h"
#include "plugins/host/registration.h"
#include <filesystem>
#include <stdexcept>
namespace {
using namespace audioalgo::editor;
class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow() : DocumentWindow("AudioAlgo Editor", background, DocumentWindow::allButtons) {
        setLookAndFeel(&look_and_feel_);
        style_editor_window(*this);
        auto descriptors = std::make_shared<audioalgo::NodeRegistry>();
        const auto discovery = std::async(std::launch::async, [descriptors] {
            return audioalgo::register_host_nodes(*descriptors);
        }).get();
        juce::String plugin_errors;
        for(auto& e:discovery.diagnostics)
            plugin_errors += juce::String::fromUTF8((e.code+" ["+e.plugin_id+"]: "+e.message+"\n").c_str());
        auto authoring_root = path_from_juce_file(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("AudioAlgo/authoring"));
        auto work_directory = authoring_root;
        auto recovery_directory = authoring_root / "drafts";
        try {
            authoring_root = audioalgo::authoring::configured_authoring_root();
            work_directory = authoring_root; recovery_directory = authoring_root / "drafts";
            work_directory = audioalgo::authoring::configured_authoring_work_directory();
            recovery_directory = audioalgo::authoring::configured_authoring_recovery_directory();
        } catch (const std::exception& error) { plugin_errors += "Storage settings could not be read; choose a folder in the draft editor. " + juce::String::fromUTF8(error.what()) + "\n"; }
        plugin_errors += juce::String::fromUTF8(audioalgo::authoring::authoring_launch_diagnostic().c_str());
        if(plugin_errors.isNotEmpty()) {
            juce::Logger::writeToLog(plugin_errors);
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                "Startup / storage diagnostics",plugin_errors);
        }
        setContentOwned(new EditorComponent(
            std::move(descriptors),
            std::make_shared<ProjectFileDocumentService>(),
            std::make_shared<audioalgo::ProjectResourceService>(),
            std::make_shared<JuceAudioPreviewService>(),
            std::make_shared<audioalgo::authoring::FileDraftStorage>(recovery_directory), true,
            std::make_shared<audioalgo::authoring::PythonAuthoring>(authoring_root, std::filesystem::path{}, std::filesystem::path{}, std::function<bool(audioalgo::authoring::ApplyPhase)>{}, work_directory), std::function<bool()>{}, true,
            std::function<void(bool, const std::filesystem::path&, DraftFileActions::Completion)>{},
            [](bool recovery, const std::filesystem::path& path) { return audioalgo::authoring::save_authoring_location(recovery, path); },
            [](const std::filesystem::path& path) { return audioalgo::authoring::save_authoring_library_location(path); }), true);
        const auto* primary_display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto initial_size = primary_display == nullptr
            ? audioalgo::editor::default_editor_window_size
            : audioalgo::editor::make_initial_editor_window_size(
                  primary_display->userArea.getWidth(), primary_display->userArea.getHeight());
        centreWithSize(initial_size.width, initial_size.height);
        setResizable(true, true);
        setResizeLimits(900, 640, 8192, 8192);
        setVisible(true);
    }
    ~MainWindow() override { setLookAndFeel(nullptr); }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    void shutdown_after_editor_jobs(std::function<void()> on_drained) {
        if (auto* editor = dynamic_cast<EditorComponent*>(getContentComponent()))
            editor->begin_shutdown(std::move(on_drained));
        else if (on_drained)
            on_drained();
    }
private:
    EditorLookAndFeel look_and_feel_;
};

class AudioAlgoApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "AudioAlgo Editor"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        window_ = std::make_unique<MainWindow>();
        const auto& parameters = getCommandLineParameterArray();
        if (!parameters.isEmpty()) {
            const auto requested = juce::File::getCurrentWorkingDirectory().getChildFile(parameters[0].unquoted());
            const auto project = requested.existsAsFile() ? requested : juce::File(parameters[0].unquoted());
            if (project.existsAsFile() && project.hasFileExtension(".aaproj")) {
                const auto path = std::filesystem::path(project.getFullPathName().toStdString());
                juce::Component::SafePointer<MainWindow> window{window_.get()};
                juce::MessageManager::callAsync([window, path] {
                    if (window != nullptr)
                        if (auto* editor = dynamic_cast<EditorComponent*>(window->getContentComponent()))
                            editor->open_project_file(path);
                });
            }
        }
        if (getCommandLineParameters().contains("--smoke-test"))
            juce::Timer::callAfterDelay(250, [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    }

    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override {
        if (window_ != nullptr) {
            window_->shutdown_after_editor_jobs([this] { quit(); });
            return;
        }
        quit();
    }

private:
    std::unique_ptr<MainWindow> window_;
};

} // namespace

START_JUCE_APPLICATION(AudioAlgoApplication)
