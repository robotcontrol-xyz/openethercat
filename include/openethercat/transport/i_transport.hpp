/**
 * @file i_transport.hpp
 * @brief openEtherCAT source file.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

#include "openethercat/core/slave_state.hpp"

namespace oec {

struct SdoAddress;
struct PdoMappingEntry;
struct EmergencyMessage;
struct TopologySnapshot;
struct FoERequest;
struct FoEResponse;
struct NetworkConfiguration;

/**
 * @brief Abstract transport interface used by the EtherCAT master.
 *
 * Implementations provide cyclic process-data exchange plus optional
 * service extensions (state handling, mailbox, topology, and redundancy).
 */
class ITransport {
public:
    virtual ~ITransport() = default;

    /**
     * @brief Open and initialize transport resources.
     * @return true on success, false on failure.
     */
    virtual bool open() = 0;
    /**
     * @brief Close and release transport resources.
     */
    virtual void close() = 0;

    // Exchanges process output bytes with process input bytes.
    /**
     * @brief Perform one cyclic process-data exchange.
     * @param txProcessData output bytes from master to slaves.
     * @param rxProcessData input bytes from slaves to master.
     * @return true when exchange was successful.
     */
    virtual bool exchange(const std::vector<std::uint8_t>& txProcessData,
                          std::vector<std::uint8_t>& rxProcessData) = 0;

    /**
     * @brief Return last transport error string.
     */
    virtual std::string lastError() const = 0;

    virtual std::uint16_t lastWorkingCounter() const { return 0U; }

    virtual bool requestNetworkState(SlaveState) { return false; }
    virtual bool readNetworkState(SlaveState&) { return false; }

    virtual bool requestSlaveState(std::uint16_t, SlaveState) { return false; }
    virtual bool readSlaveState(std::uint16_t, SlaveState&) { return false; }
    virtual bool readSlaveAlStatusCode(std::uint16_t, std::uint16_t&) { return false; }
    virtual bool reconfigureSlave(std::uint16_t) { return false; }
    virtual bool failoverSlave(std::uint16_t) { return false; }

    virtual bool sdoUpload(std::uint16_t, const SdoAddress&, std::vector<std::uint8_t>&, std::uint32_t&,
                           std::string&) {
        return false;
    }
    virtual bool sdoDownload(std::uint16_t, const SdoAddress&, const std::vector<std::uint8_t>&, std::uint32_t&,
                             std::string&) {
        return false;
    }
    virtual bool configurePdo(std::uint16_t, std::uint16_t, const std::vector<PdoMappingEntry>&,
                              std::string&) {
        return false;
    }
    virtual bool pollEmergency(EmergencyMessage&) { return false; }
    virtual bool discoverTopology(TopologySnapshot&, std::string&) { return false; }
    virtual bool isRedundancyLinkHealthy(std::string&) { return false; }
    virtual bool configureProcessImage(const NetworkConfiguration&, std::string&) { return true; }
    virtual bool foeRead(std::uint16_t, const FoERequest&, FoEResponse&, std::string&) { return false; }
    virtual bool foeWrite(std::uint16_t, const FoERequest&, const std::vector<std::uint8_t>&, std::string&) {
        return false;
    }
    virtual bool eoeSend(std::uint16_t, const std::vector<std::uint8_t>&, std::string&) { return false; }
    virtual bool eoeReceive(std::uint16_t, std::vector<std::uint8_t>&, std::string&) { return false; }
};

} // namespace oec
