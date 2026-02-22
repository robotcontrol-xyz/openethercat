/**
 * @file advanced_systems_tests.cpp
 * @brief openEtherCAT source file.
 */

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "openethercat/config/recovery_profile_loader.hpp"
#include "openethercat/master/coe_mailbox.hpp"
#include "openethercat/master/distributed_clock.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/master/hil_campaign.hpp"
#include "openethercat/master/topology_manager.hpp"
#include "openethercat/transport/mock_transport.hpp"

int main() {
    // Mailbox SDO and emergency path via mock transport.
    {
        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);

        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {{.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x2, .productCode = 0x07d83052}};
        cfg.signals = {{.logicalName = "OutputA", .direction = oec::SignalDirection::Output, .slaveName = "EL2008", .byteOffset = 0, .bitOffset = 0}};

        assert(master.configure(cfg));
        assert(master.start());

        const oec::SdoAddress addr{.index = 0x2000, .subIndex = 1};
        const auto wr = master.sdoDownload(2, addr, {0x34, 0x12});
        assert(wr.success);

        const auto rd = master.sdoUpload(2, addr);
        assert(rd.success);
        assert(rd.data.size() == 2);
        assert(rd.data[0] == 0x34);

        transport.enqueueEmergency({.errorCode = 0x8130, .errorRegister = 0x10, .manufacturerData = {1,2,3,4,5}, .slavePosition = 2});
        const auto emergencies = master.drainEmergencies(4);
        assert(emergencies.size() == 1);
        assert(emergencies[0].errorCode == 0x8130);

        std::string foeError;
        assert(master.foeWriteFile(2, {.fileName = "firmware.bin", .password = 0}, {1, 2, 3, 4}, foeError));
        const auto foeRead = master.foeReadFile(2, {.fileName = "firmware.bin", .password = 0});
        assert(foeRead.success);
        assert(foeRead.data.size() == 4);

        std::string eoeError;
        assert(master.eoeSendFrame(2, {0xDE, 0xAD, 0xBE, 0xEF}, eoeError));
        std::vector<std::uint8_t> frame;
        assert(master.eoeReceiveFrame(2, frame, eoeError));
        assert(frame.size() == 4);

        master.stop();
    }

    // Distributed clock controller trend and clamp behavior.
    {
        oec::DistributedClockController dc({.filterAlpha = 0.3, .kp = 0.2, .ki = 0.01, .correctionClampNs = 1000});
        const auto c1 = dc.update({.referenceTimeNs = 1'000'000, .localTimeNs = 999'200});
        assert(c1.has_value());
        assert(*c1 <= 1000 && *c1 >= -1000);
        const auto c2 = dc.update({.referenceTimeNs = 2'000'000, .localTimeNs = 1'999'100});
        assert(c2.has_value());
        const auto stats = dc.stats();
        assert(stats.samples == 2);
        assert(stats.maxAbsOffsetNs >= 800);
        assert(stats.jitterRmsNs > 0.0);
    }

    // Master DC sync quality monitor lock/loss and degrade policy.
    {
        ::setenv("OEC_DC_SYNC_MONITOR", "1", 1);
        ::setenv("OEC_DC_SYNC_MAX_PHASE_ERROR_NS", "100", 1);
        ::setenv("OEC_DC_SYNC_LOCK_ACQUIRE_CYCLES", "3", 1);
        ::setenv("OEC_DC_SYNC_MAX_OOW_CYCLES", "2", 1);
        ::setenv("OEC_DC_SYNC_HISTORY_WINDOW", "16", 1);
        ::setenv("OEC_DC_SYNC_ACTION", "degrade", 1);

        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);

        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {{.name = "EL1004", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x04c2c52}};
        cfg.signals = {{.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1004", .byteOffset = 0, .bitOffset = 0}};

        assert(master.configure(cfg));
        assert(master.start());

        // Three in-window samples should acquire lock.
        (void)master.updateDistributedClock(1'000'000, 1'000'010);
        (void)master.updateDistributedClock(2'000'000, 2'000'015);
        (void)master.updateDistributedClock(3'000'000, 3'000'020);
        auto q = master.distributedClockQuality();
        assert(q.enabled);
        assert(q.locked);
        assert(q.lockAcquisitions == 1);

        // Two out-of-window samples should lose lock and trigger degrade policy.
        (void)master.updateDistributedClock(4'000'000, 4'000'300);
        (void)master.updateDistributedClock(5'000'000, 5'000'350);
        q = master.distributedClockQuality();
        assert(!q.locked);
        assert(q.lockLosses >= 1);
        assert(q.policyTriggers >= 1);
        assert(master.isDegraded());
        assert(q.jitterP99Ns >= q.jitterP50Ns);

        master.stop();
        ::unsetenv("OEC_DC_SYNC_MONITOR");
        ::unsetenv("OEC_DC_SYNC_MAX_PHASE_ERROR_NS");
        ::unsetenv("OEC_DC_SYNC_LOCK_ACQUIRE_CYCLES");
        ::unsetenv("OEC_DC_SYNC_MAX_OOW_CYCLES");
        ::unsetenv("OEC_DC_SYNC_HISTORY_WINDOW");
        ::unsetenv("OEC_DC_SYNC_ACTION");
    }

    // Topology + hot-connect/missing + redundancy status.
    {
        oec::MockTransport transport(1, 1);
        oec::EthercatMaster master(transport);

        oec::NetworkConfiguration cfg;
        cfg.processImageInputBytes = 1;
        cfg.processImageOutputBytes = 1;
        cfg.slaves = {
            {.name = "EL1008", .alias = 0, .position = 1, .vendorId = 0x2, .productCode = 0x03f03052},
            {.name = "EL2008", .alias = 0, .position = 2, .vendorId = 0x2, .productCode = 0x07d83052},
        };
        cfg.signals = {{.logicalName = "InputA", .direction = oec::SignalDirection::Input, .slaveName = "EL1008", .byteOffset = 0, .bitOffset = 0}};

        assert(master.configure(cfg));
        assert(master.start());

        transport.setDiscoveredSlaves({
            {.position = 1, .vendorId = 0x2, .productCode = 0x03f03052, .online = true},
            {.position = 3, .vendorId = 0x2, .productCode = 0x0AAA0001, .online = true},
        });
        transport.setRedundancyHealthy(false);

        std::string topoError;
        assert(master.refreshTopology(topoError));
        const auto c1 = master.topologyChangeSet();
        assert(c1.generation == 1U);
        assert(c1.changed);
        assert(c1.added.size() == 2U);
        assert(c1.removed.empty());
        const auto r1 = master.redundancyStatus();
        assert(!r1.redundancyHealthy);
        assert(r1.state == oec::EthercatMaster::RedundancyState::RedundancyDegraded);
        const auto missing = master.missingSlaves();
        const auto hot = master.hotConnectedSlaves();
        assert(missing.size() == 1);
        assert(missing[0].position == 2);
        assert(hot.size() == 1);
        assert(hot[0].position == 3);

        // Reconcile updated topology and verify deterministic deltas.
        transport.setDiscoveredSlaves({
            {.position = 1, .vendorId = 0x2, .productCode = 0x03f03052, .online = true},
            {.position = 2, .vendorId = 0x2, .productCode = 0x07d83052, .online = true},
            {.position = 3, .vendorId = 0x2, .productCode = 0x0AAA0001, .online = true},
        });
        transport.setRedundancyHealthy(true);
        assert(master.refreshTopology(topoError));
        const auto c2 = master.topologyChangeSet();
        assert(c2.generation == 2U);
        assert(c2.changed);
        assert(c2.redundancyChanged);
        assert(c2.added.size() == 1U);
        assert(c2.added[0].position == 2U);
        assert(c2.removed.empty());
        assert(c2.updated.empty());
        assert(master.topologyGeneration() == 2U);
        const auto r2 = master.redundancyStatus();
        assert(r2.redundancyHealthy);
        assert(r2.state == oec::EthercatMaster::RedundancyState::RedundantHealthy);
        assert(r2.transitionCount >= 1U);
        const auto rk = master.redundancyKpis();
        assert(rk.degradeEvents == 0U); // policy disabled in this test path.

        master.stop();
    }

    // HIL conformance evaluator.
    {
        oec::HilKpi kpi;
        kpi.cycles = 100000;
        kpi.cycleFailures = 10;
        kpi.recoveryEvents = 12;
        kpi.cycleFailureRate = 0.0001;
        kpi.p99CycleRuntimeUs = 200.0;
        kpi.degradedCycles = 0;

        const auto report = oec::HilCampaignEvaluator::evaluate(kpi, 0.001, 500.0, 100);
        assert(report.rules.size() == 3);
        assert(report.rules[0].passed);
        assert(report.rules[1].passed);
    }

    // Recovery profile loader.
    {
        namespace fs = std::filesystem;
        const auto path = fs::temp_directory_path() / "oec_recovery_profile_loader_test.json";
        {
            std::ofstream f(path);
            f << "{ \"entries\": ["
              << "{ \"alStatusCode\": \"0x001B\", \"action\": \"RetryTransition\" },"
              << "{ \"alStatusCode\": \"0x0014\", \"action\": \"Failover\" }"
              << "] }";
        }

        oec::RecoveryProfile profile;
        std::string error;
        assert(oec::RecoveryProfileLoader::loadFromJsonFile(path.string(), profile, error));
        assert(profile.actionByAlStatusCode.size() == 2);
        assert(profile.actionByAlStatusCode.at(0x0014) == oec::RecoveryAction::Failover);

        fs::remove(path);
    }

    std::cout << "advanced_systems_tests passed\n";
    return 0;
}
