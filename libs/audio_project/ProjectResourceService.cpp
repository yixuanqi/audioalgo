#include "audio_project/ProjectResourceService.h"

#include <system_error>

namespace audioalgo {
namespace {

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Diagnostic resource_error(const std::string& code, const std::string& message) {
    return {code, message};
}

std::filesystem::path absolute_normalized(const std::filesystem::path& path, std::error_code& error) {
    auto result = std::filesystem::absolute(path, error);
    if (!error)
        result = result.lexically_normal();
    return result;
}

} // namespace

ProjectResourcePathOutcome ProjectResourceService::resolve(
    const std::string& reference, const std::filesystem::path& project_file) const {
    if (reference.empty())
        return resource_error("source_path_missing", "File source node requires a non-empty resource path");

    std::filesystem::path resource;
    try {
        resource = std::filesystem::u8path(reference);
    } catch (const std::exception&) {
        return resource_error("source_path_invalid", "File source resource path is not valid UTF-8");
    }

    if (resource.is_relative()) {
        if (project_file.empty())
            return resource_error("project_path_required", "A project path is required to resolve a relative resource");
        const auto base = project_file.parent_path().empty()
            ? std::filesystem::current_path() : project_file.parent_path();
        resource = base / resource;
    }
    resource = resource.lexically_normal();

    std::error_code error;
    const bool regular_file = std::filesystem::is_regular_file(resource, error);
    if (error || !regular_file)
        return resource_error("source_resource_missing", "Source resource is unavailable: " + path_to_utf8(resource));
    return resource;
}

ProjectResourceReferenceOutcome ProjectResourceService::make_reference(
    const std::filesystem::path& selected_resource, const std::filesystem::path& project_file) const {
    if (selected_resource.empty())
        return resource_error("source_resource_missing", "No source resource was selected");

    std::error_code error;
    if (!std::filesystem::is_regular_file(selected_resource, error) || error)
        return resource_error("source_resource_missing", "Selected source resource does not exist: " +
                                                            path_to_utf8(selected_resource));

    auto selected_absolute = absolute_normalized(selected_resource, error);
    if (error)
        return resource_error("source_resource_path_invalid", "Could not normalize the selected source resource path");

    if (!project_file.empty()) {
        auto project_absolute = absolute_normalized(project_file, error);
        if (!error) {
            const auto project_directory = project_absolute.parent_path();
            const auto relative = selected_absolute.lexically_relative(project_directory);
            if (!relative.empty() && relative.is_relative() && *relative.begin() != "..")
                return path_to_utf8(relative);
        }
    }

    return path_to_utf8(selected_absolute);
}

ProjectResourceReferenceOutcome ProjectResourceService::relocate(
    const std::string& missing_reference, const std::filesystem::path& project_file,
    const std::filesystem::path& selected_resource) const {
    const auto previous = resolve(missing_reference, project_file);
    if (std::holds_alternative<std::filesystem::path>(previous))
        return resource_error("source_resource_relocation_not_required",
                              "The existing source resource is still available");
    const auto& diagnostic = std::get<Diagnostic>(previous);
    if (diagnostic.code != "source_resource_missing")
        return diagnostic;
    return make_reference(selected_resource, project_file);
}

} // namespace audioalgo
