import sys
import math
import struct
import time
import zmq
import pygame

# Initialize PyGame
pygame.init()
WIDTH, HEIGHT = 1280, 800
screen = pygame.display.set_mode((WIDTH, HEIGHT))
pygame.display.set_caption("AGISX - Air Ground Integrated System X | C2 Tactical Command Scope")
clock = pygame.time.Clock()

# Color Palette (Military Obsidian / Tactical Neon)
COLOR_BG = (3, 6, 9)
COLOR_PANEL_BG = (7, 12, 18)
COLOR_GRID = (0, 50, 30)
COLOR_GRID_TEXT = (0, 130, 70)
COLOR_TEXT = (0, 255, 120)
COLOR_TEXT_DIM = (0, 150, 85)
COLOR_THREAT = (255, 45, 45)          # Hostile Red
COLOR_THREAT_BLINK = (255, 130, 130)  # Pulse Highlight
COLOR_INTERCEPTOR = (0, 220, 255)     # Friendly Cyan
COLOR_BASE = (255, 215, 0)            # Defended Asset Yellow
COLOR_KILL = (255, 150, 0)            # Detonation Orange

# ZeroMQ Telemetry Setup
ctx = zmq.Context()

sub_tracks = ctx.socket(zmq.SUB)
sub_tracks.setsockopt(zmq.SUBSCRIBE, b"")
sub_tracks.setsockopt(zmq.RCVHWM, 10000)
sub_tracks.connect("tcp://127.0.0.1:5555")

sub_kills = ctx.socket(zmq.SUB)
sub_kills.setsockopt(zmq.SUBSCRIBE, b"")
sub_kills.setsockopt(zmq.RCVHWM, 10000)
sub_kills.connect("tcp://127.0.0.1:5556")

# State Management
active_tracks = {}      # t_id -> {x, y, z, vx, vy, vz, tx, ty, tz, tvx, tvy, tvz, time}
trail_history = {}      # t_id -> list of (x, y)
kill_effects = []       # list of explosion dicts
event_logs = []         # Live C2 scrolling log messages
total_intercepts = 0

# Scope Geometry
RADAR_CENTER = (450, 400)
RADAR_RADIUS = 350
dynamic_max_range = 1000.0  # Dynamic scale in meters

# Fonts
font_xs = pygame.font.SysFont("Courier", 10)
font_sm = pygame.font.SysFont("Courier", 11)
font_md = pygame.font.SysFont("Courier", 13, bold=True)
header_font = pygame.font.SysFont("Courier", 17, bold=True)

def log_event(msg):
    timestamp = time.strftime("%H:%M:%S")
    event_logs.append(f"[{timestamp}] {msg}")
    if len(event_logs) > 8:
        event_logs.pop(0)

log_event("SYSTEM INITIALIZED: ZMQ Subscriptions Active.")

def parse_cpp_telemetry(msg):
    """Robust binary parser bypassing C++ compiler struct padding alignments."""
    if len(msg) < 52:
        return None
    try:
        t_id = struct.unpack("<I", msg[-52:-48])[0]
        kinematics = struct.unpack("<dddddd", msg[-48:])
        if any(abs(v) > 0.001 for v in kinematics):
            return t_id, kinematics
    except Exception:
        pass

    for offset in range(len(msg) - 48, 12, -4):
        try:
            kinematics = struct.unpack("<dddddd", msg[offset:offset+48])
            if any(abs(v) > 0.001 for v in kinematics):
                t_id = struct.unpack("<I", msg[offset-4:offset])[0]
                return t_id, kinematics
        except Exception:
            continue
    return None

def world_to_screen(x, y, max_range):
    scale = RADAR_RADIUS / max_range
    sx = int(RADAR_CENTER[0] + x * scale)
    sy = int(RADAR_CENTER[1] - y * scale)
    return sx, sy

running = True
sweep_angle = 0.0

print("[HUD] AGISX Advanced Tactical Simulation Engine Online...")

while running:
    sim_time = time.time()
    for event in pygame.event.get():
        if event.type == pygame.QUIT or (event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE):
            running = False

    # 1. Process Intercept / Kill Events (Port 5556)
    try:
        while True:
            k_msg = sub_kills.recv(flags=zmq.NOBLOCK)
            if len(k_msg) >= 12:
                t_id = struct.unpack("<I", k_msg[-4:])[0]
                if t_id in active_tracks:
                    kx, ky = active_tracks[t_id]['x'], active_tracks[t_id]['y']
                    kill_effects.append({'x': kx, 'y': ky, 'radius': 4.0, 'max': 45.0})
                    total_intercepts += 1
                    log_event(f"INTERCEPT CONFIRMED: TRK #{t_id:03d} Neutralized.")
                    del active_tracks[t_id]
                    if t_id in trail_history:
                        del trail_history[t_id]
    except zmq.Again:
        pass

    # 2. Process Track Telemetry (Port 5555)
    try:
        while True:
            t_msg = sub_tracks.recv(flags=zmq.NOBLOCK)
            parsed = parse_cpp_telemetry(t_msg)
            if parsed:
                t_id, (x, y, z, vx, vy, vz) = parsed
                
                # Auto-scale if received in kilometers
                if abs(x) < 50.0 and abs(y) < 50.0 and (abs(vx) > 0.1 or abs(vy) > 0.1):
                    x, y, z = x * 1000.0, y * 1000.0, z * 1000.0

                if t_id not in active_tracks:
                    active_tracks[t_id] = {
                        'x': x, 'y': y, 'z': z, 'vx': vx, 'vy': vy, 'vz': vz,
                        'tx': x, 'ty': y, 'tz': z, 'tvx': vx, 'tvy': vy, 'tvz': vz,
                        'time': sim_time
                    }
                    log_event(f"NEW TRACK ACQUIRED: #{t_id:03d}")
                else:
                    active_tracks[t_id]['tx'] = x
                    active_tracks[t_id]['ty'] = y
                    active_tracks[t_id]['tz'] = z
                    active_tracks[t_id]['tvx'] = vx
                    active_tracks[t_id]['tvy'] = vy
                    active_tracks[t_id]['tvz'] = vz
                    active_tracks[t_id]['time'] = sim_time

                if t_id not in trail_history:
                    trail_history[t_id] = []
                trail_history[t_id].append((x, y))
                if len(trail_history[t_id]) > 20:
                    trail_history[t_id].pop(0)
    except zmq.Again:
        pass

    # Smooth Interpolation (Lerp) for Track Positions
    track_items = list(active_tracks.items())
    max_dist_found = 1000.0

    for t_id, trk in track_items:
        trk['x'] += (trk['tx'] - trk['x']) * 0.25
        trk['y'] += (trk['ty'] - trk['y']) * 0.25
        trk['z'] += (trk['tz'] - trk['z']) * 0.25
        trk['vx'] += (trk['tvx'] - trk['vx']) * 0.25
        trk['vy'] += (trk['tvy'] - trk['vy']) * 0.25
        trk['vz'] += (trk['tvz'] - trk['vz']) * 0.25

        d = math.hypot(trk['x'], trk['y'])
        if d > max_dist_found:
            max_dist_found = d

    # Dynamic Scope Scaling
    target_scale = max(1000.0, math.ceil(max_dist_found / 500.0) * 500.0)
    dynamic_max_range += (target_scale - dynamic_max_range) * 0.1

    # Render Screen Background & Sidebar Panel
    screen.fill(COLOR_BG)
    pygame.draw.rect(screen, COLOR_PANEL_BG, (900, 0, 380, HEIGHT))
    pygame.draw.line(screen, COLOR_GRID, (900, 0), (900, HEIGHT), 2)

    # Render Concentric Range Rings & Compass Markers
    ring_step = dynamic_max_range / 5.0
    for i in range(1, 6):
        r_dist = ring_step * i
        r_pix = int(r_dist * (RADAR_RADIUS / dynamic_max_range))
        pygame.draw.circle(screen, COLOR_GRID, RADAR_CENTER, r_pix, 1)
        
        unit_str = f"{r_dist/1000.0:.1f}KM" if dynamic_max_range >= 5000 else f"{int(r_dist)}M"
        lbl = font_sm.render(unit_str, True, COLOR_GRID_TEXT)
        screen.blit(lbl, (RADAR_CENTER[0] + 4, RADAR_CENTER[1] - r_pix + 2))

    # Crosshairs & Perimeter
    pygame.draw.circle(screen, COLOR_GRID_TEXT, RADAR_CENTER, RADAR_RADIUS, 2)
    pygame.draw.line(screen, COLOR_GRID, (RADAR_CENTER[0] - RADAR_RADIUS, RADAR_CENTER[1]), 
                     (RADAR_CENTER[0] + RADAR_RADIUS, RADAR_CENTER[1]), 1)
    pygame.draw.line(screen, COLOR_GRID, (RADAR_CENTER[0], RADAR_CENTER[1] - RADAR_RADIUS), 
                     (RADAR_CENTER[0], RADAR_CENTER[1] + RADAR_RADIUS), 1)

    # Radar Sweep Line & Afterglow Sector
    sweep_angle += 0.03
    for a_offset in [0.0, -0.05, -0.10, -0.15]:
        curr_a = sweep_angle + a_offset
        sw_x = RADAR_CENTER[0] + int(RADAR_RADIUS * math.cos(curr_a))
        sw_y = RADAR_CENTER[1] + int(RADAR_RADIUS * math.sin(curr_a))
        alpha_color = (0, int(180 + a_offset * 600), int(80 - a_offset * 200))
        pygame.draw.line(screen, alpha_color, RADAR_CENTER, (sw_x, sw_y), 1)

    # Defended Asset Target (Origin Base)
    pygame.draw.circle(screen, COLOR_BASE, RADAR_CENTER, 6)
    pygame.draw.circle(screen, COLOR_BASE, RADAR_CENTER, 14, 1)

    # Render Trajectory Trails
    for t_id, pts in list(trail_history.items()):
        if len(pts) > 1 and t_id in active_tracks:
            scr_pts = [world_to_screen(px, py, dynamic_max_range) for px, py in pts]
            pygame.draw.lines(screen, (140, 45, 45), False, scr_pts, 1)

    # Active Threats & Impact Projection Rendering (Clean, No Jittery Text)
    blink_state = int(sim_time * 5) % 2 == 0

    for t_id, trk in track_items:
        if sim_time - trk['time'] > 4.0:
            log_event(f"TRACK LOST (TIMEOUT): #{t_id:03d}")
            del active_tracks[t_id]
            continue

        sx, sy = world_to_screen(trk['x'], trk['y'], dynamic_max_range)
        dist = math.hypot(sx - RADAR_CENTER[0], sy - RADAR_CENTER[1])

        if dist <= RADAR_RADIUS:
            scale = RADAR_RADIUS / dynamic_max_range
            v_x = sx + int(trk['vx'] * scale * 2.5)
            v_y = sy - int(trk['vy'] * scale * 2.5)

            # Impact Cone Projection Line towards Base
            rng_m = math.hypot(trk['x'], trk['y'])
            if rng_m > 150:
                base_sx, base_sy = world_to_screen(0, 0, dynamic_max_range)
                pygame.draw.line(screen, (160, 40, 40), (sx, sy), (base_sx, base_sy), 1)

            color = COLOR_THREAT_BLINK if blink_state else COLOR_THREAT

            # Threat Diamond Symbol & Velocity Vector (Clean ID tag only)
            d = 5
            pygame.draw.polygon(screen, color, [(sx, sy - d), (sx + d, sy), (sx, sy + d), (sx - d, sy)], 1)
            pygame.draw.line(screen, color, (sx, sy), (v_x, v_y), 1)

            id_lbl = font_sm.render(f"#{t_id}", True, color)
            screen.blit(id_lbl, (sx + 6, sy - 6))

    # Detonation Explosions FX
    for kill in kill_effects[:]:
        ksx, ksy = world_to_screen(kill['x'], kill['y'], dynamic_max_range)
        rad = int(kill['radius'])
        pygame.draw.circle(screen, COLOR_KILL, (ksx, ksy), rad, 2)
        pygame.draw.circle(screen, (255, 255, 255), (ksx, ksy), max(1, rad // 2), 1)
        kill['radius'] += 1.8
        if kill['radius'] >= kill['max']:
            kill_effects.remove(kill)

    # Top-Left Header Overlay
    screen.blit(header_font.render("AGISX C2 TACTICAL DEFENSE SYSTEM", True, COLOR_TEXT), (20, 20))
    screen.blit(font_md.render(f"ACTIVE TRACKS: {len(active_tracks)} | SCOPE: {dynamic_max_range/1000.0:.1f}KM", True, COLOR_TEXT_DIM), (20, 48))

    # Sidebar Panel Header
    screen.blit(header_font.render("REAL-TIME TELEMETRY", True, COLOR_TEXT), (920, 20))
    screen.blit(font_md.render(f"TOTAL INTERCEPTS: {total_intercepts}", True, COLOR_INTERCEPTOR), (920, 45))
    pygame.draw.line(screen, COLOR_GRID, (920, 68), (1250, 68), 1)

    # Populate Real-Time Threat Matrix in Sidebar
    sorted_threats = sorted(
        track_items,
        key=lambda item: math.hypot(item[1]['x'], item[1]['y'], item[1]['z'])
    )[:7]

    y_off = 80
    for t_id, trk in sorted_threats:
        rng_m = math.hypot(trk['x'], trk['y'], trk['z'])
        alt_m = trk['z']
        spd_ms = math.hypot(trk['vx'], trk['vy'], trk['vz'])

        rng_str = f"{rng_m/1000.0:4.1f}km" if rng_m >= 1000 else f"{int(rng_m)}m"
        alt_str = f"{alt_m/1000.0:4.1f}km" if abs(alt_m) >= 1000 else f"{int(alt_m)}m"

        t_card = font_md.render(f"HOSTILE TRK #{t_id:03d}", True, COLOR_THREAT_BLINK if blink_state else COLOR_THREAT)
        s_card = font_sm.render(f"  RNG:{rng_str} | ALT:{alt_str} | SPD:{spd_ms:4.0f}m/s", True, COLOR_TEXT_DIM)

        screen.blit(t_card, (920, y_off))
        screen.blit(s_card, (920, y_off + 16))
        y_off += 42

    # Bottom Sidebar C2 Event Log
    pygame.draw.line(screen, COLOR_GRID, (920, 520), (1250, 520), 1)
    screen.blit(font_md.render("C2 EVENT CONSOLE LOG", True, COLOR_TEXT), (920, 532))

    log_y = 556
    for log_msg in event_logs[-6:]:
        l_card = font_xs.render(log_msg, True, COLOR_TEXT_DIM)
        screen.blit(l_card, (920, log_y))
        log_y += 18

    pygame.display.flip()
    clock.tick(60)

pygame.quit()
sys.exit()