#!/usr/bin/env python3
"""
RPi Setup for Arca VNC Client

Configures a Raspberry Pi as a VNC server for the Arca display.
Installs x11vnc on port 5900 (RealVNC is disabled).

Handles Raspbian Buster EOL repository migration automatically.

Usage:
    python3 rpi_setup.py <host> <user> <password> [action]

Actions:
    fix-and-install  - Fix EOL repos + install x11vnc (default)
    manual-install   - Try direct .deb download if apt fails
    verify           - Check x11vnc service status
    inspect          - Inspect RPi display environment
    ssh-key          - Set up SSH key auth (placeholder)

Requires: pip install paramiko
"""

import paramiko
import sys
import time

def ssh_exec(client, cmd, timeout=60):
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode('utf-8', errors='replace').strip()
    err = stderr.read().decode('utf-8', errors='replace').strip()
    return out, err

def main():
    if len(sys.argv) < 4:
        print("Usage: python3 rpi_setup.py <host> <user> <password> [action]")
        print("Actions: fix-and-install (default), manual-install, verify, inspect, ssh-key")
        sys.exit(1)

    rpi_host = sys.argv[1]
    rpi_user = sys.argv[2]
    rpi_pass = sys.argv[3]
    action = sys.argv[4] if len(sys.argv) > 4 else "fix-and-install"

    print(f"Connecting to {rpi_user}@{rpi_host}...")
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(rpi_host, username=rpi_user, password=rpi_pass, timeout=10)
    print("Connected!\n")

    if action == "fix-and-install":
        fix_repos_and_install(client)
    elif action == "manual-install":
        manual_install(client)
    elif action == "verify":
        verify(client)
    elif action == "inspect":
        inspect(client)
    elif action == "ssh-key":
        setup_ssh_key(client)
    else:
        print(f"Unknown action: {action}")
        sys.exit(1)

    client.close()

def fix_repos_and_install(client):
    """Fix EOL repos and install x11vnc."""
    
    # First, try to fix apt sources for Buster
    print("=== Fixing apt sources for Buster EOL ===")
    
    # Backup current sources
    ssh_exec(client, "sudo cp /etc/apt/sources.list /etc/apt/sources.list.bak 2>/dev/null")
    
    # Try the archive repo
    cmds = [
        # Check what sources exist
        "cat /etc/apt/sources.list",
    ]
    
    for cmd in cmds:
        out, err = ssh_exec(client, cmd)
        print(f"$ {cmd}")
        print(out[:500] if out else "(empty)")
        if err:
            print(f"  err: {err[:200]}")
        print()
    
    # Fix sources.list to use archive 
    print("=== Updating sources.list to use archive repos ===")
    new_sources = "deb http://legacy.raspbian.org/raspbian/ buster main contrib non-free rpi"
    out, err = ssh_exec(client, f'echo "{new_sources}" | sudo tee /etc/apt/sources.list')
    print(f"New sources.list: {out}")
    
    # Also fix raspberry pi archive
    out, err = ssh_exec(client, 'echo "deb http://legacy.raspberrypi.org/debian/ buster main" | sudo tee /etc/apt/sources.list.d/raspi.list')
    print(f"New raspi.list: {out}")
    
    # Update and install
    print("\n=== Running apt-get update ===")
    out, err = ssh_exec(client, "sudo apt-get update 2>&1 | tail -10", timeout=120)
    print(out)
    if err:
        print(f"  err: {err[:300]}")
    
    print("\n=== Installing x11vnc ===")
    out, err = ssh_exec(client, "sudo apt-get install -y x11vnc 2>&1 | tail -15", timeout=120)
    print(out)
    
    # Verify
    out, _ = ssh_exec(client, "which x11vnc 2>/dev/null && x11vnc --version 2>&1 | head -1")
    if "x11vnc" in out:
        print(f"\nx11vnc installed: {out}")
        configure_x11vnc(client)
    else:
        print("\nFailed to install x11vnc via apt. Trying manual install...")
        manual_install(client)

def manual_install(client):
    """Try to install x11vnc from a direct .deb download."""
    print("=== Trying manual x11vnc installation ===")
    
    # Download x11vnc for armhf buster
    url = "http://archive.raspbian.org/raspbian/pool/main/x/x11vnc/x11vnc_0.9.13-6_armhf.deb"
    out, err = ssh_exec(client, f"wget -q '{url}' -O /tmp/x11vnc.deb 2>&1 && echo 'Downloaded' || echo 'Download failed'", timeout=60)
    print(out)
    
    if "Downloaded" in out:
        # Install with dependencies
        out, err = ssh_exec(client, "sudo dpkg -i /tmp/x11vnc.deb 2>&1; sudo apt-get -f install -y 2>&1 | tail -10", timeout=60)
        print(out)
    else:
        # Try another mirror
        url2 = "http://ftp.debian.org/debian/pool/main/x/x11vnc/x11vnc_0.9.13-6_armhf.deb"
        out, err = ssh_exec(client, f"wget -q '{url2}' -O /tmp/x11vnc.deb 2>&1 && echo 'Downloaded' || echo 'Download failed'", timeout=60)
        print(out)
        if "Downloaded" in out:
            out, err = ssh_exec(client, "sudo dpkg -i /tmp/x11vnc.deb 2>&1; sudo apt-get -f install -y 2>&1 | tail -10", timeout=60)
            print(out)
    
    # Check
    out, _ = ssh_exec(client, "which x11vnc")
    if out:
        print(f"\nx11vnc installed at: {out}")
        configure_x11vnc(client)
    else:
        print("\nCould not install x11vnc. Falling back to dispmanx VNC...")
        install_dispmanx_vnc(client)

def install_dispmanx_vnc(client):
    """Install dispmanx_vnc as a fallback - lightweight, no X11 dependency."""
    print("\n=== Trying dispmanx_vnc (lightweight alternative) ===")
    out, err = ssh_exec(client, """
        cd /tmp &&
        git clone https://github.com/patsm00re/dispmanx_vnc.git 2>&1 &&
        cd dispmanx_vnc &&
        make 2>&1 | tail -5 &&
        sudo cp dispmanx_vncserver /usr/local/bin/ &&
        echo 'dispmanx_vnc installed'
    """, timeout=120)
    print(out)
    if err:
        print(f"  err: {err[:300]}")

def configure_x11vnc(client):
    """Configure x11vnc as a service on port 5900."""
    print("\n=== Configuring x11vnc ===")
    
    # Create password file
    out, err = ssh_exec(client, "mkdir -p /home/pi/.vnc && x11vnc -storepasswd arca /home/pi/.vnc/x11vnc_passwd 2>&1")
    print(f"Password: {out or err}")
    
    # Stop the broken service first
    ssh_exec(client, "sudo systemctl stop x11vnc-arca 2>/dev/null")
    
    # Disable RealVNC to free port 5900
    print("Disabling RealVNC server...")
    ssh_exec(client, "sudo systemctl stop vncserver-x11-serviced 2>/dev/null; sudo systemctl disable vncserver-x11-serviced 2>/dev/null")
    
    # Create systemd service
    service = r"""[Unit]
Description=x11vnc VNC Server for Ludbox Linux
After=multi-user.target graphical.target
Wants=graphical.target

[Service]
Type=simple
User=root
ExecStart=/usr/bin/x11vnc -display :0 -rfbport 5900 -rfbauth /home/pi/.vnc/x11vnc_passwd -shared -forever -noxdamage -repeat -nap -wait 50 -defer 50 -auth /var/run/lightdm/root/:0
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical.target
"""
    
    # Write and enable
    cmd = f"cat > /tmp/x11vnc-arca.service << 'SERVICEEOF'\n{service}\nSERVICEEOF\nsudo cp /tmp/x11vnc-arca.service /etc/systemd/system/x11vnc-arca.service && sudo systemctl daemon-reload && sudo systemctl enable x11vnc-arca && sudo systemctl restart x11vnc-arca && echo 'Service started'"
    out, err = ssh_exec(client, cmd)
    print(out)
    if err:
        print(f"  ({err[:200]})")
    
    # Wait and verify
    time.sleep(3)
    verify(client)

def verify(client):
    """Verify x11vnc is running."""
    print("\n=== Verification ===")
    out, _ = ssh_exec(client, "sudo systemctl status x11vnc-arca --no-pager 2>&1 | head -12")
    print(out)
    
    out, _ = ssh_exec(client, "ss -tlnp | grep 5900 || echo 'Port 5900 NOT listening'")
    print(f"\nPort check: {out}")
    
    out, _ = ssh_exec(client, "ps aux | grep x11vnc | grep -v grep")
    print(f"Process: {out}")

def setup_ssh_key(client):
    """Set up SSH key auth from Arca to RPi."""
    print("=== SSH Key Setup ===")
    # This would be called from the Arca side
    pass

def inspect(client):
    """Inspect the RPi display environment and VNC configuration."""
    checks = [
        ("OS",               "cat /etc/os-release | head -4"),
        ("Display env",      "echo DISPLAY=$DISPLAY XDG_SESSION_TYPE=$XDG_SESSION_TYPE"),
        ("Display processes","ps aux | grep -v grep | grep -iE 'Xorg|xwayland|wayland|vnc|chromium' | head -20"),
        ("VNC packages",     "dpkg -l 2>/dev/null | grep -iE 'vnc|x11vnc|wayvnc|tigervnc' || echo 'none'"),
        ("x11vnc available", "which x11vnc 2>/dev/null || echo 'not installed'"),
        ("Ports in use",     "ss -tlnp 2>/dev/null | grep -E ':59[0-9][0-9]' || echo 'none'"),
        ("RealVNC config",   "sudo cat /root/.vnc/config.d/vncserver-x11 2>/dev/null | head -10 || echo 'not readable'"),
        ("Screen resolution","DISPLAY=:0 xrandr 2>/dev/null | head -5 || echo 'xrandr unavailable'"),
        ("X displays",       "ls /tmp/.X11-unix/ 2>/dev/null || echo 'none'"),
        ("x11vnc service",   "sudo systemctl status x11vnc-arca --no-pager 2>&1 | head -8"),
    ]

    for name, cmd in checks:
        out, err = ssh_exec(client, cmd)
        print(f"=== {name} ===")
        if out:
            print(out)
        if err and err != out:
            print(f"  (stderr: {err[:200]})")
        print()

if __name__ == "__main__":
    main()
