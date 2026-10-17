#include "audio_project/ProjectFileIO.h"

#include <juce_core/juce_core.h>

#include <string>
#include <type_traits>

namespace audioalgo {
namespace {

juce::File to_juce_file(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    const std::string utf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return juce::File(juce::String::fromUTF8(utf8.c_str()));
}

std::string to_utf8(const juce::String& value) {
    const auto bytes = value.toUTF8();
    return std::string(bytes.getAddress(), static_cast<std::size_t>(bytes.sizeInBytes() - 1));
}

bool get_string(const juce::DynamicObject& object, const juce::Identifier& key, std::string& out) {
    const auto value = object.getProperty(key);
    if (!value.isString()) return false;
    out = to_utf8(value.toString());
    return true;
}

juce::var parameter_to_var(const ParameterValue& value) {
    return std::visit([](const auto& item) -> juce::var {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::string>)
            return juce::var(juce::String::fromUTF8(item.c_str()));
        else
            return juce::var(item);
    }, value);
}

bool var_to_parameter(const juce::var& value, ParameterValue& out) {
    if (value.isBool()) out = static_cast<bool>(value);
    else if (value.isInt64()) out = static_cast<std::int64_t>(static_cast<juce::int64>(value));
    else if (value.isInt()) out = static_cast<std::int64_t>(static_cast<int>(value));
    else if (value.isDouble()) out = static_cast<double>(value);
    else if (value.isString()) out = to_utf8(value.toString());
    else return false;
    return true;
}

Diagnostic bad_project(const std::string& message) {
    return {"project_invalid", message};
}

} // namespace

ProjectLoadOutcome load_project_file(const std::filesystem::path& path) {
    const auto file = to_juce_file(path);
    if (!file.existsAsFile())
        return Diagnostic{"project_file_missing", "Project file does not exist: " + to_utf8(file.getFullPathName())};

    const auto parsed = juce::JSON::parse(file.loadFileAsString());
    const auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return bad_project("Project JSON root must be an object");

    ProjectModel project;
    const auto schema = root->getProperty("schema_version");
    if (!schema.isInt() && !schema.isInt64())
        return bad_project("schema_version must be an integer");
    project.schema_version = static_cast<std::uint32_t>(static_cast<int>(schema));
    if (project.schema_version != 1)
        return Diagnostic{"project_schema_unsupported", "Only project schema version 1 is supported"};
    if (!get_string(*root, "project_id", project.project_id) || !get_string(*root, "name", project.name))
        return bad_project("project_id and name must be strings");
    project.revision = static_cast<std::uint64_t>(static_cast<juce::int64>(root->getProperty("revision")));
    if (!get_string(*root, "selected_output_node_id", project.selected_output_node_id))
        return bad_project("selected_output_node_id must be a string");

    const auto node_array_var = root->getProperty("nodes");
    const auto* node_array = node_array_var.getArray();
    if (node_array == nullptr)
        return bad_project("nodes must be an array");
    for (const auto& item : *node_array) {
        const auto* object = item.getDynamicObject();
        if (object == nullptr)
            return bad_project("Each node must be an object");
        NodeInstance node;
        if (!get_string(*object, "id", node.id) || !get_string(*object, "type_id", node.type_id))
            return bad_project("Each node requires string id and type_id fields");
        node.x = static_cast<double>(object->getProperty("x"));
        node.y = static_cast<double>(object->getProperty("y"));
        const auto parameters_var = object->getProperty("parameters");
        const auto* parameters = parameters_var.getDynamicObject();
        if (parameters == nullptr)
            return bad_project("Node parameters must be an object");
        const auto& properties = parameters->getProperties();
        for (int index = 0; index < properties.size(); ++index) {
            ParameterValue value;
            if (!var_to_parameter(properties.getValueAt(index), value))
                return bad_project("Node parameter values must be bool, integer, float, or string");
            node.parameters.emplace(to_utf8(properties.getName(index).toString()), std::move(value));
        }
        project.nodes.push_back(std::move(node));
    }

    const auto connection_array_var = root->getProperty("connections");
    const auto* connection_array = connection_array_var.getArray();
    if (connection_array == nullptr)
        return bad_project("connections must be an array");
    for (const auto& item : *connection_array) {
        const auto* object = item.getDynamicObject();
        if (object == nullptr)
            return bad_project("Each connection must be an object");
        Connection connection;
        if (!get_string(*object, "from_node", connection.from_node) ||
            !get_string(*object, "from_port", connection.from_port) ||
            !get_string(*object, "to_node", connection.to_node) ||
            !get_string(*object, "to_port", connection.to_port))
            return bad_project("Each connection requires string node and port IDs");
        project.connections.push_back(std::move(connection));
    }
    return project;
}

ProjectSaveOutcome save_project_file(const std::filesystem::path& path, const ProjectModel& project) {
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("schema_version", static_cast<int>(project.schema_version));
    root->setProperty("project_id", juce::String::fromUTF8(project.project_id.c_str()));
    root->setProperty("name", juce::String::fromUTF8(project.name.c_str()));
    root->setProperty("revision", static_cast<juce::int64>(project.revision));
    root->setProperty("selected_output_node_id", juce::String::fromUTF8(project.selected_output_node_id.c_str()));

    juce::Array<juce::var> nodes;
    for (const auto& node : project.nodes) {
        juce::DynamicObject::Ptr object = new juce::DynamicObject();
        object->setProperty("id", juce::String::fromUTF8(node.id.c_str()));
        object->setProperty("type_id", juce::String::fromUTF8(node.type_id.c_str()));
        object->setProperty("x", node.x);
        object->setProperty("y", node.y);
        juce::DynamicObject::Ptr parameters = new juce::DynamicObject();
        for (const auto& [key, value] : node.parameters)
            parameters->setProperty(juce::Identifier(juce::String::fromUTF8(key.c_str())), parameter_to_var(value));
        object->setProperty("parameters", juce::var(parameters.get()));
        nodes.add(juce::var(object.get()));
    }
    root->setProperty("nodes", juce::var(nodes));

    juce::Array<juce::var> connections;
    for (const auto& connection : project.connections) {
        juce::DynamicObject::Ptr object = new juce::DynamicObject();
        object->setProperty("from_node", juce::String::fromUTF8(connection.from_node.c_str()));
        object->setProperty("from_port", juce::String::fromUTF8(connection.from_port.c_str()));
        object->setProperty("to_node", juce::String::fromUTF8(connection.to_node.c_str()));
        object->setProperty("to_port", juce::String::fromUTF8(connection.to_port.c_str()));
        connections.add(juce::var(object.get()));
    }
    root->setProperty("connections", juce::var(connections));

    const auto json = juce::JSON::toString(juce::var(root.get()), true);
    const auto utf8 = json.toUTF8();
    const auto file = to_juce_file(path);
    if (!file.getParentDirectory().isDirectory())
        return Diagnostic{"project_directory_missing", "Project directory does not exist"};
    if (!file.replaceWithData(utf8.getAddress(), static_cast<std::size_t>(utf8.sizeInBytes() - 1)))
        return Diagnostic{"project_save_failed", "Could not atomically save project JSON"};
    return std::monostate{};
}

} // namespace audioalgo
