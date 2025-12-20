#ifndef BLYNK_CONFIG_H
#define BLYNK_CONFIG_H

// Blynk credentials
#define BLYNK_TEMPLATE_ID "TMPL3FSgr7kt0"
#define BLYNK_AUTH_TOKEN "9tFVYgGkUNSnPqiX_q0fvni_lm8QnX-T"

// Blynk Virtual Pin Assignments
#define BLYNK_PIN_TEMPERATURE    0  // V0: Temperature (°C)
#define BLYNK_PIN_OXYGEN         1  // V1: Oxygen (mg/L)
#define BLYNK_PIN_PH             2  // V2: pH level
#define BLYNK_PIN_FEEDING        3  // V3: Hours since feeding
#define BLYNK_PIN_CLEANING       4  // V4: Days since cleaning
#define BLYNK_PIN_MOOD           5  // V5: Fish mood (HAPPY/SAD)
#define BLYNK_PIN_AI_ADVICE      6  // V6: AI advice text

// Blynk server
#define BLYNK_SERVER "blynk.cloud"
#define BLYNK_PORT 80

#endif // BLYNK_CONFIG_H
