/**
 * @file process_image.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace oec {

/**
 * @brief Input/output process image container with bit helpers.
 */
class ProcessImage {
public:
    ProcessImage(std::size_t inputBytes, std::size_t outputBytes)
        : input_(inputBytes, 0U), output_(outputBytes, 0U) {}

    std::vector<std::uint8_t>& inputBytes() noexcept { return input_; }
    const std::vector<std::uint8_t>& inputBytes() const noexcept { return input_; }

    std::vector<std::uint8_t>& outputBytes() noexcept { return output_; }
    const std::vector<std::uint8_t>& outputBytes() const noexcept { return output_; }

    bool readInputBit(std::size_t byteOffset, std::uint8_t bitIndex) const {
        checkBit(byteOffset, bitIndex, input_);
        return ((input_[byteOffset] >> bitIndex) & 0x1U) != 0U;
    }

    bool readOutputBit(std::size_t byteOffset, std::uint8_t bitIndex) const {
        checkBit(byteOffset, bitIndex, output_);
        return ((output_[byteOffset] >> bitIndex) & 0x1U) != 0U;
    }

    void writeOutputBit(std::size_t byteOffset, std::uint8_t bitIndex, bool value) {
        checkBit(byteOffset, bitIndex, output_);
        const auto mask = static_cast<std::uint8_t>(1U << bitIndex);
        if (value) {
            output_[byteOffset] |= mask;
        } else {
            output_[byteOffset] &= static_cast<std::uint8_t>(~mask);
        }
    }

private:
    static void checkBit(std::size_t byteOffset, std::uint8_t bitIndex,
                         const std::vector<std::uint8_t>& bytes) {
        if (bitIndex >= 8U) {
            throw std::out_of_range("bitIndex must be < 8");
        }
        if (byteOffset >= bytes.size()) {
            throw std::out_of_range("byteOffset out of range");
        }
    }

    std::vector<std::uint8_t> input_;
    std::vector<std::uint8_t> output_;
};

} // namespace oec
