/**
 * @file production_hardening_tests.cpp
 * @brief openEtherCAT source file.
 */

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "openethercat/config/recovery_profile_loader.hpp"
#include "openethercat/config/config_validator.hpp"
#include "openethercat/master/cycle_controller.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/master/slave_diagnostics.hpp"
#include "openethercat/transport/mock_transport.hpp"

using namespace std::chrono_literals;

namespace {
class NoStateTransport final : public oec::ITransport {
public:
    bool open() override { return true; }
    void close() override {}
    bool exchange(const std::vector<std::uint8_t>& tx, std::vector<std::uint8_t>& rx) override {
        rx.assign(tx.size(), 0U);
        return true;
    }
    std::string lastError() const override { return "unsupported"; }
};
} // namespace

int main() {
    // Validator catches out-of-range mapping.
    {
        oec::NetworkConfiguration bad;
        bad.processImageInputBytes = 1;
        bad.processImageOutputBytes = 1;
        bad.signals = {
            {.logicalName = "BadSignal",
             .direction = oec::SignalDirection::Input,
             .slaveName = "EL1008",
             .byteOffset = 2,
             .bitOffset = 0},
        };

        const auto issues = oec::ConfigurationValidator::validate(bad);
        assert(oec::ConfigurationValidator::hasErrors(issues));
    }

    // Cycle controller drives deterministic periodic cycles.
    {
        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {
            {.name = "EL1008", .alias = 0, .position = 1, .vendorId = 0x00000002, .productCode = 0x03f03052},
            {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x00000002, .productCode = 0x07d83052},
        };
        cfg.signals = {
            {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0},
            {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
        };

        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);
        assert(master.configure(cfg));
        assert(master.start());

        oec::CycleController controller;
        oec::CycleControllerOptions options;
        options.period = 1ms;
        options.stopOnError = true;
        options.maxConsecutiveFailures = 3;

        std::atomic<std::uint64_t> reportCount{0};
        assert(controller.start(master, options, [&](const oec::CycleReport& report) {
            (void)report;
            ++reportCount;
        }));

        std::this_thread::sleep_for(20ms);
        controller.stop();
        master.stop();

        const auto stats = master.statistics();
        assert(reportCount.load() > 0);
        assert(stats.cyclesTotal >= reportCount.load());
        assert(stats.cyclesFailed == 0);
        assert(stats.lastWorkingCounter == 1U);
    }

    // Startup enforces state machine support when enabled.
    {
        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {
            {.name = "EL1008", .alias = 0, .position = 1, .vendorId = 0x00000002, .productCode = 0x03f03052},
        };
        cfg.signals = {
            {.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0},
        };

        NoStateTransport transport;
        oec::EthercatMaster master(transport);
        assert(master.configure(cfg));
        assert(!master.start());

        auto options = oec::EthercatMaster::StateMachineOptions{};
        options.enable = false;
        master.setStateMachineOptions(options);
        assert(master.start());
        master.stop();
    }

    // Recovery policy path: detect degraded slave, decode AL status, and recover.
    {
        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {
            {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x00000002, .productCode = 0x07d83052},
        };
        cfg.signals = {
            {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
        };

        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);
        assert(master.configure(cfg));
        assert(master.start());

        transport.setSlaveAlStatusCode(2, 0x0017U);
        assert(transport.requestSlaveState(2, oec::SlaveState::SafeOp));
        transport.injectExchangeFailures(1);

        assert(!master.runCycle());
        const auto diagnostics = master.collectSlaveDiagnostics();
        assert(diagnostics.size() == 1);
        assert(diagnostics[0].identity.position == 2);
        assert(diagnostics[0].alStatus.name.size() > 0);

        // Master should keep running after recovery attempt.
        assert(master.runCycle());
        master.stop();
    }

    // Recovery profile loader + event history + failover degradation path.
    {
        namespace fs = std::filesystem;
        const auto base = fs::temp_directory_path() / "oec_recovery_profile_test";
        fs::create_directories(base);
        const auto profilePath = base / "profile.json";
        {
            std::ofstream f(profilePath);
            f << "{ \"entries\": ["
              << "{ \"alStatusCode\": \"0x0014\", \"action\": \"Failover\" },"
              << "{ \"alStatusCode\": \"0x0017\", \"action\": \"Reconfigure\" }"
              << "] }";
        }

        oec::RecoveryProfile profile;
        std::string profileError;
        assert(oec::RecoveryProfileLoader::loadFromJsonFile(profilePath.string(), profile, profileError));
        assert(profile.actionByAlStatusCode.size() == 2);

        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {
            {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x00000002, .productCode = 0x07d83052},
        };
        cfg.signals = {
            {.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0},
        };

        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);
        assert(master.configure(cfg));
        assert(master.start());

        auto options = oec::EthercatMaster::RecoveryOptions{};
        options.maxEventHistory = 4;
        master.setRecoveryOptions(options);

        for (const auto& kv : profile.actionByAlStatusCode) {
            master.setRecoveryActionOverride(kv.first, kv.second);
        }

        transport.setSlaveAlStatusCode(2, 0x0014U);
        transport.injectExchangeFailures(1);
        assert(!master.runCycle());
        assert(master.isDegraded());

        const auto events = master.recoveryEvents();
        assert(!events.empty());
        assert(events.back().action == oec::RecoveryAction::Failover);

        master.clearRecoveryEvents();
        assert(master.recoveryEvents().empty());
        master.stop();
        fs::remove_all(base);
    }

    std::cout << "production_hardening_tests passed\n";
    return 0;
}
