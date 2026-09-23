#ifndef TELEMETRY_PROTOCOL_HPP
#define TELEMETRY_PROTOCOL_HPP

#include <cstdint>

constexpr uint8_t AGISX_PROTOCOL_VERSION = 1;

#pragma pack(push, 1)

struct Header {
    uint8_t magic;         // 0xA6
    uint8_t version;       // AGISX_PROTOCOL_VERSION
    uint8_t packet_type;   // 1: Measurement, 2: Track State
    uint64_t timestamp_us;
    uint32_t sequence;
};

struct RawRadarMeasurement {
    Header header;
    double range;          // double precision matching generator
    double azimuth;        // double precision matching generator
    double elevation;      // double precision matching generator
    double doppler_vel;    // m/s
    double snr;            // dB
};

struct ProcessedTrackState {
    Header header;
    uint32_t track_id;
    double x;   // Position X (meters)
    double y;   // Position Y (meters)
    double z;   // Position Z (meters)
    double vx;  // Velocity X (m/s)
    double vy;  // Velocity Y (m/s)
    double vz;  // Velocity Z (m/s)
    double ax;  // Acceleration X (m/s^2)
    double ay;  // Acceleration Y (m/s^2)
    double az;  // Acceleration Z (m/s^2)
};

#pragma pack(pop)

#endif // TELEMETRY_PROTOCOL_HPP