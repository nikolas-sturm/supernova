#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace terra {

class SseDecoder {
public:
    void push(const std::string_view chunk,
              const std::function<void(const std::string&, const nlohmann::json&)>& listener) {
        constexpr std::size_t maximumBufferedBytes = 8 * 1024 * 1024;
        if (chunk.size() > maximumBufferedBytes - buffer_.size()) {
            throw std::runtime_error("Sol event exceeds 8 MiB.");
        }
        buffer_.append(chunk);
        while (true) {
            const auto end = buffer_.find('\n');
            if (end == std::string::npos) break;
            auto line = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) {
                dispatch(listener);
            } else if (!line.starts_with(':')) {
                const auto separator = line.find(':');
                const auto field = line.substr(0, separator);
                auto value =
                    separator == std::string::npos ? std::string{} : line.substr(separator + 1);
                if (!value.empty() && value.front() == ' ') value.erase(value.begin());
                if (field == "id" && value.find('\0') == std::string::npos) {
                    id_ = std::move(value);
                } else if (field == "data") {
                    if (!data_.empty()) data_ += '\n';
                    data_ += value;
                }
            }
        }
    }

private:
    void dispatch(const std::function<void(const std::string&, const nlohmann::json&)>& listener) {
        if (data_.empty()) return;
        nlohmann::json event;
        try {
            event = nlohmann::json::parse(data_);
        } catch (const std::exception&) {
            throw std::runtime_error("Sol event stream returned malformed JSON.");
        }
        if (!event.is_object() || event.value("schemaVersion", 0) != 1) {
            throw std::runtime_error("Sol event stream returned unsupported schemaVersion.");
        }
        listener(id_, event);
        data_.clear();
    }

    std::string buffer_;
    std::string id_;
    std::string data_;
};

}  // namespace terra
