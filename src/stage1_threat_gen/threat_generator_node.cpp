#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <random>
#include <cmath>
#include <map>
#include <cstring>
#include <zmq.hpp>
#include "telemetry_protocol.hpp"

// Radar Sensor Platform Location (Defended Base Origin)
constexpr double RADAR_ORIGIN_X = 0.0;
constexpr double RADAR_ORIGIN_Y = 0.0;
constexpr double RADAR_ORIGIN_Z = 0.0;

// 1-Sigma Measurement Noise for Primary Surveillance Radar
constexpr double NOISE_SIGMA_RANGE = 15.0;       // meters
constexpr double NOISE_SIGMA_AZIMUTH = 0.002;     // ~0.11 degrees (radians)
constexpr double NOISE_SIGMA_ELEVATION = 0.002;   // ~0.11 degrees (radians)

struct TrajectoryPoint {
    double time_s;
    double x, y, z;
    double vx, vy, vz;
    double ax, ay, az;
};

std::vector<TrajectoryPoint> load_trajectory_csv(const std::string& filepath) {
    std::vector<TrajectoryPoint> trajectory;
    std::ifstream file(filepath);
    
    if (!file.is_open()) {
        std::cerr << "[Stage 1 Error] Could not open dataset file: " << filepath << std::endl;
        return trajectory;
    }

    std::string line;
    std::getline(file, line); // Skip CSV header

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string val;
        TrajectoryPoint pt{};
        
        std::getline(ss, val, ','); pt.time_s = std::stod(val);
        std::getline(ss, val, ','); pt.x      = std::stod(val);
        std::getline(ss, val, ','); pt.y      = std::stod(val);
        std::getline(ss, val, ','); pt.z      = std::stod(val);
        std::getline(ss, val, ','); pt.vx     = std::stod(val);
        std::getline(ss, val, ','); pt.vy     = std::stod(val);
        std::getline(ss, val, ','); pt.vz     = std::stod(val);
        std::getline(ss, val, ','); pt.ax     = std::stod(val);
        std::getline(ss, val, ','); pt.ay     = std::stod(val);
        std::getline(ss, val, ','); pt.az     = std::stod(val);

        trajectory.push_back(pt);
    }

    std::cout << "[Stage 1: Threat Gen] Ingested " << trajectory.size() 
              << " frames from: " << filepath << std::endl;
    return trajectory;
}

int main() {
    std::cout << "[Stage 1: Threat Gen] Initializing Multi-Target Radar Feeder..." << std::endl;

    auto track_alpha   = load_trajectory_csv("data/trajectories/profile_alpha_ballistic.csv");
    auto track_bravo   = load_trajectory_csv("data/trajectories/profile_bravo_cruise.csv");
    auto track_charlie = load_trajectory_csv("data/trajectories/profile_charlie_hypersonic.csv");

    if (track_alpha.empty() || track_bravo.empty() || track_charlie.empty()) {
        std::cerr << "[Stage 1 Error] Dataset missing. Run 'python3 data/generate_dataset.py' first." << std::endl;
        return 1;
    }

    zmq::context_t ctx(1);
    
    // Publisher for Raw Radar Echoes
    zmq::socket_t pub(ctx, zmq::socket_type::pub);
    pub.bind("tcp://127.0.0.1:5554");

    // Subscriber for Kill Notifications from Stage 4 Interceptor Guidance
    zmq::socket_t sub_kills(ctx, zmq::socket_type::sub);
    sub_kills.set(zmq::sockopt::subscribe, "");
    sub_kills.set(zmq::sockopt::rcvhwm, 10000);
    sub_kills.connect("tcp://127.0.0.1:5556");

    std::map<uint32_t, bool> destroyed_profiles;

    std::cout << "[Stage 1: Threat Gen] Streaming Multi-Threat Radar Echoes on Port 5554 (100 Hz)..." << std::endl;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<double> noise_range(0.0, NOISE_SIGMA_RANGE);
    std::normal_distribution<double> noise_azimuth(0.0, NOISE_SIGMA_AZIMUTH);
    std::normal_distribution<double> noise_elevation(0.0, NOISE_SIGMA_ELEVATION);

    size_t idx_alpha = 0;
    size_t idx_bravo = 0;
    size_t idx_charlie = 0;
    uint32_t global_seq = 0;

    auto next_tick = std::chrono::steady_clock::now();

    while (true) {
        // 1. Process Kill Notifications from Stage 4
        zmq::message_t kill_msg;
        if (sub_kills.recv(kill_msg, zmq::recv_flags::dontwait)) {
            if (kill_msg.size() >= sizeof(ProcessedTrackState)) {
                ProcessedTrackState kill_evt;
                std::memcpy(&kill_evt, kill_msg.data(), sizeof(ProcessedTrackState));
                if (kill_evt.header.packet_type == 4) {
                    destroyed_profiles[kill_evt.track_id] = true;
                    std::cout << "\n[Stage 1: Threat Gen] Threat Track #" << kill_evt.track_id 
                              << " Radar Echoes TERMINATED (Hard Kinetic Kill)." << std::endl;
                }
            }
        }

        auto now = std::chrono::system_clock::now();
        uint64_t timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();

        auto emit_radar_blip = [&](const TrajectoryPoint& pt) {
            double dx = pt.x - RADAR_ORIGIN_X;
            double dy = pt.y - RADAR_ORIGIN_Y;
            double dz = pt.z - RADAR_ORIGIN_Z;

            double true_range = std::sqrt(dx * dx + dy * dy + dz * dz);
            double true_azimuth = std::atan2(dy, dx);
            double true_elevation = std::asin(dz / (true_range + 1e-6));

            double meas_range = true_range + noise_range(gen);
            double meas_azimuth = true_azimuth + noise_azimuth(gen);
            double meas_elevation = true_elevation + noise_elevation(gen);

            RawRadarMeasurement blip{};
            blip.header.magic = 0xA6;
            blip.header.version = AGISX_PROTOCOL_VERSION;
            blip.header.packet_type = 1; // Raw measurement
            blip.header.timestamp_us = timestamp_us;
            blip.header.sequence = ++global_seq;
            blip.range = meas_range;
            blip.azimuth = meas_azimuth;
            blip.elevation = meas_elevation;
            blip.doppler_vel = 0.0;
            blip.snr = 20.0;

            zmq::message_t msg(&blip, sizeof(RawRadarMeasurement));
            pub.send(msg, zmq::send_flags::none);
        };

        // Emit blips only if target profile hasn't been intercepted
        if (!destroyed_profiles[1] && idx_alpha < track_alpha.size()) {
            emit_radar_blip(track_alpha[idx_alpha++]);
        }
        if (!destroyed_profiles[2] && idx_bravo < track_bravo.size()) {
            emit_radar_blip(track_bravo[idx_bravo++]);
        }
        if (!destroyed_profiles[3] && idx_charlie < track_charlie.size()) {
            emit_radar_blip(track_charlie[idx_charlie++]);
        }

        // Reset stream logic if all remaining trajectories end or scenario completes
        if (idx_alpha >= track_alpha.size() && 
            idx_bravo >= track_bravo.size() && 
            idx_charlie >= track_charlie.size()) {
            
            std::cout << "\n[Stage 1: Threat Gen] Trajectory pass complete. Resetting active streams..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            idx_alpha = 0;
            idx_bravo = 0;
            idx_charlie = 0;
            destroyed_profiles.clear(); // Reset kill flags for new scenario run
        }

        if (global_seq % 300 == 0) {
            std::cout << "[Stage 1 @ 100Hz] Emitted " << global_seq 
                      << " total radar returns across active threat vectors." << std::endl;
        }

        next_tick += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next_tick);
    }

    return 0;
}