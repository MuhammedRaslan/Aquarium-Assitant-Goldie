# Google Gemini AI Integration Setup

## Step 1: Get Free Gemini API Key

1. Go to **Google AI Studio**: https://aistudio.google.com/
2. Sign in with your Google account
3. Click **"Get API Key"** → **"Create API Key"**
4. Copy the API key (starts with `AIza...`)

## Step 2: Configure WiFi and API Key

Edit `main/wifi_config.h`:

```c
#define WIFI_SSID "YOUR_WIFI_NETWORK_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define GEMINI_API_KEY "AIzaSy..."  // Paste your API key here
```

## Step 3: Build and Flash

```bash
idf.py build flash monitor
```

## How It Works

**On Startup:**
- ESP32 connects to WiFi
- Shows "WiFi connected" or falls back to offline mode

**When Parameters Change:**
- AI Assistant queries Gemini API
- Shows "Consulting AI..." while waiting
- Displays AI-generated advice (personalized, context-aware)

**Features:**
- Real AI analysis (not just rules)
- Natural language advice
- Considers all 5 parameters together
- Free tier: 15 requests/minute, 1500/day

**Offline Mode:**
- If WiFi fails, shows warning
- System still works normally (just no AI advice)

## Free Tier Limits

- **15 requests per minute**
- **1500 requests per day**

For aquarium monitoring (checking every few minutes), you'll use ~500 requests/day max - well within limits!

## Troubleshooting

**"WiFi connection failed":**
- Check SSID/password in `wifi_config.h`
- Make sure ESP32 is in WiFi range

**"AI offline":**
- Verify API key is correct
- Check internet connection
- Ensure Google AI Studio account is active

**API errors:**
- Check serial monitor for HTTP status codes
- Verify Gemini API quota not exceeded
- Make sure API key has proper permissions
