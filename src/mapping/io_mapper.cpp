/**
 * @file io_mapper.cpp
 * @brief openEtherCAT source file.
 */

#include "openethercat/mapping/io_mapper.hpp"

namespace oec {

bool IoMapper::bind(const SignalBinding& binding) {
    return bindings_.emplace(binding.logicalName, binding).second;
}

bool IoMapper::setOutput(ProcessImage& image, const std::string& logicalName, bool value) const {
    const auto it = bindings_.find(logicalName);
    if (it == bindings_.end()) {
        return false;
    }
    if (it->second.direction != SignalDirection::Output) {
        return false;
    }
    image.writeOutputBit(it->second.byteOffset, it->second.bitOffset, value);
    return true;
}

bool IoMapper::getInput(const ProcessImage& image, const std::string& logicalName, bool& value) const {
    const auto it = bindings_.find(logicalName);
    if (it == bindings_.end()) {
        return false;
    }
    if (it->second.direction != SignalDirection::Input) {
        return false;
    }
    value = image.readInputBit(it->second.byteOffset, it->second.bitOffset);
    return true;
}

bool IoMapper::registerInputCallback(const std::string& logicalName, InputCallback callback) {
    const auto it = bindings_.find(logicalName);
    if (it == bindings_.end() || it->second.direction != SignalDirection::Input) {
        return false;
    }
    callbacks_[logicalName] = std::move(callback);
    return true;
}

void IoMapper::dispatchInputChanges(const ProcessImage& image) {
    for (const auto& entry : callbacks_) {
        const auto bindingIt = bindings_.find(entry.first);
        if (bindingIt == bindings_.end() || bindingIt->second.direction != SignalDirection::Input) {
            continue;
        }

        const bool current = image.readInputBit(bindingIt->second.byteOffset, bindingIt->second.bitOffset);
        const auto prevIt = previousInputState_.find(entry.first);
        const bool changed = (prevIt == previousInputState_.end()) || (prevIt->second != current);

        if (changed) {
            entry.second(current);
            previousInputState_[entry.first] = current;
        }
    }
}

} // namespace oec
