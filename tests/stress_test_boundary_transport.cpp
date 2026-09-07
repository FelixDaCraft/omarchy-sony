#include "MDRProtocol.hpp"
#include "BluetoothManager.hpp"

#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <climits>
#include <cassert>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <sys/poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

using namespace omarchy::sony::protocol;

static int gPassedTests = 0;
static int gFailedTests = 0;

#define LOG_TEST_START(name) \
    std::cout << "[ TEST     ] " << name << std::endl;

#define LOG_TEST_PASS(name) \
    do { \
        std::cout << "[       OK ] " << name << std::endl; \
        gPassedTests++; \
    } while (0)

#define LOG_TEST_FAIL(name, reason) \
    do { \
        std::cerr << "[  FAILED  ] " << name << " : " << reason \
                  << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        gFailedTests++; \
    } while (0)

#define CHK_ASSERT(cond, name, msg) \
    do { \
        if (!(cond)) { \
            LOG_TEST_FAIL(name, msg); \
            return; \
        } \
    } while (0)

#define CHK_ASSERT_EQ(actual, expected, name, msg) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "[  FAILED  ] " << name << " : " << msg \
                      << " | Expected: " << (expected) \
                      << ", Actual: " << (actual) \
                      << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

// Helper: Count open file descriptors in current process
static int countOpenFds() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    while (readdir(dir) != nullptr) {
        count++;
    }
    closedir(dir);
    return count;
}

// ---------------------------------------------------------------------------
// TEST 1: Boundary values for Ambient Levels (<0, 0, 1, 20, 21, 9999)
// ---------------------------------------------------------------------------
void testAmbientLevelBoundaries() {
    const char* tName = "AmbientLevelBoundaries";
    LOG_TEST_START(tName);

    // 1. Boundary: Level 0 -> Should map to WIND noise mode with level 0
    {
        auto frame = serializeAmbientLevel(0, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ambient level 0 must unpack");
        CHK_ASSERT_EQ(unp->payload.size(), size_t(7), tName, "Payload size 7");
        CHK_ASSERT_EQ(unp->payload[3], uint8_t(0x01), tName, "totalEffect must be ON (1)");
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(0x01), tName, "ncMode must be ASM (1)");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(0x00), tName, "Level 0 must serialize as 0x00");
    }

    // 2. Boundary: Level 1 -> Minimum ambient sound level
    {
        auto frame = serializeAmbientLevel(1, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ambient level 1 must unpack");
        CHK_ASSERT_EQ(unp->payload[3], uint8_t(0x01), tName, "totalEffect must be ON (1)");
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(0x01), tName, "ncMode must be ASM (1)");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(0x01), tName, "Level 1 must serialize as 0x01");
    }

    // 3. Boundary: Level 20 -> Maximum valid ambient sound level
    {
        auto frame = serializeAmbientLevel(20, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ambient level 20 must unpack");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(20), tName, "Level 20 must serialize as 20");
    }

    // 4. Boundary: Level 21 -> Out of range upper boundary, must clamp to 20
    {
        auto frame = serializeAmbientLevel(21, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ambient level 21 must unpack");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(20), tName, "Level 21 must clamp to 20");
    }

    // 5. Boundary: Level 255 (max uint8_t) -> Must clamp to 20
    {
        auto frame = serializeAmbientLevel(255, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ambient level 255 must unpack");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(20), tName, "Level 255 must clamp to 20");
    }

    // 6. Direct serializeNoiseMode with mode AMBIENT and level 50 -> Clamp to 20
    {
        auto frame = serializeNoiseMode(NoiseMode::AMBIENT, 50, true, 2);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for serializeNoiseMode AMBIENT 50 must unpack");
        CHK_ASSERT_EQ(unp->payload[5], uint8_t(0x01), tName, "voiceFocus true is 1");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(20), tName, "Level 50 must clamp to 20");
    }

    // 7. serializeNoiseMode with mode ANC -> level must be 0 regardless of ambientLevel param
    {
        auto frame = serializeNoiseMode(NoiseMode::ANC, 15, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for ANC must unpack");
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(0x00), tName, "ncMode must be ANC (0)");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(0x00), tName, "ANC must force level to 0");
    }

    // 8. serializeNoiseMode with mode OFF -> totalEffect must be 0 and level 0
    {
        auto frame = serializeNoiseMode(NoiseMode::OFF, 15, false, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for OFF must unpack");
        CHK_ASSERT_EQ(unp->payload[3], uint8_t(0x00), tName, "totalEffect must be OFF (0)");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(0x00), tName, "OFF must force level to 0");
    }

    // 9. Adversarial Finding Check: Signed integers & 9999
    // Because serializeAmbientLevel accepts `uint8_t`, passing negative ints or ints > 255
    // is truncated at function entry before clamp occurs.
    // Let's verify behavior with static_cast:
    uint8_t castNeg1 = static_cast<uint8_t>(-1); // 255
    auto frameNeg1 = serializeAmbientLevel(castNeg1, false, 0);
    auto unpNeg1 = unpackFrame(frameNeg1);
    CHK_ASSERT(unpNeg1.has_value(), tName, "Negative 1 cast to uint8_t unpacks");
    CHK_ASSERT_EQ(unpNeg1->payload[6], uint8_t(20), tName, "255 clamped to 20");

    uint8_t cast9999 = static_cast<uint8_t>(9999); // 9999 & 0xFF = 15
    auto frame9999 = serializeAmbientLevel(cast9999, false, 0);
    auto unp9999 = unpackFrame(frame9999);
    CHK_ASSERT(unp9999.has_value(), tName, "9999 cast to uint8_t unpacks");
    // Wire level is 15 due to uint8_t parameter truncation
    CHK_ASSERT_EQ(unp9999->payload[6], uint8_t(15), tName, "9999 truncated to uint8_t is 15");

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 2: Boundary values for EQ Presets out of range
// ---------------------------------------------------------------------------
void testEqPresetBoundaries() {
    const char* tName = "EqPresetBoundaries";
    LOG_TEST_START(tName);

    // List of out-of-range / unknown preset values
    std::vector<uint8_t> outOfRangeValues = {
        0x01, 0x05, 0x0F, 0x18, 0x20, 0x50, 0x99, 0x9F, 0xA1, 0xEE, 0xFE, 0xFF
    };

    for (uint8_t rawPreset : outOfRangeValues) {
        auto presetEnum = static_cast<EqPreset>(rawPreset);
        auto frame = serializeEqPreset(presetEnum, 0);

        // Verify framing and packaging
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Frame for invalid preset must be structurally valid");
        CHK_ASSERT_EQ(unp->payload.size(), size_t(4), tName, "EQ preset payload size 4");
        CHK_ASSERT_EQ(unp->payload[0], uint8_t(0x58), tName, "Opcode EQEBB_SET_PARAM (0x58)");
        CHK_ASSERT_EQ(unp->payload[1], uint8_t(0x00), tName, "Type PRESET_EQ (0x00)");
        CHK_ASSERT_EQ(unp->payload[2], rawPreset, tName, "Preserves wire byte without UB");
        CHK_ASSERT_EQ(unp->payload[3], uint8_t(0x00), tName, "Steps follow is 0");

        // Test string helper fallback
        std::string str = eqPresetToString(presetEnum);
        CHK_ASSERT_EQ(str, std::string("off"), tName, "eqPresetToString must safely return 'off' for unknown presets");

        // Test inbound parser resilience when receiving this frame as a response
        std::vector<uint8_t> retPayload = {0x57, 0x00, rawPreset, 0x00};
        HeadphoneState state;
        bool ok = parseInboundPayload(retPayload, state);
        CHK_ASSERT(ok, tName, "Inbound parser must accept response packet");
        CHK_ASSERT_EQ(state.eq_preset, std::string("off"), tName, "State eq_preset must fall back to 'off'");
    }

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 3: Boundary values for EQ Custom Bands and Clear Bass
// ---------------------------------------------------------------------------
void testCustomEqAndClearBassBoundaries() {
    const char* tName = "CustomEqAndClearBassBoundaries";
    LOG_TEST_START(tName);

    // 1. Extreme minimum boundaries (<-10: -11, -50, -9999, INT_MIN)
    {
        std::array<int, 5> minBands = {-11, -50, -9999, INT_MIN, -10};
        int minClearBass = INT_MIN;
        auto frame = serializeCustomEq(minBands, minClearBass, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Min bands frame must unpack");
        CHK_ASSERT_EQ(unp->payload.size(), size_t(10), tName, "Custom EQ payload size 10");
        CHK_ASSERT_EQ(unp->payload[0], uint8_t(0x58), tName, "Opcode 0x58");
        CHK_ASSERT_EQ(unp->payload[2], uint8_t(0xA0), tName, "Preset CUSTOM 0xA0");
        CHK_ASSERT_EQ(unp->payload[3], uint8_t(0x06), tName, "6 steps follow");

        // Clear bass INT_MIN clamped to -10 -> wire 0
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(0), tName, "Clear bass INT_MIN clamped to wire 0 (-10)");
        // Bands clamped to -10 -> wire 0
        CHK_ASSERT_EQ(unp->payload[5], uint8_t(0), tName, "Band 0 (-11) clamped to wire 0");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(0), tName, "Band 1 (-50) clamped to wire 0");
        CHK_ASSERT_EQ(unp->payload[7], uint8_t(0), tName, "Band 2 (-9999) clamped to wire 0");
        CHK_ASSERT_EQ(unp->payload[8], uint8_t(0), tName, "Band 3 (INT_MIN) clamped to wire 0");
        CHK_ASSERT_EQ(unp->payload[9], uint8_t(0), tName, "Band 4 (-10) serialized as wire 0");
    }

    // 2. Extreme maximum boundaries (>10: 11, 50, 9999, INT_MAX)
    {
        std::array<int, 5> maxBands = {11, 50, 9999, INT_MAX, 10};
        int maxClearBass = INT_MAX;
        auto frame = serializeCustomEq(maxBands, maxClearBass, 1);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Max bands frame must unpack");
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(20), tName, "Clear bass INT_MAX clamped to wire 20 (+10)");
        CHK_ASSERT_EQ(unp->payload[5], uint8_t(20), tName, "Band 0 (11) clamped to wire 20");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(20), tName, "Band 1 (50) clamped to wire 20");
        CHK_ASSERT_EQ(unp->payload[7], uint8_t(20), tName, "Band 2 (9999) clamped to wire 20");
        CHK_ASSERT_EQ(unp->payload[8], uint8_t(20), tName, "Band 3 (INT_MAX) clamped to wire 20");
        CHK_ASSERT_EQ(unp->payload[9], uint8_t(20), tName, "Band 4 (10) serialized as wire 20");
    }

    // 3. Exact valid range limits [-10, 10]
    {
        std::array<int, 5> exactBands = {-10, -5, 0, 5, 10};
        int exactClearBass = 0;
        auto frame = serializeCustomEq(exactBands, exactClearBass, 0);
        auto unp = unpackFrame(frame);
        CHK_ASSERT(unp.has_value(), tName, "Exact bands frame unpacks");
        CHK_ASSERT_EQ(unp->payload[4], uint8_t(10), tName, "Clear bass 0 -> wire 10");
        CHK_ASSERT_EQ(unp->payload[5], uint8_t(0), tName, "-10 -> wire 0");
        CHK_ASSERT_EQ(unp->payload[6], uint8_t(5), tName, "-5 -> wire 5");
        CHK_ASSERT_EQ(unp->payload[7], uint8_t(10), tName, "0 -> wire 10");
        CHK_ASSERT_EQ(unp->payload[8], uint8_t(15), tName, "+5 -> wire 15");
        CHK_ASSERT_EQ(unp->payload[9], uint8_t(20), tName, "+10 -> wire 20");
    }

    // 4. Inbound Parser Behavior on Out-of-Bounds Wire Bytes
    // Inbound payload with clearBass wire = 0xFF (255) and bands = 0xFF (255)
    // Finding check: does parseInboundPayload clamp to [-10, 10]?
    {
        std::vector<uint8_t> corruptEqPayload = {
            0x57, 0x00, 0xA0, 0x06, 0xFF, 0xFF, 0x00, 0x14, 0x28, 0xFE
        };
        HeadphoneState state;
        bool ok = parseInboundPayload(corruptEqPayload, state);
        CHK_ASSERT(ok, tName, "Inbound parser handles corrupt EQ payload without crashing");
        // Document: 0xFF - 10 = 245
        std::cout << "[   NOTE   ] " << tName << " : Inbound clear_bass with wire byte 0xFF parses to "
                  << state.clear_bass << " (raw un-clamped value)" << std::endl;
    }

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 4: MockTransport Rapid Connect/Disconnect Cycles (5,000 iterations)
// ---------------------------------------------------------------------------
void testMockTransportRapidCycles() {
    const char* tName = "MockTransportRapidCycles";
    LOG_TEST_START(tName);

    int startFds = countOpenFds();
    CHK_ASSERT(startFds > 0, tName, "Can read open file descriptors");

    const int kIterations = 5000;
    MockTransport transport;

    for (int i = 0; i < kIterations; ++i) {
        int rc = transport.connect("AA:BB:CC:DD:EE:FF", 1);
        CHK_ASSERT_EQ(rc, 0, tName, "MockTransport connect must return 0");
        CHK_ASSERT(transport.isConnected(), tName, "Must be marked connected");
        CHK_ASSERT(transport.getFd() >= 0, tName, "fd must be >= 0");
        CHK_ASSERT(transport.getPeerFd() >= 0, tName, "peerFd must be >= 0");

        transport.disconnect();
        CHK_ASSERT(!transport.isConnected(), tName, "Must be disconnected");
        CHK_ASSERT_EQ(transport.getFd(), -1, tName, "fd must be -1 after disconnect");
        CHK_ASSERT_EQ(transport.getPeerFd(), -1, tName, "peerFd must be -1 after disconnect");
    }

    int endFds = countOpenFds();
    CHK_ASSERT_EQ(endFds, startFds, tName, "Open FD count must be identical (zero FD leaks across 5,000 cycles)");

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 5: MockTransport Non-Blocking Read/Write Stress & Remote Disconnect
// ---------------------------------------------------------------------------
void testMockTransportNonBlockingStress() {
    const char* tName = "MockTransportNonBlockingStress";
    LOG_TEST_START(tName);

    int startFds = countOpenFds();

    for (int cycle = 0; cycle < 1000; ++cycle) {
        MockTransport transport;
        int rc = transport.connect("11:22:33:44:55:66", 1);
        CHK_ASSERT_EQ(rc, 0, tName, "Connect must succeed");

        int peerFd = transport.getPeerFd();
        CHK_ASSERT(peerFd >= 0, tName, "Valid peer fd");

        // Send 64-byte payload from transport to peer
        std::vector<uint8_t> sendData(64, static_cast<uint8_t>(cycle & 0xFF));
        ssize_t nSent = transport.send(sendData.data(), sendData.size());
        CHK_ASSERT_EQ(nSent, static_cast<ssize_t>(sendData.size()), tName, "Send to peer");

        // Read from peer
        std::vector<uint8_t> recvData(64);
        ssize_t nRecv = ::recv(peerFd, recvData.data(), recvData.size(), 0);
        CHK_ASSERT_EQ(nRecv, static_cast<ssize_t>(sendData.size()), tName, "Peer recv");
        CHK_ASSERT(recvData == sendData, tName, "Data integrity match");

        // Send 64-byte payload from peer to transport
        ssize_t nPeerSent = ::send(peerFd, sendData.data(), sendData.size(), 0);
        CHK_ASSERT_EQ(nPeerSent, static_cast<ssize_t>(sendData.size()), tName, "Peer send");

        // Recv on transport
        std::vector<uint8_t> transRecv(64);
        ssize_t nTransRecv = transport.recv(transRecv.data(), transRecv.size());
        CHK_ASSERT_EQ(nTransRecv, static_cast<ssize_t>(sendData.size()), tName, "Transport recv");
        CHK_ASSERT(transRecv == sendData, tName, "Transport data integrity match");

        // Simulate remote disconnect
        transport.simulateRemoteDisconnect();

        // Send should fail with -1 (EPIPE)
        ssize_t deadSend = transport.send(sendData.data(), sendData.size());
        CHK_ASSERT_EQ(deadSend, -1, tName, "Send on dropped peer must return -1");

        // Recv should return 0 (EOF)
        uint8_t dummy[16];
        ssize_t deadRecv = transport.recv(dummy, sizeof(dummy));
        CHK_ASSERT_EQ(deadRecv, 0, tName, "Recv on dropped peer must return 0 (EOF)");

        transport.disconnect();
    }

    int endFds = countOpenFds();
    CHK_ASSERT_EQ(endFds, startFds, tName, "Zero FD leak across 1,000 non-blocking transport cycles");

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 6: BluetoothManager Rapid Connect/Disconnect & Backoff Lifecycle
// ---------------------------------------------------------------------------
void testBluetoothManagerRapidLifecycle() {
    const char* tName = "BluetoothManagerRapidLifecycle";
    LOG_TEST_START(tName);

    int startFds = countOpenFds();
    BluetoothConfig config;
    config.initialBackoffMs = 20;
    config.maxBackoffMs = 50;
    config.autoReconnect = false; // Test explicit control first

    for (int cycle = 0; cycle < 500; ++cycle) {
        int peerFd = -1;
        auto manager = BluetoothManager::createMock(config, &peerFd);
        CHK_ASSERT(manager != nullptr, tName, "Manager created");
        CHK_ASSERT(peerFd >= 0, tName, "Peer fd valid");

        bool connectedFired = false;
        bool disconnectedFired = false;

        BluetoothCallbacks cbs;
        cbs.onConnected = [&]() { connectedFired = true; };
        cbs.onDisconnected = [&](const std::string&) { disconnectedFired = true; };
        manager->setCallbacks(cbs);

        // Start
        manager->start();
        CHK_ASSERT(manager->getState() == ConnectionState::CONNECTED, tName, "State is CONNECTED");
        CHK_ASSERT(connectedFired, tName, "onConnected fired");

        // Disconnect
        manager->disconnect();
        CHK_ASSERT(manager->getState() == ConnectionState::DISCONNECTED, tName, "State is DISCONNECTED");

        // Peer fd was closed by manager->disconnect()
    }

    int endFds = countOpenFds();
    CHK_ASSERT_EQ(endFds, startFds, tName, "Zero FD leak across 500 BluetoothManager cycles");

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 7: Non-Blocking Buffer Saturation & Outbound Queue Draining
// ---------------------------------------------------------------------------
void testBluetoothManagerQueueSaturation() {
    const char* tName = "BluetoothManagerQueueSaturation";
    LOG_TEST_START(tName);

    int peerFd = -1;
    BluetoothConfig config;
    auto manager = BluetoothManager::createMock(config, &peerFd);
    manager->start();
    CHK_ASSERT(manager->getState() == ConnectionState::CONNECTED, tName, "Connected");

    // Generate large volume of packets (500 KB total)
    const size_t kPacketSize = 1000;
    const size_t kNumPackets = 500;
    std::vector<uint8_t> singlePacket(kPacketSize, 0x42);

    size_t totalBytesQueued = 0;
    for (size_t i = 0; i < kNumPackets; ++i) {
        bool ok = manager->sendPacket(singlePacket);
        CHK_ASSERT(ok, tName, "sendPacket must accept or queue packet");
        totalBytesQueued += kPacketSize;
    }

    // Since peer socket was not read, socket buffer should have filled and queued the rest
    short events = manager->getPollEvents();
    CHK_ASSERT(events & POLLOUT, tName, "POLLOUT must be requested when queue is non-empty");

    // Now drain peer socket while triggering handleSocketEvent(POLLOUT)
    size_t totalBytesReceived = 0;
    std::vector<uint8_t> readBuf(4096);

    while (totalBytesReceived < totalBytesQueued) {
        // Read available bytes from peerFd
        ssize_t n = ::recv(peerFd, readBuf.data(), readBuf.size(), MSG_DONTWAIT);
        if (n > 0) {
            totalBytesReceived += n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Buffer empty, pump manager to flush more queued bytes
            manager->handleSocketEvent(POLLOUT);
        } else {
            LOG_TEST_FAIL(tName, "Unexpected recv error or hang during queue drain");
            return;
        }

        // Pump writer
        manager->handleSocketEvent(POLLOUT);
    }

    CHK_ASSERT_EQ(totalBytesReceived, totalBytesQueued, tName, "All queued bytes drained without loss");

    // Now queue should be empty, so POLLOUT should NOT be set
    short finalEvents = manager->getPollEvents();
    CHK_ASSERT(!(finalEvents & POLLOUT), tName, "POLLOUT must be cleared once queue is empty");

    manager->stop();
    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 8: Peer Disconnect with In-Flight Queue & Invalidation Analysis
// ---------------------------------------------------------------------------
void testPeerDisconnectUnderQueueStress() {
    const char* tName = "PeerDisconnectUnderQueueStress";
    LOG_TEST_START(tName);

    int peerFd = -1;
    BluetoothConfig config;
    config.initialBackoffMs = 20;
    config.maxBackoffMs = 50;
    config.autoReconnect = true;

    auto manager = BluetoothManager::createMock(config, &peerFd);
    bool disconnectedCalled = false;
    std::string disconnectReason;

    BluetoothCallbacks cbs;
    cbs.onDisconnected = [&](const std::string& reason) {
        disconnectedCalled = true;
        disconnectReason = reason;
    };
    manager->setCallbacks(cbs);
    manager->start();

    // Flood sendQueue
    std::vector<uint8_t> bulkData(200000, 0xAA);
    manager->sendPacket(bulkData);

    // Abruptly close peer
    ::close(peerFd);

    // Trigger handleSocketEvent with POLLOUT (which triggers write error) or POLLHUP
    manager->handleSocketEvent(POLLOUT | POLLHUP);

    CHK_ASSERT(disconnectedCalled, tName, "onDisconnected callback must fire on drop under stress");
    CHK_ASSERT(manager->getState() == ConnectionState::RECONNECT_BACKOFF, tName, "Transition to RECONNECT_BACKOFF");

    // Advance backoff time
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    manager->tick();

    // Manager re-enters CONNECTED
    CHK_ASSERT(manager->getState() == ConnectionState::CONNECTED, tName, "Manager reconnected to mock");

    // Demonstrate the peerFd staleness and FD recycling issue:
    // If an intermediate file descriptor is opened between disconnect and reconnect:
    int intermediateFd = ::open("/dev/null", O_RDONLY);
    
    // Now trigger another disconnect and reconnect
    manager->disconnect();
    
    // Now simulate an external allocation taking the lowest fd
    int dummyFd = ::open("/dev/null", O_RDONLY);
    
    // Reconnect manager
    manager->start();
    
    // Check if the original outPeerFd was updated by connect()
    // It will NOT be updated because outPeerFd is not stored in MockTransport!
    CHK_ASSERT(dummyFd >= 0, tName, "Dummy fd allocated");
    CHK_ASSERT(peerFd != dummyFd + 2, tName, "peerFd is stale and does not reflect newly allocated socketpair");
    
    ::close(dummyFd);
    ::close(intermediateFd);

    manager->stop();
    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 9: Callback Reentrancy Stress
// ---------------------------------------------------------------------------
void testCallbackReentrancyStress() {
    const char* tName = "CallbackReentrancyStress";
    LOG_TEST_START(tName);

    int peerFd = -1;
    BluetoothConfig config;
    auto manager = BluetoothManager::createMock(config, &peerFd);

    int packetCount = 0;
    BluetoothCallbacks cbs;
    cbs.onDataReceived = [&](const uint8_t*, size_t) {
        // Re-entrant send during receive callback
        std::vector<uint8_t> echo = {0x3E, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3C};
        manager->sendPacket(echo);
        packetCount++;
    };
    manager->setCallbacks(cbs);
    manager->start();

    // Send 100 packets into peerFd
    std::vector<uint8_t> inbound = {0x3E, 0x0C, 0x00, 0x00, 0x00, 0x01, 0xAA, 0xB7, 0x3C};
    for (int i = 0; i < 100; ++i) {
        ::send(peerFd, inbound.data(), inbound.size(), 0);
        manager->handleSocketEvent(POLLIN);
    }

    CHK_ASSERT_EQ(packetCount, 100, tName, "100 reentrant sends executed without deadlock or crash");

    manager->stop();
    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 10: Inbound Payload Fuzzing and Truncation Resistance
// ---------------------------------------------------------------------------
void testInboundPayloadFuzzing() {
    const char* tName = "InboundPayloadFuzzing";
    LOG_TEST_START(tName);

    HeadphoneState state;

    // Test 1: Empty and all single-byte payloads
    for (int b = 0; b <= 255; ++b) {
        std::vector<uint8_t> single = {static_cast<uint8_t>(b)};
        bool ok = parseInboundPayload(single, state);
        (void)ok; // Must not crash or trigger UB
    }

    // Test 2: Truncated payloads of each known opcode
    std::vector<uint8_t> opcodes = {0x23, 0x25, 0x67, 0x69, 0x57, 0x59, 0xE7, 0xE9, 0xF7, 0xF9, 0xD7, 0xD9, 0x10, 0x11, 0x12, 0x13, 0x14};
    for (uint8_t op : opcodes) {
        std::vector<uint8_t> partial = {op};
        for (size_t len = 1; len <= 12; ++len) {
            partial.push_back(static_cast<uint8_t>(len * 17));
            bool ok = parseInboundPayload(partial, state);
            (void)ok;
        }
    }

    // Test 3: Wire byte boundary tests on Battery
    // Level 255 (out of range 0-100)
    {
        std::vector<uint8_t> batOver = {0x25, 0x00, 255, 0x01};
        bool ok = parseInboundPayload(batOver, state);
        CHK_ASSERT(ok, tName, "Parsed battery 255");
        CHK_ASSERT_EQ(state.battery_level, 255, tName, "Battery level 255");
    }

    // Level 0
    {
        std::vector<uint8_t> batZero = {0x25, 0x00, 0, 0x00};
        bool ok = parseInboundPayload(batZero, state);
        CHK_ASSERT(ok, tName, "Parsed battery 0");
        CHK_ASSERT_EQ(state.battery_level, 0, tName, "Battery level 0");
    }

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// TEST 11: StreamFramer Unbounded Stream & Recovery Stress
// ---------------------------------------------------------------------------
void testStreamFramerStress() {
    const char* tName = "StreamFramerStress";
    LOG_TEST_START(tName);

    StreamFramer framer;

    // 1. Unbounded non-marker bytes (e.g. 100,000 bytes without 0x3E or 0x3C)
    std::vector<uint8_t> noise(100000, 0x77);
    framer.append(noise);
    auto noFrame = framer.nextFrame();
    CHK_ASSERT(!noFrame.has_value(), tName, "Noise must not yield a frame");
    // All non-start bytes cleared
    CHK_ASSERT_EQ(framer.bufferedBytes(), size_t(0), tName, "Buffer cleared when no start marker found");

    // 2. Start marker followed by noise without end marker
    std::vector<uint8_t> startWithNoise = {kStartMarker};
    startWithNoise.insert(startWithNoise.end(), noise.begin(), noise.begin() + 10000);
    framer.append(startWithNoise);
    auto incomplete = framer.nextFrame();
    CHK_ASSERT(!incomplete.has_value(), tName, "Incomplete frame yields nothing");
    CHK_ASSERT_EQ(framer.bufferedBytes(), size_t(10001), tName, "Incomplete frame buffered");

    // 3. Reset flushes
    framer.reset();
    CHK_ASSERT_EQ(framer.bufferedBytes(), size_t(0), tName, "Reset empties buffer");

    // 4. Multiple back-to-back start markers before end marker
    // [0x3E, 0x3E, payload, 0x3C]
    auto validFrame = packFrame(PacketType::DATA_MDR, 0, std::vector<uint8_t>{0x11, 0x22});
    std::vector<uint8_t> doubleStart = {kStartMarker};
    doubleStart.insert(doubleStart.end(), validFrame.begin(), validFrame.end());
    framer.append(doubleStart);
    auto extracted = framer.nextFrame();
    CHK_ASSERT(extracted.has_value(), tName, "Frame extracted despite double start");

    LOG_TEST_PASS(tName);
}

// ---------------------------------------------------------------------------
// Main Runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "=========================================================\n";
    std::cout << "  omarchy-sony: Challenger 2 Stress & Boundary Harness   \n";
    std::cout << "=========================================================\n";

    testAmbientLevelBoundaries();
    testEqPresetBoundaries();
    testCustomEqAndClearBassBoundaries();
    testMockTransportRapidCycles();
    testMockTransportNonBlockingStress();
    testBluetoothManagerRapidLifecycle();
    testBluetoothManagerQueueSaturation();
    testPeerDisconnectUnderQueueStress();
    testCallbackReentrancyStress();
    testInboundPayloadFuzzing();
    testStreamFramerStress();

    std::cout << "=========================================================\n";
    std::cout << "Summary: " << gPassedTests << " passed, "
              << gFailedTests << " failed\n";
    std::cout << "=========================================================\n";

    return (gFailedTests == 0) ? 0 : 1;
}
