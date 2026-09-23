#include <iostream>
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <Eigen/Dense>
#include <zmq.hpp>
#include "telemetry_protocol.hpp"

constexpr double BASE_X = 0.0;
constexpr double BASE_Y = 0.0;
constexpr double BASE_Z = 0.0;

struct ThreatEvaluation {
    uint32_t track_id;
    double range_to_base;
    double closing_speed;
    double time_to_impact;
    double threat_score;
};

int main() {
    std::cout << "[Stage 3: C2/WTA Brain] Initializing Threat Prioritization Engine..." << std::endl;

    zmq::context_t ctx(1);

    // Subscriber to Stage 2 Track Stream
    zmq::socket_t sub_tracks(ctx, zmq::socket_type::sub);
    sub_tracks.set(zmq::sockopt::subscribe, "");
    sub_tracks.set(zmq::sockopt::rcvhwm, 10000);
    sub_tracks.connect("tcp://127.0.0.1:5555");

    // Subscriber to Stage 4 Kill Notifications
    zmq::socket_t sub_kills(ctx, zmq::socket_type::sub);
    sub_kills.set(zmq::sockopt::subscribe, "");
    sub_kills.set(zmq::sockopt::rcvhwm, 10000);
    sub_kills.connect("tcp://127.0.0.1:5556");

    std::map<uint32_t, ProcessedTrackState> track_database;
    std::map<uint32_t, bool> killed_tracks;
    uint32_t evaluation_cycles = 0;

    std::cout << "[Stage 3: C2/WTA Brain] Active and listening for tracks & kill reports..." << std::endl;

    while (true) {
        // 1. Process Kill Notifications from Stage 4
        zmq::message_t kill_msg;
        if (sub_kills.recv(kill_msg, zmq::recv_flags::dontwait)) {
            if (kill_msg.size() >= sizeof(ProcessedTrackState)) {
                ProcessedTrackState kill_evt;
                std::memcpy(&kill_evt, kill_msg.data(), sizeof(ProcessedTrackState));
                if (kill_evt.header.packet_type == 4) {
                    killed_tracks[kill_evt.track_id] = true;
                    track_database.erase(kill_evt.track_id);
                    std::cout << "[Stage 3 C2 Alert] Target Track #" << kill_evt.track_id 
                              << " marked KILLED. Purged from Threat Matrix." << std::endl;
                }
            }
        }

        // 2. Process Active Tracks from Stage 2
        zmq::message_t msg;
        auto res = sub_tracks.recv(msg, zmq::recv_flags::none);
        if (!res || msg.size() < sizeof(ProcessedTrackState)) continue;

        ProcessedTrackState track;
        std::memcpy(&track, msg.data(), sizeof(ProcessedTrackState));

        if (track.header.magic != 0xA6 || track.header.packet_type != 2) continue;
        if (killed_tracks[track.track_id]) continue; // Ignore destroyed threats

        track_database[track.track_id] = track;
        evaluation_cycles++;

        if (evaluation_cycles % 100 == 0 && !track_database.empty()) {
            std::vector<ThreatEvaluation> threat_list;

            for (const auto& [id, t] : track_database) {
                double dx = t.x - BASE_X;
                double dy = t.y - BASE_Y;
                double dz = t.z - BASE_Z;
                double range = std::sqrt(dx * dx + dy * dy + dz * dz);

                double closing_speed = -(dx * t.vx + dy * t.vy + dz * t.vz) / (range + 1e-6);
                double tti = (closing_speed > 1.0) ? (range / closing_speed) : 999.0;

                double threat_score = (1.0 / (range + 1.0)) * 1e6 + (1.0 / (tti + 0.1)) * 500.0;

                threat_list.push_back({id, range, closing_speed, tti, threat_score});
            }

            std::sort(threat_list.begin(), threat_list.end(), [](const ThreatEvaluation& a, const ThreatEvaluation& b) {
                return a.threat_score > b.threat_score;
            });

            std::cout << "\n================ [Stage 3 Threat Matrix @ 100Hz] ================" << std::endl;
            for (size_t i = 0; i < threat_list.size(); ++i) {
                const auto& te = threat_list[i];
                std::cout << "Priority #" << (i + 1) << " | Track ID #" << te.track_id
                          << " | Range: " << static_cast<int>(te.range_to_base) << " m"
                          << " | Closing Vel: " << static_cast<int>(te.closing_speed) << " m/s"
                          << " | TTI: " << te.time_to_impact << " s"
                          << " | Score: " << te.threat_score << std::endl;
            }
            std::cout << "=================================================================\n" << std::endl;
        }
    }

    return 0;
}