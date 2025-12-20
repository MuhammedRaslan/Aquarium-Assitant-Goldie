# Aquarium Mood System - Parameter Scoring

## Overview

The aquarium animation mood is determined by 5 parameters, each contributing a score from **-2 to +2** points:

1. **Temperature** (°C)
2. **Oxygen Level** (mg/L)
3. **pH Level** (0-14)
4. **Feed Timing** (time since last feed)
5. **Tank Cleanliness** (time since last cleaning)

**Total Score Range**: -10 to +10

---

## Mood Categories

| Total Score | Mood Category | Animation Frames | Description |
|-------------|---------------|------------------|-------------|
| **5 to 10** | **HAPPY** 😊 | Frames 1-8 | Most parameters in ideal range |
| **0 to 4** | **SAD** 😞 | Frames 9-16 | Some parameters acceptable, some concerning |
| **-10 to -1** | **ANGRY** 😠 | Frames 17-24 | Multiple parameters out of range |

---

## Parameter Scoring Tables

### 1. Temperature (°C)

| Temperature Range | Score | Status |
|-------------------|-------|--------|
| 24.0 - 26.0°C | **+2** | Perfect ✅ |
| 22.0 - 23.9°C or 26.1 - 28.0°C | **+1** | Acceptable ⚠️ |
| 20.0 - 21.9°C or 28.1 - 30.0°C | **-1** | Concerning ⚠️⚠️ |
| < 20.0°C or > 30.0°C | **-2** | Critical ❌ |

### 2. Oxygen Level (mg/L)

| Oxygen Range | Score | Status |
|--------------|-------|--------|
| 7.0 - 9.0 mg/L | **+2** | Perfect ✅ |
| 6.0 - 6.9 mg/L or 9.1 - 10.0 mg/L | **+1** | Acceptable ⚠️ |
| 5.0 - 5.9 mg/L or 10.1 - 11.0 mg/L | **-1** | Concerning ⚠️⚠️ |
| < 5.0 mg/L or > 11.0 mg/L | **-2** | Critical ❌ |

### 3. pH Level

| pH Range | Score | Status |
|----------|-------|--------|
| 6.8 - 7.5 | **+2** | Perfect ✅ |
| 6.5 - 6.7 or 7.6 - 8.0 | **+1** | Acceptable ⚠️ |
| 6.0 - 6.4 or 8.1 - 8.5 | **-1** | Concerning ⚠️⚠️ |
| < 6.0 or > 8.5 | **-2** | Critical ❌ |

### 4. Feed Timing (Time Since Last Feed)

| Time Since Feed | Score | Status |
|-----------------|-------|--------|
| ≤ 8 hours | **+2** | Well fed ✅ |
| 8 - 12 hours | **+1** | Getting hungry ⚠️ |
| 12 - 24 hours | **-1** | Very hungry ⚠️⚠️ |
| ≥ 24 hours | **-2** | Starving ❌ |

### 5. Tank Cleanliness (Time Since Last Cleaning)

| Time Since Cleaning | Score | Status |
|---------------------|-------|--------|
| ≤ 7 days | **+2** | Clean tank ✅ |
| 7 - 14 days | **+1** | Needs cleaning soon ⚠️ |
| 14 - 21 days | **-1** | Dirty tank ⚠️⚠️ |
| ≥ 21 days | **-2** | Very dirty ❌ |

---

## Example Mood Calculations

### Example 1: Happy Fish 😊
- Temperature: 25°C → **+2**
- Oxygen: 8.0 mg/L → **+2**
- pH: 7.2 → **+2**
- Last fed: 6 hours ago → **+2**
- Last cleaned: 5 days ago → **+2**
- **Total: +10** → **HAPPY** (Frames 1-8)

### Example 2: Sad Fish 😞
- Temperature: 23°C → **+1** (acceptable)
- Oxygen: 6.5 mg/L → **+1** (acceptable)
- pH: 7.8 → **+1** (acceptable)
- Last fed: 10 hours ago → **+1** (getting hungry)
- Last cleaned: 12 days ago → **+1** (needs cleaning)
- **Total: +5** → **HAPPY** (Frames 1-8)

Actually still happy! Let's try:
- Temperature: 28°C → **+1**
- Oxygen: 6.2 mg/L → **+1**
- pH: 8.0 → **+1**
- Last fed: 15 hours ago → **-1** (very hungry)
- Last cleaned: 9 days ago → **+1**
- **Total: +3** → **SAD** (Frames 9-16)

### Example 3: Angry Fish 😠
- Temperature: 31°C → **-2** (too hot!)
- Oxygen: 4.5 mg/L → **-2** (critically low)
- pH: 8.7 → **-2** (too alkaline)
- Last fed: 26 hours ago → **-2** (starving)
- Last cleaned: 8 days ago → **+1** (still okay)
- **Total: -7** → **ANGRY** (Frames 17-24)

---

## Code Integration

### Update Functions

```cpp
// Update temperature (Gauge 1)
dashboard_update_sensor1(25.5f);  // Temperature in °C

// Update oxygen (Gauge 2)
dashboard_update_sensor2(8.2f);   // Oxygen in mg/L

// Update pH level
dashboard_update_ph(7.3f);        // pH value

// Log feeding (automatically updates timestamp)
// Press Feed button in UI

// Log tank cleaning (automatically updates timestamp)
// Press Water button in UI
```

### Manual Mood Override

```cpp
// Force specific mood
dashboard_set_animation_category(0);  // Happy
dashboard_set_animation_category(1);  // Sad
dashboard_set_animation_category(2);  // Angry

// Check current mood
uint8_t current_mood = dashboard_get_animation_category();
```

---

## Automatic Mood Updates

The system automatically re-evaluates mood when:
- ✅ Temperature changes (`dashboard_update_sensor1()`)
- ✅ Oxygen level changes (`dashboard_update_sensor2()`)
- ✅ pH level changes (`dashboard_update_ph()`)
- ✅ Fish is fed (Feed button pressed)
- ✅ Tank is cleaned (Water button pressed)

The mood evaluation logs detailed scores to the serial monitor:
```
Mood Scores: Temp=+2, O2=+2, pH=+2, Feed=+1, Clean=+2 | Total=+9
Mood: HAPPY
```

---

## Customization

To adjust ranges, modify these constants in `dashboard.cpp`:

```cpp
// Temperature ranges (°C)
#define TEMP_MIN_IDEAL 24.0f
#define TEMP_MAX_IDEAL 26.0f
#define TEMP_MIN_ACCEPTABLE 22.0f
#define TEMP_MAX_ACCEPTABLE 28.0f

// Oxygen ranges (mg/L)
#define OXYGEN_MIN_IDEAL 7.0f
#define OXYGEN_MAX_IDEAL 9.0f
#define OXYGEN_MIN_ACCEPTABLE 6.0f
#define OXYGEN_MAX_ACCEPTABLE 10.0f

// pH ranges
#define PH_MIN_IDEAL 6.8f
#define PH_MAX_IDEAL 7.5f
#define PH_MIN_ACCEPTABLE 6.5f
#define PH_MAX_ACCEPTABLE 8.0f

// Feed intervals (seconds)
#define FEED_INTERVAL_IDEAL 28800      // 8 hours
#define FEED_INTERVAL_MAX 43200        // 12 hours
#define FEED_INTERVAL_CRITICAL 86400   // 24 hours

// Cleaning intervals (seconds)
#define CLEAN_INTERVAL_IDEAL 604800    // 7 days
#define CLEAN_INTERVAL_MAX 1209600     // 14 days
#define CLEAN_INTERVAL_CRITICAL 1814400 // 21 days
```
