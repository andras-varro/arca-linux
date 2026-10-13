# Arca Browser Modifications Guide

> ## ⚠️ Obsolete
>
> These games run inside the vendor's X11 → Jetty → WebKit browser stack. That entire stack is
> **disabled and deleted** by `../commissioning/provision.sh` on every commissioned device, so nothing here
> works on a current unit. It is kept only as a historical record of the first approach.
>
> The native replacement is [`../native_apps/`](../native_apps/) — see
> `native_apps/brick_breaker` for the direct successor to the HTML brick breaker.


> **Complete guide for browser-based game modifications and LED control implementation**

## Table of Contents

1. [Overview](#overview)
2. [Brick Breaker Game](#brick-breaker-game)
3. [LED Control System](#led-control-system)
4. [Deployment Process](#deployment-process)
5. [Modification Status](#modification-status)
6. [Troubleshooting](#troubleshooting)

---

## Overview

This guide covers browser-based modifications to the Steelcase RoomWizard, specifically the implementation of a brick breaker game with LED feedback. The modifications replace the original room booking interface while maintaining the existing Jetty/Java infrastructure.

### What Was Implemented

- **Brick Breaker Game:** Full-featured HTML5 game with touch controls
- **LED Control Backend:** Java servlet for hardware LED control
- **Screen Blanking:** Automatic screen blanking after 5 minutes of inactivity
- **High Score Persistence:** localStorage-based score tracking

### Files Modified

1. **`index.html`** - Main game file (replaced original interface)
2. **`index.html.template`** - Template file (prevents boot-time regeneration)
3. **`LEDControlServlet.java`** - Backend LED control servlet
4. **`web.xml`** - Servlet registration

---

## Brick Breaker Game

### Game Features

#### Core Gameplay
- ✅ **5 Lives System:** Start with 5 lives, lose one when ball is lost
- ✅ **15 Progressive Levels:** Increasing difficulty with different brick patterns
- ✅ **Level Looping:** After level 15, returns to level 1
- ✅ **High Score Tracking:** Persists across sessions using localStorage
- ✅ **Score System:** Points based on brick health and level multiplier

#### Touch Controls
- ✅ **Paddle Movement:** Touch and drag anywhere on screen to move paddle
- ✅ **Ball Launch:** Tap screen to launch ball from paddle
- ✅ **Pause Button:** Top-left corner (fades after 3 seconds)
- ✅ **Restart Button:** Top-right corner (fades after 3 seconds)
- ✅ **Intuitive Interface:** No learning curve required

#### Power-Ups (15% spawn chance)
1. **Expand Paddle** (Green 'E'): Increases paddle width by 50% for 10 seconds
2. **Shrink Paddle** (Red 'S'): Decreases paddle width by 30% for 10 seconds
3. **Multi-Ball** (Blue 'M'): Spawns 2 additional balls
4. **Slow Ball** (Yellow 'L'): Reduces ball speed by 40% for 15 seconds
5. **Extra Life** (Pink '+'): Grants an additional life

#### Visual Features
- ✅ **Smooth 60fps Animation:** Optimized for 800MHz CPU
- ✅ **Particle Effects:** Minimal particles on brick destruction
- ✅ **Color-Coded Bricks:** Green (1 hit), Yellow (2 hits), Orange (3 hits), Red (4 hits)
- ✅ **Glowing Effects:** Paddle and ball have subtle glow
- ✅ **Responsive Design:** Adapts to any screen resolution
- ✅ **Professional UI:** Clean HUD with score, level, and lives display

#### Screen States
- ✅ **Start Screen:** Instructions and high score display
- ✅ **Playing:** Active gameplay with HUD
- ✅ **Paused:** Overlay with resume/restart options
- ✅ **Level Complete:** Bonus points and auto-advance
- ✅ **Game Over:** Final score and high score comparison

#### Performance Optimizations
- ✅ **Object Pooling:** Reuses ball and particle objects
- ✅ **Efficient Collision Detection:** AABB with early exit
- ✅ **Fixed Time Step Physics:** Consistent gameplay at any framerate
- ✅ **Minimal DOM Manipulation:** Single canvas element
- ✅ **Optimized Rendering:** Only draws active objects
- ✅ **Low Memory Footprint:** Suitable for embedded hardware

#### Screen Blanking (Burn-in Prevention)
- ✅ **5-Minute Idle Timeout:** Automatically blanks screen after inactivity
- ✅ **Activity Detection:** Tracks touch events, ball movement, brick destruction
- ✅ **Touch to Wake:** Any touch unblanks the screen
- ✅ **Seamless Resume:** Game state preserved during blanking

### Level Progression

| Level | Rows | Max Brick Health | Ball Speed | Difficulty |
|-------|------|------------------|------------|------------|
| 1     | 3    | 1                | 1.0x       | Easy       |
| 2-3   | 4    | 2                | 1.1-1.2x   | Easy       |
| 4-6   | 5    | 3                | 1.2-1.3x   | Medium     |
| 7-10  | 6    | 3-4              | 1.4-1.5x   | Medium     |
| 11-15 | 6    | 4                | 1.6-1.8x   | Hard       |

### Color Scheme

- **Background:** Dark blue (#0a0e27)
- **Paddle:** Cyan (#00ffff) with glow
- **Ball:** White (#ffffff) with glow
- **Bricks:** 
  - 1 hit: Green (#00ff00)
  - 2 hits: Yellow (#ffff00)
  - 3 hits: Orange (#ff8800)
  - 4 hits: Red (#ff0000)
- **UI Text:** White with shadow

### How to Play

1. **Power on** the Steelcase RoomWizard device
2. Wait for the system to boot (X11 → Jetty → Browser)
3. The **Brick Breaker start screen** will appear
4. Touch the **START** button to begin
5. **Move paddle:** Touch and drag anywhere on screen
6. **Launch ball:** Tap screen when ball is on paddle
7. **Destroy bricks:** Bounce ball off paddle to hit bricks
8. **Collect power-ups:** Catch falling power-ups with paddle
9. **Complete level:** Destroy all bricks to advance

---

## LED Control System

### Overview

The LED control system allows the JavaScript game to control the Steelcase RoomWizard's red and green LEDs through a Java servlet backend.

### Architecture

```
JavaScript Game → HTTP GET → Jetty Server → LEDControlServlet
                                                ↓
                                    /sys/class/leds/red_led/brightness
                                    /sys/class/leds/green_led/brightness
                                                ↓
                                          JSON Response
```

### API Endpoint

**URL:** `http://localhost/frontpanel/led`

**Method:** GET

**Parameters:**
- `color` (required): "red" or "green"
- `brightness` (required): 0-100

**Example Requests:**
```bash
# Turn on green LED at full brightness
curl "http://localhost/frontpanel/led?color=green&brightness=100"

# Set red LED to 50% brightness
curl "http://localhost/frontpanel/led?color=red&brightness=50"

# Turn off green LED
curl "http://localhost/frontpanel/led?color=green&brightness=0"
```

**Response Format:**
```json
{
  "status": "success",
  "color": "green",
  "brightness": 100
}
```

**Error Response:**
```json
{
  "status": "error",
  "message": "Invalid color. Must be 'red' or 'green'"
}
```

### LED Events in Game

The game automatically triggers LED feedback for:

| Event | LED | Pattern |
|-------|-----|---------|
| Level Complete | Green | Flash 3 times |
| Power-Up Collected | Green | Brief pulse (200ms) |
| Ball Lost | Red | Flash 2 times |
| Game Over | Red | Flash 5 times |

### Implementation

#### Java Servlet (LEDControlServlet.java)

```java
import javax.servlet.*;
import javax.servlet.http.*;
import java.io.*;

public class LEDControlServlet extends HttpServlet {
    private static final String RED_LED = "/sys/class/leds/red_led/brightness";
    private static final String GREEN_LED = "/sys/class/leds/green_led/brightness";
    
    @Override
    protected void doGet(HttpServletRequest request, HttpServletResponse response) 
            throws ServletException, IOException {
        String color = request.getParameter("color");
        String brightness = request.getParameter("brightness");
        
        response.setContentType("application/json");
        PrintWriter out = response.getWriter();
        
        try {
            if (color == null || brightness == null) {
                out.println("{\"status\":\"error\",\"message\":\"Missing parameters\"}");
                return;
            }
            
            int value = Integer.parseInt(brightness);
            value = Math.max(0, Math.min(100, value));
            
            String ledPath = color.equals("red") ? RED_LED : 
                           color.equals("green") ? GREEN_LED : null;
            
            if (ledPath == null) {
                out.println("{\"status\":\"error\",\"message\":\"Invalid color\"}");
                return;
            }
            
            try (FileWriter writer = new FileWriter(ledPath)) {
                writer.write(String.valueOf(value));
            }
            
            out.println("{\"status\":\"success\",\"color\":\"" + color + 
                       "\",\"brightness\":" + value + "}");
        } catch (Exception e) {
            out.println("{\"status\":\"error\",\"message\":\"" + 
                       e.getMessage().replace("\"", "'") + "\"}");
        }
    }
}
```

#### Servlet Registration (web.xml)

```xml
<servlet>
    <servlet-name>LEDControl</servlet-name>
    <servlet-class>LEDControlServlet</servlet-class>
</servlet>
<servlet-mapping>
    <servlet-name>LEDControl</servlet-name>
    <url-pattern>/led</url-pattern>
</servlet-mapping>
```

#### JavaScript LED Controller

```javascript
const LEDController = {
    available: true,
    
    async setLED(color, brightness) {
        try {
            const response = await fetch(`/frontpanel/led?color=${color}&brightness=${brightness}`);
            const result = await response.json();
            if (result.status !== 'success') {
                console.log('LED control failed:', result.message);
            }
        } catch (e) {
            console.log('LED control error:', e);
        }
    },
    
    async flash(color, times = 3) {
        for (let i = 0; i < times; i++) {
            await this.setLED(color, 100);
            await new Promise(resolve => setTimeout(resolve, 100));
            await this.setLED(color, 0);
            await new Promise(resolve => setTimeout(resolve, 100));
        }
    },
    
    async levelComplete() {
        await this.flash('green', 3);
    },
    
    async powerUpCollected() {
        await this.setLED('green', 100);
        setTimeout(() => this.setLED('green', 0), 200);
    },
    
    async ballLost() {
        await this.flash('red', 2);
    },
    
    async gameOver() {
        await this.flash('red', 5);
    }
};
```

---

## Deployment Process

### Prerequisites

- **Backup:** Complete backup of original SD card (CRITICAL!)
- **Java Development Kit:** JDK 8 or compatible for compiling servlet
- **SD Card Access:** Ability to mount and modify SD card partitions
- **Serial Console** (recommended): For debugging if boot fails

### Step 1: Compile the LED Control Servlet

#### Option A: Compile on Development Machine (Recommended)

```bash
# Navigate to the servlet directory
cd "partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes"

# Compile the servlet (requires servlet-api.jar from Jetty)
javac -cp "../../lib/servlet-api-3.1.jar" LEDControlServlet.java

# Verify compilation
ls -la LEDControlServlet.class
```

#### Option B: Compile on Device After Deployment

```bash
# SSH into device (if network is configured)
ssh root@<device-ip>

# Navigate to servlet directory
cd /opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes

# Compile using device's Java
/opt/openjre-8/bin/javac -cp "../../lib/servlet-api-3.1.jar" LEDControlServlet.java

# Restart Jetty
/etc/init.d/webserver restart
```

### Step 2: Prepare the Root Filesystem Partition

#### Mount the Partition Image

```bash
# Create mount point
mkdir -p /tmp/rootfs_mount

# Mount the root filesystem image
sudo mount -o loop partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c.img /tmp/rootfs_mount

# Or if it's a directory (already extracted):
# The partition is already extracted as a directory, so we can work directly
```

#### Verify Modified Files

```bash
# Check that all modified files are present
ls -la "partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/pages/index.html"
ls -la "partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes/LEDControlServlet.class"
ls -la "partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/web.xml"
```

### Step 3: Create/Update Partition Image

If working with extracted directories, you need to create a partition image:

```bash
# Calculate required size (add 10% overhead)
SIZE=$(du -sb partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c | awk '{print int($1 * 1.1)}')

# Create ext3 filesystem image
dd if=/dev/zero of=sd_rootfs_part.img bs=1 count=0 seek=$SIZE
mkfs.ext3 sd_rootfs_part.img

# Mount and copy files
mkdir -p /tmp/new_rootfs
sudo mount -o loop sd_rootfs_part.img /tmp/new_rootfs
sudo cp -a partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/* /tmp/new_rootfs/
sudo umount /tmp/new_rootfs
```

### Step 4: Regenerate MD5 Checksums

**CRITICAL:** All modified partition images must have updated MD5 checksums or the device will refuse to boot.

```bash
# Generate MD5 for root filesystem
md5sum sd_rootfs_part.img > sd_rootfs_part.img.md5

# Verify the MD5 file was created
cat sd_rootfs_part.img.md5
```

### Step 5: Prepare SD Card

#### Option A: Direct SD Card Modification (Risky)

```bash
# Insert SD card and identify device (e.g., /dev/sdb)
lsblk

# Mount the root partition (usually partition 5 or 6)
sudo mount /dev/sdb6 /mnt/sdcard

# Copy modified files
sudo cp -a partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/* \
    /mnt/sdcard/opt/jetty-9-4-11/webapps/frontpanel/

# Unmount
sudo umount /mnt/sdcard
```

#### Option B: Full Image Write (Safer)

```bash
# Create complete SD card image with modifications
# This requires reassembling all partitions

# Write to SD card (CAUTION: This will erase the card!)
sudo dd if=modified_sdcard.img of=/dev/sdb bs=4M status=progress
sync
```

### Step 6: Boot the Device

1. **Insert modified SD card** into the Steelcase RoomWizard
2. **Power on** the device
3. **Monitor boot process** via serial console (if available)
4. **Wait for X11 and browser** to start (may take 60-90 seconds)

### Step 7: Verify Deployment

#### Check Game Loads

1. The device should boot to the brick breaker game
2. You should see the difficulty selection screen
3. Touch controls should work

#### Test LED Control

```bash
# SSH into device
ssh root@<device-ip>

# Test LEDs directly
echo 100 > /sys/class/leds/green_led/brightness
sleep 1
echo 0 > /sys/class/leds/green_led/brightness

# Test via HTTP endpoint
curl "http://localhost/frontpanel/led?color=green&brightness=100"
curl "http://localhost/frontpanel/led?color=red&brightness=50"
curl "http://localhost/frontpanel/led?color=green&brightness=0"
```

---

## Modification Status

### Current Status: ✅ READY FOR TESTING

Both `index.html` and `index.html.template` have been replaced in the extracted partition directory. The modification is complete and ready for:

1. **Testing on actual hardware** (if you have access to the Steelcase RoomWizard device)
2. **Repackaging the partition** (if needed for your deployment method)
3. **Writing to SD card** (following your deployment procedure)

### Files Modified

#### Modified Files (Windows)
- ✅ `bouncing-ball.html` - New bouncing ball application
- ✅ `partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/pages/index.html` - Replaced
- ✅ `partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/pages/index.html.template` - Replaced
- ✅ `partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/pages/index.html.original` - Backup
- ✅ `partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes/LEDControlServlet.java` - New
- ✅ `partitions/108a1490-8feb-4d0c-b3db-995dc5fc066c/opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/web.xml` - Modified

### Important Notes

1. **Template File:** The `index.html.template` was also replaced to prevent the boot-time script (`configure_jetty.sh`) from regenerating the original interface.

2. **Backup Preserved:** The original `index.html` was backed up as `index.html.original.html` for easy rollback if needed.

3. **No Other Changes:** No modifications were made to:
   - Init scripts
   - Boot sequence
   - Jetty configuration
   - X11 settings
   - Watchdog configuration

---

## Troubleshooting

### Device Won't Boot

**Symptoms:** Device resets repeatedly, never shows display

**Causes:**
1. MD5 checksum mismatch
2. Corrupted partition image
3. Watchdog timeout

**Solutions:**
```bash
# 1. Verify MD5 checksums
md5sum -c sd_rootfs_part.img.md5

# 2. Check control block
# The device may have incremented boot_tracker to failure mode (≥2)
# You may need to reset the control block or restore from backup

# 3. Restore original SD card backup
sudo dd if=original_backup.img of=/dev/sdb bs=4M status=progress
```

### Game Loads But LEDs Don't Work

**Symptoms:** Game works, but no LED feedback

**Diagnosis:**
```bash
# Check if servlet compiled
ls -la /opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/classes/LEDControlServlet.class

# Check servlet registration
grep -A5 "ledControlServlet" /opt/jetty-9-4-11/webapps/frontpanel/WEB-INF/web.xml

# Check Jetty logs for errors
tail -100 /var/log/jetty.log | grep -i error

# Test LED hardware directly
echo 100 > /sys/class/leds/red_led/brightness
```

**Solutions:**
1. Compile servlet if .class file is missing
2. Verify web.xml has correct servlet registration
3. Check file permissions (should be readable by jetty user)
4. Restart Jetty: `/etc/init.d/webserver restart`

### Servlet Compilation Errors

**Error:** `package javax.servlet does not exist`

**Solution:**
```bash
# Find servlet-api JAR
find /opt/jetty-9-4-11 -name "*servlet*.jar"

# Compile with correct classpath
javac -cp "/opt/jetty-9-4-11/lib/servlet-api-3.1.jar" LEDControlServlet.java
```

### Permission Denied on LED Control

**Error:** `Failed to control LED: Permission denied`

**Solution:**
```bash
# Check LED file permissions
ls -la /sys/class/leds/*/brightness

# Jetty may need permission to write to LED files
# Add jetty user to appropriate group or modify udev rules
chmod 666 /sys/class/leds/*/brightness
```

### Rollback Procedure

If anything goes wrong:

1. **Power off** the device
2. **Remove SD card**
3. **Restore original backup**:
   ```bash
   sudo dd if=original_backup.img of=/dev/sdb bs=4M status=progress
   sync
   ```
4. **Reinsert card** and power on

---

## Related Documentation

- [System Analysis](../SYSTEM_ANALYSIS.md) - Hardware and firmware analysis
 - Native C games development
- [Commissioning](../COMMISSIONING.md) - Device setup and commissioning
 - Current development status

---
