#include "EditorWorkspace.h"
#include "EditorHostServices.h"
#include "plugins/builtin/registration.h"
#include "plugins/host/registration.h"
#include <stdexcept>
namespace {
using namespace audioalgo::editor;
class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow() : DocumentWindow("AudioAlgo Editor", background, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        auto descriptors = std::make_shared<audioalgo::NodeRegistry>();
        const auto discovery=audioalgo::register_host_nodes(*descriptors);
        juce::String plugin_errors;
        for(auto& e:discovery.diagnostics)
            plugin_errors += juce::String::fromUTF8((e.code+" ["+e.plugin_id+"]: "+e.message+"\n").c_str());
        if(plugin_errors.isNotEmpty()) {
            juce::Logger::writeToLog(plugin_errors);
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                "Node plugin diagnostics",plugin_errors);
        }
        setContentOwned(new EditorComponent(
            std::move(descriptors),
            std::make_shared<ProjectFileDocumentService>(),
            std::make_shared<audioalgo::ProjectResourceService>(),
            std::make_shared<JuceAudioPreviewService>()), true);
        const auto* primary_display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        const auto initial_size = primary_display == nullptr
            ? audioalgo::editor::default_editor_window_size
            : audioalgo::editor::make_initial_editor_window_size(
                  primary_display->userArea.getWidth(), primary_display->userArea.getHeight());
        centreWithSize(initial_size.width, initial_size.height);
        setResizable(true, true);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    void shutdown_after_editor_jobs(std::function<void()> on_drained) {
        if (auto* editor = dynamic_cast<EditorComponent*>(getContentComponent()))
            editor->begin_shutdown(std::move(on_drained));
        else if (on_drained)
            on_drained();
    }
};

class AudioAlgoApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "AudioAlgo Editor"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String&) override {
        window_ = std::make_unique<MainWindow>();
        if (getCommandLineParameters().contains("--smoke-test"))
            juce::Timer::callAfterDelay(250, [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
    }

    void shutdown() override { window_.reset(); }
    void systemRequestedQuit() override {
        if (quit_requested_)
            return;
        quit_requested_ = true;
        if (window_ != nullptr) {
            window_->shutdown_after_editor_jobs([this] { quit(); });
            return;
        }
        quit();
    }

private:
    std::unique_ptr<MainWindow> window_;
    bool quit_requested_ = false;
};

} // namespace

START_JUCE_APPLICATION(AudioAlgoApplication)
