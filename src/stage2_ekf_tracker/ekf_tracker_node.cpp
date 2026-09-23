#include <iostream>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstring>
#include <Eigen/Dense>
#include <zmq.hpp>
#include "telemetry_protocol.hpp"

// Radar Origin Coordinates (Matching Stage 1 Base)
constexpr double RADAR_ORIGIN_X = 0.0;
constexpr double RADAR_ORIGIN_Y = 0.0;
constexpr double RADAR_ORIGIN_Z = 0.0;

// GNN Data Association & Persistence Tweaks
constexpr double MAHALANOBIS_GATE_THRESHOLD = 25.0; // Widened gate for dense multi-target noise
constexpr int MAX_MISSED_UPDATES = 100;              // Retain track memory across temporary dropouts

struct TrackedEntity {
    uint32_t track_id;
    Eigen::VectorXd x; // State Vector [x, y, z, vx, vy, vz]
    Eigen::MatrixXd P; // Covariance Matrix (6x6)
    int missed_updates{0};
    bool active{true};
};

// Convert Spherical Radar Measurement (range, az, el) to Cartesian (x, y, z)
Eigen::Vector3d spherical_to_cartesian(double range, double azimuth, double elevation) {
    double x = RADAR_ORIGIN_X + range * std::cos(elevation) * std::cos(azimuth);
    double y = RADAR_ORIGIN_Y + range * std::cos(elevation) * std::sin(azimuth);
    double z = RADAR_ORIGIN_Z + range * std::sin(elevation);
    return Eigen::Vector3d(x, y, z);
}

int main() {
    std::cout << "==================================================\n";
    std::cout << "      AGISX STAGE 2: EKF MULTI-TARGET TRACKER     \n";
    std::cout << "==================================================\n";
    std::cout << "[Config] Mahalanobis Gate Threshold : " << MAHALANOBIS_GATE_THRESHOLD << "\n";
    std::cout << "[Config] Max Missed Update Allowance : " << MAX_MISSED_UPDATES << " frames\n";
    std::cout << "--------------------------------------------------\n";

    zmq::context_t ctx(1);

    // Subscriber for Raw Radar Blips
    zmq::socket_t sub_blips(ctx, zmq::socket_type::sub);
    sub_blips.set(zmq::sockopt::subscribe, "");
    sub_blips.connect("tcp://127.0.0.1:5554");

    // Publisher for Processed EKF Tracks
    zmq::socket_t pub_tracks(ctx, zmq::socket_type::pub);
    pub_tracks.bind("tcp://127.0.0.1:5555");

    std::vector<TrackedEntity> tracks;
    uint32_t next_track_id = 1;

    double dt = 0.01; // 100 Hz sampling step

    // Process Noise Covariance (Q)
    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(6, 6) * 1.0;
    
    // Measurement Noise Covariance (R)
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity() * 225.0; // 15m range std-dev squared

    std::cout << "[Stage 2: EKF Tracker] Multi-Target Tracking Engine Running..." << std::endl;

    while (true) {
        zmq::message_t msg;
        if (sub_blips.recv(msg, zmq::recv_flags::dontwait)) {
            if (msg.size() >= sizeof(RawRadarMeasurement)) {
                RawRadarMeasurement blip;
                std::memcpy(&blip, msg.data(), sizeof(RawRadarMeasurement));

                Eigen::Vector3d z_meas = spherical_to_cartesian(blip.range, blip.azimuth, blip.elevation);

                // --- 1. EKF Predict Step for Active Tracks ---
                Eigen::MatrixXd F = Eigen::MatrixXd::Identity(6, 6);
                F(0, 3) = dt;
                F(1, 4) = dt;
                F(2, 5) = dt;

                for (auto& trk : tracks) {
                    if (trk.active) {
                        trk.x = F * trk.x;
                        trk.P = F * trk.P * F.transpose() + Q;
                        trk.missed_updates++;
                    }
                }

                // --- 2. Global Nearest Neighbor (GNN) Association ---
                int best_track_idx = -1;
                double min_mahalanobis_dist = 1e9;

                Eigen::MatrixXd H = Eigen::MatrixXd::Zero(3, 6);
                H(0, 0) = 1.0; H(1, 1) = 1.0; H(2, 2) = 1.0;

                for (size_t i = 0; i < tracks.size(); ++i) {
                    if (!tracks[i].active) continue;

                    Eigen::Vector3d z_pred = tracks[i].x.head<3>();
                    Eigen::Vector3d y_res = z_meas - z_pred;
                    Eigen::Matrix3d S = H * tracks[i].P * H.transpose() + R;

                    double dist = std::sqrt(y_res.transpose() * S.inverse() * y_res);

                    if (dist < min_mahalanobis_dist && dist <= MAHALANOBIS_GATE_THRESHOLD) {
                        min_mahalanobis_dist = dist;
                        best_track_idx = static_cast<int>(i);
                    }
                }

                // --- 3. Update Existing Track or Initiate New Track ---
                if (best_track_idx != -1) {
                    // Update matching track
                    auto& trk = tracks[best_track_idx];
                    trk.missed_updates = 0;

                    Eigen::Vector3d z_pred = trk.x.head<3>();
                    Eigen::Vector3d y_res = z_meas - z_pred;
                    Eigen::Matrix3d S = H * trk.P * H.transpose() + R;
                    Eigen::MatrixXd K = trk.P * H.transpose() * S.inverse();

                    trk.x = trk.x + K * y_res;
                    trk.P = (Eigen::MatrixXd::Identity(6, 6) - K * H) * trk.P;
                } else {
                    // Initialize new track profile
                    TrackedEntity new_trk;
                    new_trk.track_id = next_track_id++;
                    new_trk.x = Eigen::VectorXd::Zero(6);
                    new_trk.x.head<3>() = z_meas;
                    new_trk.P = Eigen::MatrixXd::Identity(6, 6) * 100.0;
                    tracks.push_back(new_trk);

                    std::cout << "[Stage 2: EKF Tracker] Initiated Track ID #" << new_trk.track_id << std::endl;
                }

                // --- 4. Prune Stale Tracks exceeding update allowance ---
                for (auto& trk : tracks) {
                    if (trk.active && trk.missed_updates > MAX_MISSED_UPDATES) {
                        trk.active = false;
                        std::cout << "[Stage 2: EKF Tracker] Pruned stale Track ID #" << trk.track_id << std::endl;
                    }
                }

                // --- 5. Broadcast Updated Active Track States ---
                for (const auto& trk : tracks) {
                    if (trk.active && trk.missed_updates < 5) {
                        ProcessedTrackState track_pkt{};
                        track_pkt.header.magic = 0xA6;
                        track_pkt.header.version = AGISX_PROTOCOL_VERSION;
                        track_pkt.header.packet_type = 2;
                        track_pkt.track_id = trk.track_id;

                        track_pkt.x  = trk.x(0); track_pkt.y  = trk.x(1); track_pkt.z  = trk.x(2);
                        track_pkt.vx = trk.x(3); track_pkt.vy = trk.x(4); track_pkt.vz = trk.x(5);

                        zmq::message_t track_msg(&track_pkt, sizeof(ProcessedTrackState));
                        pub_tracks.send(track_msg, zmq::send_flags::none);
                    }
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}