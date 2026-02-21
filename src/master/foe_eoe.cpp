#include "openethercat/master/foe_eoe.hpp"

namespace oec {

FoeEoeService::FoeEoeService(ITransport& transport) : transport_(transport) {}

FoEResponse FoeEoeService::readFile(std::uint16_t slavePosition, const FoERequest& request) const {
    FoEResponse response;
    std::string error;
    response.success = transport_.foeRead(slavePosition, request, response, error);
    if (!response.success && response.error.empty()) {
        response.error = error;
    }
    return response;
}

bool FoeEoeService::writeFile(std::uint16_t slavePosition,
                              const FoERequest& request,
                              const std::vector<std::uint8_t>& data,
                              std::string& outError) const {
    return transport_.foeWrite(slavePosition, request, data, outError);
}

bool FoeEoeService::sendEthernetOverEthercat(std::uint16_t slavePosition,
                                             const std::vector<std::uint8_t>& frame,
                                             std::string& outError) const {
    return transport_.eoeSend(slavePosition, frame, outError);
}

bool FoeEoeService::receiveEthernetOverEthercat(std::uint16_t slavePosition,
                                                std::vector<std::uint8_t>& frame,
                                                std::string& outError) const {
    return transport_.eoeReceive(slavePosition, frame, outError);
}

} // namespace oec
