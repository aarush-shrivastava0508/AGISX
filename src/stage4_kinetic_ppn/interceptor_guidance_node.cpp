#include <iostream>
#include <cstring>
#include <map>
#include <Eigen/Dense>
#include <zmq.hpp>
#include "telemetry_protocol.hpp"

constexpr double NAV_CONST_N = 4.0;
constexpr double INTERCEPTOR_MAX_ACCEL = 300.0; // m/s^2 (~30G limit)

struct Interceptor {
    uint32_t id;
    uint32_t target_track_id;
    Eigen::Vector3d pos;
    Eigen::Vector3d vel;
    double last_distance;
    bool active;
};

int main() {
    std::cout << "[Stage 4: Interceptor Guidance] Initializing Dynamic Multi-Salvo Engine..." << std::endl;

    zmq::context_t ctx(1);

    // Subscriber to Stage 2 Track Stream
    zmq::socket_t sub(ctx, zmq::socket_type::sub);
    sub.set(zmq::sockopt::subscribe, "");
    sub.set(zmq::sockopt::rcvhwm, 10000);
    sub.connect("tcp://127.0.0.1:5555");

    // Publisher for Kill Notifications back to Tracker & C2 Brain
    zmq::socket_t pub_kill(ctx, zmq::socket_type::pub);
    pub_kill.bind("tcp://127.0.0.1:5556");

    std::map<uint32_t, Interceptor> interceptors;
    std::map<uint32_t, bool> eliminated_tracks;
    
    uint32_t next_interceptor_id = 1;
    uint32_t guidance_frames = 0;

    std::cout << "[Stage 4: Interceptor Guidance] Ready for dynamic threat engagement..." << std::endl;

    while (true) {
        zmq::message_t msg;
        auto res = sub.recv(msg, zmq::recv_flags::none);
        if (!res || msg.size() < sizeof(ProcessedTrackState)) continue;

        ProcessedTrackState track;
        std::memcpy(&track, msg.data(), sizeof(ProcessedTrackState));

        if (track.header.magic != 0xA6 || track.header.packet_type != 2) continue;

        // Skip if target was already killed
        if (eliminated_tracks[track.track_id]) continue;

        // Auto-assign new interceptor if track has no active missile assigned
        if (interceptors.find(track.track_id) == interceptors.end()) {
            Interceptor new_missile;
            new_missile.id = next_interceptor_id++;
            new_missile.target_track_id = track.track_id;
            new_missile.pos = Eigen::Vector3d(0.0, 0.0, 0.0);
            new_missile.vel = Eigen::Vector3d(0.0, 0.0, 800.0);
            new_missile.last_distance = 1e9;
            new_missile.active = true;

            interceptors[track.track_id] = new_missile;
            std::cout << "\n[Stage 4 Launch] Interceptor #" << new_missile.id 
                      << " Launched against Threat Track #" << track.track_id << "!" << std::endl;
        }

        auto& interceptor = interceptors[track.track_id];
        if (!interceptor.active) continue;

        guidance_frames++;

        Eigen::Vector3d target_pos(track.x, track.y, track.z);
        Eigen::Vector3d target_vel(track.vx, track.vy, track.vz);

        Eigen::Vector3d r_rel = target_pos - interceptor.pos;
        Eigen::Vector3d v_rel = target_vel - interceptor.vel;

        double distance = r_rel.norm();

        // Line-of-Sight rate calculation
        Eigen::Vector3d omega_los = r_rel.cross(v_rel) / (distance * distance + 1e-6);

        // Proportional Navigation Guidance
        Eigen::Vector3d accel_cmd = NAV_CONST_N * v_rel.cross(omega_los);

        if (accel_cmd.norm() > INTERCEPTOR_MAX_ACCEL) {
            accel_cmd = accel_cmd.normalized() * INTERCEPTOR_MAX_ACCEL;
        }

        double dt = 0.01;
        interceptor.vel += accel_cmd * dt;
        interceptor.pos += interceptor.vel * dt;

        // Closest Point of Approach (CPA) check to prevent high-speed overshoots
        bool cpa_triggered = (distance > interceptor.last_distance) && (distance < 50.0);
        interceptor.last_distance = distance;

        // Lethal proximity trigger
        if (distance < 25.0 || cpa_triggered) {
            std::cout << "\n=================================================================" << std::endl;
            std::cout << ">>> [KINETIC INTERCEPT CONFIRMED] Threat Track #" << track.track_id
                      << " Destroyed by Interceptor #" << interceptor.id 
                      << " at Range: " << distance << " m <<<" << std::endl;
            std::cout << "=================================================================\n" << std::endl;
            
            interceptor.active = false;
            eliminated_tracks[track.track_id] = true;

            // Broadcast Kill Event to Stage 2 & Stage 3
            ProcessedTrackState kill_msg = track;
            kill_msg.header.packet_type = 4; // 4 = Kill Event / Track Destroyed
            zmq::message_t out_kill(&kill_msg, sizeof(ProcessedTrackState));
            pub_kill.send(out_kill, zmq::send_flags::none);
        } else if (guidance_frames % 200 == 0) {
            std::cout << "[Stage 4 @ 100Hz] Missile #" << interceptor.id 
                      << " -> Track #" << track.track_id 
                      << " | Dist: " << static_cast<int>(distance) 
                      << " m | Accel: " << static_cast<int>(accel_cmd.norm()) << " m/s^2" << std::endl;
        }
    }

    return 0;
}