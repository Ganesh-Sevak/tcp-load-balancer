#include "lb/admin.hpp"

#include <charconv>
#include <string_view>

namespace lb {

std::optional<AdminBackendCommand> parse_admin_backend_command(const std::string& path, std::string& error) {
    constexpr std::string_view prefix{"/backends/"};
    if (!path.starts_with(prefix)) {
        error = "not a backend command path";
        return std::nullopt;
    }

    const auto tail = std::string_view(path).substr(prefix.size());
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= tail.size()) {
        error = "backend command path must be /backends/{id}/{drain|enable}";
        return std::nullopt;
    }

    const auto id_text = tail.substr(0, slash);
    std::size_t backend_id = 0;
    const auto* begin = id_text.data();
    const auto* end = id_text.data() + id_text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, backend_id);
    if (ec != std::errc{} || ptr != end) {
        error = "backend id must be an unsigned integer";
        return std::nullopt;
    }

    const auto action_text = tail.substr(slash + 1);
    if (action_text == "drain") {
        return AdminBackendCommand{backend_id, AdminBackendAction::Drain};
    }
    if (action_text == "enable") {
        return AdminBackendCommand{backend_id, AdminBackendAction::Enable};
    }

    error = "backend action must be drain or enable";
    return std::nullopt;
}

}  // namespace lb
