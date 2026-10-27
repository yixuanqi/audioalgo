#include "EditorPreferences.h"
#include "audio_node_authoring/Draft.h"
#include <juce_core/juce_core.h>
#include <stdexcept>

namespace audioalgo::editor {
namespace {
std::mutex settings_io;
juce::String text(const std::filesystem::path& path) {
    const auto utf8=path.u8string();return juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.data()),static_cast<int>(utf8.size()));
}
std::filesystem::path path(const juce::String& value) {return std::filesystem::u8path(value.toStdString());}
juce::var read(const juce::File& file) {
    if(!file.exists()){auto* object=new juce::DynamicObject;object->setProperty("schema_version",1);return juce::var(object);}
    if(!file.existsAsFile()||file.getSize()>65536)throw std::runtime_error("Editor preferences are not a valid small settings file; existing bytes preserved");
    const auto value=juce::JSON::parse(file.loadFileAsString());
    if(!value.isObject()||value["schema_version"]!=juce::var(1))throw std::runtime_error("Editor preferences are invalid; existing bytes preserved");
    for(const auto* field:{"editor_executable","last_project_directory"}) {
        const auto item=value[field];
        if(!item.isVoid()&&(!item.isString()||(!item.toString().isEmpty()&&!path(item.toString()).is_absolute())))
            throw std::runtime_error("Editor preferences require absolute paths; existing bytes preserved");
    }
    return value;
}
}
std::filesystem::path editor_preferences_file() {
    return path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("AudioAlgo/node-workspace-preferences.json").getFullPathName());
}
EditorPreferencesOutcome EditorPreferencesStore::load() const {
    std::lock_guard lock(settings_io);
    try {const auto value=read(juce::File(text(file_)));return EditorPreferences{path(value["editor_executable"].toString()),path(value["last_project_directory"].toString())};}
    catch(const std::exception& error){return audioalgo::Diagnostic{"editor_preferences_failed",error.what()};}
}
void EditorPreferencesStore::stage(EditorPreferences value) const {std::lock_guard lock(mutex_);latest_=std::move(value);}
std::optional<audioalgo::Diagnostic> EditorPreferencesStore::flush() const {
    std::lock_guard io_lock(settings_io);
    try {
        std::optional<EditorPreferences> selected;{std::lock_guard lock(mutex_);selected=latest_;}
        if(!selected)return {};
        for(const auto& value:{selected->editor_executable,selected->last_project_directory})
            if(!value.empty()&&!value.is_absolute())throw std::runtime_error("Choose absolute editor/project paths");
        const auto file=juce::File(text(file_));const auto original=file.existsAsFile()?std::optional(file.loadFileAsString()):std::nullopt;
        auto value=read(file);value.getDynamicObject()->setProperty("editor_executable",text(selected->editor_executable));
        value.getDynamicObject()->setProperty("last_project_directory",text(selected->last_project_directory));
        const auto bytes=juce::JSON::toString(value,false);
        if(bytes.getNumBytesAsUTF8()>65536)throw std::runtime_error("Editor preferences exceed the settings file size limit");
        if(original&&*original==bytes)return {};
        authoring::ensure_draft_parent_directory(file_);
        juce::TemporaryFile temporary(file);
        if(!temporary.getFile().replaceWithText(bytes)||temporary.getFile().loadFileAsString()!=bytes)
            throw std::runtime_error("Could not write/verify editor preferences");
        const auto current=file.existsAsFile()?std::optional(file.loadFileAsString()):std::nullopt;
        if(current!=original)throw std::runtime_error("Editor preferences changed externally; existing bytes preserved");
        if(!temporary.overwriteTargetFileWithTemporary())throw std::runtime_error("Could not replace editor preferences");
        return {};
    } catch(const std::exception& error){return audioalgo::Diagnostic{"editor_preferences_failed",error.what()};}
}
}
