#pragma once

#include <string>

namespace audioalgo {

struct Diagnostic {
    std::string code;
    std::string message;
    std::string node_id;
    std::string port_id;
};

} // namespace audioalgo
