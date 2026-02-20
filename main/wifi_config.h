#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// WiFi credentials - UPDATE THESE WITH YOUR VALUES
#define WIFI_SSID "Muhammed's Phone"
#define WIFI_PASS "1234567888"

// Groq AI API configuration
#define GROQ_API_KEY "YOUR_API_KEY_HERE"
#define GROQ_API_URL "https://api.groq.com/openai/v1/chat/completions"

#endif // WIFI_CONFIG_H

/*curl "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent" \
  -H 'Content-Type: application/json' \
  -H 'X-goog-api-key: AIzaSyBc7K746LoUXyyUVscbVFWgjzJ5EtbwI6E' \
  -X POST \
  -d '{
    "contents": [
      {
        "parts": [
          {
            "text": "Explain how AI works in a few words"
          }
        ]
      }
    ]
  }'*/