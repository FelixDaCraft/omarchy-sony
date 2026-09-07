#include "MDRProtocol.hpp"
#include <algorithm>
#include <sstream>
#include <chrono>

namespace omarchy::sony::protocol {

// ---------------------------------------------------------------------------
// HeadphoneState Implementation
// ---------------------------------------------------------------------------

std::string HeadphoneState::toJson() const {
    if (!connected) {
        return "{\"schema_version\":1,\"connected\":false}";
    }

    std::ostringstream ss;
    ss << "{"
       << "\"schema_version\":" << schema_version << ","
       << "\"connected\":true,"
       << "\"device_name\":\"" << device_name << "\","
       << "\"battery_level\":" << battery_level << ","
       << "\"charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"battery_charging\":" << (battery_charging ? "true" : "false") << ","
       << "\"noise_mode\":\"" << noise_mode << "\","
       << "\"ambient_level\":" << ambient_sound_level << ","
       << "\"ambient_sound_level\":" << ambient_sound_level << ","
       << "\"voice_passthrough\":" << (voice_passthrough ? "true" : "false") << ","
       << "\"eq_preset\":\"" << eq_preset << "\","
       << "\"eq_bands\":["
       << eq_custom_bands[0] << "," << eq_custom_bands[1] << "," << eq_custom_bands[2] << ","
       << eq_custom_bands[3] << "," << eq_custom_bands[4] << "],"
       << "\"eq_custom_bands\":["
       << eq_custom_bands[0] << "," << eq_custom_bands[1] << "," << eq_custom_bands[2] << ","
       << eq_custom_bands[3] << "," << eq_custom_bands[4] << "],"
       << "\"clear_bass\":" << clear_bass << ","
       << "\"speak_to_chat\":" << (speak_to_chat ? "true" : "false") << ","
       << "\"dsee\":" << (dsee_extreme ? "true" : "false") << ","
       << "\"dsee_extreme\":" << (dsee_extreme ? "true" : "false") << ","
       << "\"multipoint\":" << (multipoint ? "true" : "false") << ","
       << "\"ear_detection\":" << (ear_detection ? "true" : "false") << ","
       << "\"codec\":\"" << codec << "\","
       << "\"last_updated\":" << last_updated
       << "}";
    return ss.str();
}

HeadphoneState HeadphoneState::makeDisconnected() {
    HeadphoneState s;
    s.schema_version = 1;
    s.connected = false;
    return s;
}

// ---------------------------------------------------------------------------
// Low-Level Framing, Escaping & Checksums
// ---------------------------------------------------------------------------

uint8_t calculateChecksum(std::span<const uint8_t> data) noexcept {
    uint8_t sum = 0;
    for (uint8_t b : data) {
        sum += b;
    }
    return sum;
}

std::vector<uint8_t> escapeBytes(std::span<const uint8_t> unescaped) {
    std::vector<uint8_t> out;
    out.reserve(unescaped.size() * 2);
    for (uint8_t b : unescaped) {
        switch (b) {
            case kEndMarker:    out.push_back(kEscapeSentry); out.push_back(kEscaped3C); break;
            case kEscapeSentry: out.push_back(kEscapeSentry); out.push_back(kEscaped3D); break;
            case kStartMarker:  out.push_back(kEscapeSentry); out.push_back(kEscaped3E); break;
            default:            out.push_back(b); break;
        }
    }
    return out;
}

std::vector<uint8_t> unescapeBytes(std::span<const uint8_t> escaped) {
    std::vector<uint8_t> out;
    out.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); ++i) {
        uint8_t b = escaped[i];
        if (b == kEscapeSentry) {
            if (i + 1 >= escaped.size()) return {}; // Incomplete escape at EOF
            uint8_t next = escaped[++i];
            switch (next) {
                case kEscaped3C: out.push_back(kEndMarker); break;
                case kEscaped3D: out.push_back(kEscapeSentry); break;
                case kEscaped3E: out.push_back(kStartMarker); break;
                default: return {}; // Invalid escape sequence
            }
        } else {
            out.push_back(b);
        }
    }
    return out;
}

std::vector<uint8_t> packFrame(PacketType type, uint8_t seq, std::span<const uint8_t> payload) {
    std::vector<uint8_t> unescaped;
    unescaped.reserve(6 + payload.size() + 1);

    unescaped.push_back(static_cast<uint8_t>(type));
    unescaped.push_back(seq);

    // 4-byte Big-Endian Length
    uint32_t len = static_cast<uint32_t>(payload.size());
    unescaped.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    unescaped.push_back(static_cast<uint8_t>(len & 0xFF));

    // Payload
    unescaped.insert(unescaped.end(), payload.begin(), payload.end());

    // 8-bit additive Checksum (modulo 256 sum over all unescaped bytes before checksum)
    uint8_t csum = calculateChecksum(unescaped);
    unescaped.push_back(csum);

    // Escape and encapsulate with start/end markers
    std::vector<uint8_t> frame;
    frame.reserve(unescaped.size() * 2 + 2);
    frame.push_back(kStartMarker);
    std::vector<uint8_t> escaped = escapeBytes(unescaped);
    frame.insert(frame.end(), escaped.begin(), escaped.end());
    frame.push_back(kEndMarker);

    return frame;
}

std::optional<UnpackedFrame> unpackFrame(std::span<const uint8_t> frameBytes) {
    if (frameBytes.size() < 9) return std::nullopt;
    if (frameBytes.front() != kStartMarker || frameBytes.back() != kEndMarker) return std::nullopt;

    // Strip markers
    std::span<const uint8_t> inner = frameBytes.subspan(1, frameBytes.size() - 2);
    std::vector<uint8_t> unescaped = unescapeBytes(inner);
    if (unescaped.size() < 7) return std::nullopt; // Type(1) + Seq(1) + Len(4) + Csum(1)

    // Verify Checksum
    uint8_t receivedCsum = unescaped.back();
    std::span<const uint8_t> checkSpan(unescaped.data(), unescaped.size() - 1);
    if (calculateChecksum(checkSpan) != receivedCsum) return std::nullopt;

    PacketType type = static_cast<PacketType>(unescaped[0]);
    uint8_t seq = unescaped[1];
    uint32_t len = (static_cast<uint32_t>(unescaped[2]) << 24) |
                   (static_cast<uint32_t>(unescaped[3]) << 16) |
                   (static_cast<uint32_t>(unescaped[4]) << 8)  |
                   static_cast<uint32_t>(unescaped[5]);

    if (unescaped.size() - 7 != len) return std::nullopt;

    std::vector<uint8_t> payload(unescaped.begin() + 6, unescaped.begin() + 6 + len);
    return UnpackedFrame{type, seq, std::move(payload)};
}

// ---------------------------------------------------------------------------
// StreamFramer Implementation
// ---------------------------------------------------------------------------

void StreamFramer::append(std::span<const uint8_t> incoming) {
    constexpr size_t kMaxBufferSize = 64 * 1024; // 64 KB
    if (buffer_.size() + incoming.size() > kMaxBufferSize) {
        buffer_.clear(); // Discard corrupted unclosed stream data to prevent unbounded growth
    }
    buffer_.insert(buffer_.end(), incoming.begin(), incoming.end());
}

std::optional<std::vector<uint8_t>> StreamFramer::nextFrame() {
    auto startIt = std::find(buffer_.begin(), buffer_.end(), kStartMarker);
    if (startIt == buffer_.end()) {
        buffer_.clear();
        return std::nullopt;
    }
    if (startIt != buffer_.begin()) {
        buffer_.erase(buffer_.begin(), startIt);
        startIt = buffer_.begin();
    }

    auto endIt = std::find(startIt + 1, buffer_.end(), kEndMarker);
    if (endIt == buffer_.end()) {
        return std::nullopt; // Incomplete frame
    }

    std::vector<uint8_t> frame(startIt, endIt + 1);
    buffer_.erase(buffer_.begin(), endIt + 1);
    return frame;
}

void StreamFramer::reset() {
    buffer_.clear();
}

// ---------------------------------------------------------------------------
// Command Serializers (Host -> XM5)
// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeACK(uint8_t rx_seq) {
    return packFrame(PacketType::ACK, static_cast<uint8_t>(1 - rx_seq), {});
}

std::vector<uint8_t> serializeNoiseMode(NoiseMode mode, uint8_t ambientLevel, bool voiceFocus, uint8_t seq) {
    uint8_t totalEffect = (mode == NoiseMode::OFF) ? 0x00 : 0x01;
    uint8_t ncMode = (mode == NoiseMode::AMBIENT || mode == NoiseMode::WIND) ? 0x01 : 0x00;
    uint8_t voice = (mode == NoiseMode::AMBIENT && voiceFocus) ? 0x01 : 0x00;
    uint8_t level = (mode == NoiseMode::AMBIENT) ? static_cast<uint8_t>(std::clamp(static_cast<int>(ambientLevel), 0, 20)) : 0x00;

    std::vector<uint8_t> payload = {
        0x68, // NCASM_SET_PARAM
        0x17, // MODE_NC_ASM_DUAL_NC_MODE_SWITCH_AND_ASM_SEAMLESS
        0x01, // CHANGED
        totalEffect,
        ncMode,
        voice,
        level
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeAmbientLevel(uint8_t level, bool voiceFocus, uint8_t seq) {
    NoiseMode mode = (level == 0) ? NoiseMode::WIND : NoiseMode::AMBIENT;
    return serializeNoiseMode(mode, level, voiceFocus, seq);
}

std::vector<uint8_t> serializeEqPreset(EqPreset preset, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0x58, // EQEBB_SET_PARAM
        0x00, // PRESET_EQ
        static_cast<uint8_t>(preset),
        0x00  // 0 band steps follow (preset selection only)
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeCustomEq(const std::array<int, 5>& bands, int clearBass, uint8_t seq) {
    auto clampVal = [](int v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(v, -10, 10) + 10);
    };

    std::vector<uint8_t> payload = {
        0x58, // EQEBB_SET_PARAM
        0x00, // PRESET_EQ
        static_cast<uint8_t>(EqPreset::CUSTOM), // 0xA0
        0x06, // 6 steps follow
        clampVal(clearBass),
        clampVal(bands[0]),
        clampVal(bands[1]),
        clampVal(bands[2]),
        clampVal(bands[3]),
        clampVal(bands[4])
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeSpeakToChat(bool enabled, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0xF8, // SYSTEM_SET_PARAM
        0x0C, // SMART_TALKING_MODE_TYPE2
        static_cast<uint8_t>(enabled ? 0x00 : 0x01), // ON = 0, OFF = 1
        0x01  // previewModeOnOffValue = OFF
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeDsee(bool enabled, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0xE8, // AUDIO_SET_PARAM
        0x01, // UPSCALING
        static_cast<uint8_t>(enabled ? 0x01 : 0x00) // AUTO/ON = 1, OFF = 0
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeMultipoint(bool enabled, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0xD8, // GENERAL_SETTING_SET_PARAM
        0xD1, // GENERAL_SETTING1
        0x00, // BOOLEAN_TYPE
        static_cast<uint8_t>(enabled ? 0x00 : 0x01) // ON = 0, OFF = 1
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeEarDetection(bool enabled, uint8_t seq) {
    std::vector<uint8_t> payload = {
        0xF8, // SYSTEM_SET_PARAM
        0x01, // PLAYBACK_CONTROL_BY_WEARING
        static_cast<uint8_t>(enabled ? 0x00 : 0x01) // ON = 0, OFF = 1
    };
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

// ---------------------------------------------------------------------------
// Query Serializers
// ---------------------------------------------------------------------------

std::vector<uint8_t> serializeQueryBattery(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x22, 0x00 }; // POWER_GET_STATUS, BATTERY
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryNoiseMode(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x66, 0x17 }; // NCASM_GET_PARAM, MODE_NC_ASM_...
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryEq(uint8_t seq) {
    std::vector<uint8_t> payload = { 0x56 }; // EQEBB_GET_PARAM
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryDsee(uint8_t seq) {
    std::vector<uint8_t> payload = { 0xE6, 0x01 }; // AUDIO_GET_PARAM, UPSCALING
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQuerySpeakToChat(uint8_t seq) {
    std::vector<uint8_t> payload = { 0xF6, 0x0C }; // SYSTEM_GET_PARAM, SMART_TALKING_MODE_TYPE2
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryEarDetection(uint8_t seq) {
    std::vector<uint8_t> payload = { 0xF6, 0x01 }; // SYSTEM_GET_PARAM, PLAYBACK_CONTROL_BY_WEARING
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

std::vector<uint8_t> serializeQueryGeneralSetting(uint8_t seq) {
    std::vector<uint8_t> payload = { 0xD6, 0xD1 }; // GENERAL_SETTING_GET_PARAM, GENERAL_SETTING1
    return packFrame(PacketType::DATA_MDR, seq, payload);
}

// ---------------------------------------------------------------------------
// Inbound State Deserializer (XM5 -> Host)
// ---------------------------------------------------------------------------

bool parseInboundPayload(std::span<const uint8_t> payload, HeadphoneState& state) {
    if (payload.empty()) return false;
    uint8_t cmd = payload[0];
    bool updated = false;

    // 1. Battery Status (POWER_RET_STATUS / POWER_NTFY_STATUS)
    if ((cmd == 0x23 || cmd == 0x25) && payload.size() >= 3) {
        uint8_t type = payload[1];
        if (type == 0x00 && payload.size() >= 4) { // BATTERY
            state.battery_level = payload[2];
            state.battery_charging = (payload[3] == 0x01);
            updated = true;
        } else if (type == 0x08 && payload.size() >= 5) { // BATTERY_WITH_THRESHOLD
            state.battery_level = payload[2];
            state.battery_charging = (payload[4] == 0x01);
            updated = true;
        }
    }
    // 2. Noise Control & Ambient Sound (NCASM_RET_PARAM / NCASM_NTFY_PARAM)
    else if ((cmd == 0x67 || cmd == 0x69) && payload.size() >= 7) {
        uint8_t type = payload[1];
        if (type == 0x17) {
            uint8_t totalEffect = payload[3];
            uint8_t ncMode = payload[4];
            uint8_t voiceFocus = payload[5];
            uint8_t level = payload[6];

            if (totalEffect == 0x00) {
                state.noise_mode = "off";
            } else if (ncMode == 0x00) {
                state.noise_mode = "anc";
                if (level > 0) {
                    state.ambient_sound_level = level;
                }
            } else if (ncMode == 0x01) {
                if (level == 0) {
                    state.noise_mode = "wind";
                } else {
                    state.noise_mode = "ambient";
                    state.ambient_sound_level = level;
                }
            }
            state.voice_passthrough = (voiceFocus == 0x01);
            updated = true;
        }
    }
    // 3. Equalizer (EQEBB_RET_PARAM / EQEBB_NTFY_PARAM)
    else if ((cmd == 0x57 || cmd == 0x59) && payload.size() >= 3) {
        uint8_t type = payload[1];
        if (type == 0x00) { // PRESET_EQ
            state.eq_preset = eqPresetToString(static_cast<EqPreset>(payload[2]));
            if (payload.size() >= 10 && payload[3] == 0x06) {
                state.clear_bass = static_cast<int>(payload[4]) - 10;
                for (size_t i = 0; i < 5; ++i) {
                    state.eq_custom_bands[i] = static_cast<int>(payload[5 + i]) - 10;
                }
            }
            updated = true;
        }
    }
    // 4. DSEE (AUDIO_RET_PARAM / AUDIO_NTFY_PARAM)
    else if ((cmd == 0xE7 || cmd == 0xE9) && payload.size() >= 3) {
        uint8_t type = payload[1];
        if (type == 0x01) { // UPSCALING
            state.dsee_extreme = (payload[2] == 0x01);
            updated = true;
        }
    }
    // 5. System (Speak-to-Chat / Ear Detection) (SYSTEM_RET_PARAM / SYSTEM_NTFY_PARAM)
    else if ((cmd == 0xF7 || cmd == 0xF9) && payload.size() >= 3) {
        uint8_t type = payload[1];
        if (type == 0x0C) { // SMART_TALKING_MODE_TYPE2 (Speak-to-Chat)
            state.speak_to_chat = (payload[2] == 0x00);
            updated = true;
        } else if (type == 0x01) { // PLAYBACK_CONTROL_BY_WEARING (Ear Detection)
            state.ear_detection = (payload[2] == 0x00);
            updated = true;
        }
    }
    // 6. General Setting (Multipoint) (GENERAL_SETTING_RET_PARAM / GENERAL_SETTING_NTNY_PARAM)
    else if ((cmd == 0xD7 || cmd == 0xD9) && payload.size() >= 4) {
        uint8_t type = payload[1];
        if (type == 0xD1) { // GENERAL_SETTING1
            state.multipoint = (payload[3] == 0x00);
            updated = true;
        }
    }
    // Alternative single-byte dispatch (e.g. from compact representations)
    else if (cmd == 0x10 && payload.size() >= 3) { // Battery: [0x10, level, charging]
        state.battery_level = payload[1];
        state.battery_charging = (payload[2] != 0);
        updated = true;
    } else if (cmd == 0x11 && payload.size() >= 4) { // NC/ASM: [0x11, mode, level, voiceFocus]
        state.noise_mode = noiseModeToString(static_cast<NoiseMode>(payload[1]));
        state.ambient_sound_level = payload[2];
        state.voice_passthrough = (payload[3] != 0);
        updated = true;
    } else if (cmd == 0x12 && payload.size() >= 2) { // EQ Preset: [0x12, preset]
        state.eq_preset = eqPresetToString(static_cast<EqPreset>(payload[1]));
        updated = true;
    } else if (cmd == 0x13 && payload.size() >= 7) { // Custom EQ: [0x13, b0..b4, clearBass]
        for (size_t i = 0; i < 5; ++i) {
            state.eq_custom_bands[i] = static_cast<int>(payload[1 + i]) - 10;
        }
        state.clear_bass = static_cast<int>(payload[6]) - 10;
        updated = true;
    } else if (cmd == 0x14 && payload.size() >= 5) { // Toggles: [0x14, s2c, dsee, multi, ear]
        state.speak_to_chat = (payload[1] != 0);
        state.dsee_extreme = (payload[2] != 0);
        state.multipoint = (payload[3] != 0);
        state.ear_detection = (payload[4] != 0);
        updated = true;
    }

    if (updated) {
        state.connected = true;
        state.last_updated = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    return updated;
}

// ---------------------------------------------------------------------------
// Enum <-> String Helpers
// ---------------------------------------------------------------------------

std::string eqPresetToString(EqPreset preset) {
    switch (preset) {
        case EqPreset::OFF:     return "off";
        case EqPreset::BRIGHT:  return "bright";
        case EqPreset::EXCITED: return "excited";
        case EqPreset::MELLOW:  return "mellow";
        case EqPreset::RELAXED: return "relaxed";
        case EqPreset::VOCAL:   return "vocal";
        case EqPreset::TREBLE:  return "treble";
        case EqPreset::BASS:    return "bass";
        case EqPreset::SPEECH:  return "speech";
        case EqPreset::CUSTOM:  return "custom";
        default:                return "off";
    }
}

EqPreset stringToEqPreset(const std::string& str) {
    if (str == "off")     return EqPreset::OFF;
    if (str == "bright")  return EqPreset::BRIGHT;
    if (str == "excited") return EqPreset::EXCITED;
    if (str == "mellow")  return EqPreset::MELLOW;
    if (str == "relaxed") return EqPreset::RELAXED;
    if (str == "vocal")   return EqPreset::VOCAL;
    if (str == "treble")  return EqPreset::TREBLE;
    if (str == "bass")    return EqPreset::BASS;
    if (str == "speech")  return EqPreset::SPEECH;
    if (str == "custom")  return EqPreset::CUSTOM;
    return EqPreset::OFF;
}

std::string noiseModeToString(NoiseMode mode) {
    switch (mode) {
        case NoiseMode::OFF:     return "off";
        case NoiseMode::ANC:     return "anc";
        case NoiseMode::AMBIENT: return "ambient";
        case NoiseMode::WIND:    return "wind";
        default:                 return "off";
    }
}

NoiseMode stringToNoiseMode(const std::string& str) {
    if (str == "anc")     return NoiseMode::ANC;
    if (str == "ambient") return NoiseMode::AMBIENT;
    if (str == "wind")    return NoiseMode::WIND;
    if (str == "off")     return NoiseMode::OFF;
    return NoiseMode::ANC;
}

} // namespace omarchy::sony::protocol
