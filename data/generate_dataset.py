import os
import math
import csv

# Directory Setup
OUTPUT_DIR = "data/trajectories"
os.makedirs(OUTPUT_DIR, exist_ok=True)

# Global Physical Constants
G = 9.80665            # Standard gravity (m/s^2)
RHO_0 = 1.225          # Sea-level atmospheric air density (kg/m^3)
H_0 = 7400.0           # Atmospheric scale height (m)

def air_density(z: float) -> float:
    """Calculates density using the barometric exponential profile."""
    if z < 0.0:
        return RHO_0
    return RHO_0 * math.exp(-z / H_0)

def generate_ballistic_srbm():
    """Profile Alpha: Short-Range Ballistic Missile (SRBM).
    Re-entry phase influenced by gravity and quadratic aerodynamic drag.
    """
    file_path = os.path.join(OUTPUT_DIR, "profile_alpha_ballistic.csv")
    dt = 0.01  # 100 Hz simulation rate
    
    # Missile Physical Characteristics (Scud/Iskander class)
    mass = 3800.0            # Mass in kg
    radius = 0.44            # Frontal radius in m
    area = math.pi * (radius ** 2)  # Cross-sectional area: ~0.608 m^2
    cd = 0.32                # Supersonic drag coefficient
    
    # Initial Kinematic Conditions (Pointed down toward origin from 30 km out)
    x = 24000.0
    y = 12000.0
    z = 14000.0
    vx = -650.0
    vy = -320.0
    vz = -280.0
    
    time = 0.0
    records = []
    
    while z > 0.0:
        v = math.sqrt(vx**2 + vy**2 + vz**2)
        rho = air_density(z)
        
        # Dynamic pressure: q = 0.5 * rho * v^2
        # Aerodynamic drag force: F_d = q * cd * area
        f_drag = 0.5 * rho * (v**2) * cd * area
        
        # Drag acts opposite to velocity vector: a_drag = -(F_d / m) * (v_vec / v)
        ax = -(f_drag / mass) * (vx / v)
        ay = -(f_drag / mass) * (vy / v)
        az = -G - (f_drag / mass) * (vz / v)
        
        records.append([round(time, 3), round(x, 2), round(y, 2), round(z, 2),
                        round(vx, 2), round(vy, 2), round(vz, 2),
                        round(ax, 2), round(ay, 2), round(az, 2)])
        
        # Semi-implicit Euler integration
        vx += ax * dt
        vy += ay * dt
        vz += az * dt
        
        x += vx * dt
        y += vy * dt
        z += vz * dt
        time += dt

    _save_csv(file_path, records)
    print(f"[Dataset] Generated SRBM Profile: {len(records)} frames -> {file_path}")

def generate_cruise_missile():
    """Profile Bravo: Low-Altitude Subsonic Cruise Missile.
    Maintains altitude and speed while following a heading dog-leg corridor.
    """
    file_path = os.path.join(OUTPUT_DIR, "profile_bravo_cruise.csv")
    dt = 0.01
    
    # Kinematics: Terrain hugging at 180 m altitude, Mach 0.85 (~290 m/s)
    speed = 290.0
    z = 180.0
    vz = 0.0
    az = 0.0
    
    x = -22000.0
    y = 18000.0
    time = 0.0
    records = []
    
    while time < 80.0:
        # Heading angle calculation toward asset (0,0) with a 30-second dog-leg waypoint turn
        if time < 30.0:
            target_heading = math.atan2(-y, -x) + 0.35  # Offset waypoint to evade early radar
        else:
            target_heading = math.atan2(-y, -x)        # Inbound homing terminal leg
            
        vx = speed * math.cos(target_heading)
        vy = speed * math.sin(target_heading)
        
        # Smooth turn rates
        ax = 0.0
        ay = 0.0
        
        records.append([round(time, 3), round(x, 2), round(y, 2), round(z, 2),
                        round(vx, 2), round(vy, 2), round(vz, 2),
                        round(ax, 2), round(ay, 2), round(az, 2)])
        
        x += vx * dt
        y += vy * dt
        time += dt
        
        if math.sqrt(x**2 + y**2) < 100.0:
            break

    _save_csv(file_path, records)
    print(f"[Dataset] Generated Cruise Profile: {len(records)} frames -> {file_path}")

def generate_hypersonic_glide():
    """Profile Charlie: Hypersonic Maneuvering Vehicle (HGV).
    Flies at Mach 4.5+ executing oscillatory high-G lateral weaving S-turns.
    """
    file_path = os.path.join(OUTPUT_DIR, "profile_charlie_hypersonic.csv")
    dt = 0.01
    
    x = 35000.0
    y = -15000.0
    z = 8500.0
    
    speed = 1500.0          # Mach 4.41 (m/s)
    heading = math.atan2(-y, -x)
    omega = 0.45            # Weave frequency in rad/s
    lateral_accel_g = 6.5   # 6.5G lateral evasive maneuvers
    
    time = 0.0
    records = []
    
    while z > 200.0 and math.sqrt(x**2 + y**2) > 300.0:
        # Base velocity vector toward asset
        vx_nominal = speed * math.cos(heading)
        vy_nominal = speed * math.sin(heading)
        
        # Oscillating lateral acceleration: a_lat = A_max * sin(omega * t)
        a_lat = (lateral_accel_g * G) * math.sin(omega * time)
        
        # Project lateral acceleration perpendicular to nominal heading
        ax = -a_lat * math.sin(heading)
        ay = a_lat * math.cos(heading)
        az = -3.5  # Shallow controlled descent
        
        vx = vx_nominal + ax * dt
        vy = vy_nominal + ay * dt
        vz = az * time * 0.1  # Glide descent profile
        
        records.append([round(time, 3), round(x, 2), round(y, 2), round(z, 2),
                        round(vx, 2), round(vy, 2), round(vz, 2),
                        round(ax, 2), round(ay, 2), round(az, 2)])
        
        x += vx * dt
        y += vy * dt
        z += vz * dt
        time += dt

    _save_csv(file_path, records)
    print(f"[Dataset] Generated Hypersonic Profile: {len(records)} frames -> {file_path}")

def _save_csv(file_path, records):
    headers = ["time_s", "x_m", "y_m", "z_m", "vx_mps", "vy_mps", "vz_mps", "ax_mps2", "ay_mps2", "az_mps2"]
    with open(file_path, mode="w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        writer.writerows(records)

if __name__ == "__main__":
    generate_ballistic_srbm()
    generate_cruise_missile()
    generate_hypersonic_glide()