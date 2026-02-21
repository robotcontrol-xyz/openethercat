#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "openethercat/config/eni_esi_models.hpp"
#include "openethercat/core/process_image.hpp"

namespace oec {

class IoMapper {
public:
    using InputCallback = std::function<void(bool)>;

    bool bind(const SignalBinding& binding);

    bool setOutput(ProcessImage& image, const std::string& logicalName, bool value) const;
    bool getInput(const ProcessImage& image, const std::string& logicalName, bool& value) const;

    bool registerInputCallback(const std::string& logicalName, InputCallback callback);

    void dispatchInputChanges(const ProcessImage& image);

private:
    std::unordered_map<std::string, SignalBinding> bindings_;
    std::unordered_map<std::string, InputCallback> callbacks_;
    std::unordered_map<std::string, bool> previousInputState_;
};

} // namespace oec
