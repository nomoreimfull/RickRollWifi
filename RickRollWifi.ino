// ===========================================================================
//  WiFi SSID advertiser + OLED animation, with lyric text overlaid on frames,
//  plus a persistent "clients connected" score in the bottom-right corner.
//
//  Frame data (F1..F20 arrays + the frames[] table + numFrames) now lives in
//  frames.h. This file only contains program logic.
// ===========================================================================

#include <ESP8266WiFi.h>
extern "C" {
  #include "user_interface.h"
}
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

#include "frames.h"   // provides FR000..FR099, frames[], and numFrames

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---- animation timing ----
// Fixed framerate via a micros() period timer (doesn't drift with draw time).
// 1,000,000 us / 29.9 fps = 33445 us per frame.
const uint32_t FRAME_PERIOD_US = 33445;   // ~29.9 fps
unsigned long  lastFrameUs = 0;
int currentFrame = 0;

// ---- lyrics ----
const char* lyrics[] = {
  "Were no strangers to love",
  "You know the rules and so do I",
  "A full commitments",
  "what Im thinking of",
  "You wouldnt get this",
  "from any other guy",
  "I just wanna tell you",
  "how Im feeling",
  "Gotta make you understand",
  "Never gonna give you up",
  "Never gonna let you down",
  "Never gonna run around",
  "and desert you",
  "Never gonna make you cry",
  "Never gonna say goodbye",
  "Never gonna tell a lie",
  "and hurt you",
  "Weve known each other",
  "for so long",
  "Your hearts been aching",
  "but youre too shy to say it",
  "Inside we both know",
  "whats been going on",
  "We know the game",
  "and were gonna play it",
  "And if you ask me how Im feeling",
  "Dont tell me",
  "youre too blind to see",
  "Never gonna give you up",
  "Never gonna let you down",
  "Never gonna run around",
  "and desert you",
  "Never gonna make you cry",
  "Never gonna say goodbye",
  "Never gonna tell a lie",
  "and hurt you",
};
const int numLines = sizeof(lyrics) / sizeof(lyrics[0]);

int currentLine = 0;
unsigned long lastLineChange = 0;
const unsigned long lineInterval = 20000; // 20 s

// ---------------------------------------------------------------------------
//  Persistent score: number of stations that have associated with the AP.
//  Stored in emulated EEPROM (flash) so it survives power cycles.
// ---------------------------------------------------------------------------
#define EEPROM_SIZE  64
#define SCORE_MAGIC  0xC0DE      // sentinel so we can detect blank/first-boot flash

struct ScoreStore {
  uint16_t magic;
  uint32_t score;
};

volatile uint32_t score = 0;     // touched by the WiFi event callback
volatile bool scoreDirty = false;
WiFiEventHandler stationConnectedHandler;

void saveScore() {
  ScoreStore s;
  s.magic = SCORE_MAGIC;
  s.score = score;
  EEPROM.put(0, s);
  EEPROM.commit();
}

void loadScore() {
  EEPROM.begin(EEPROM_SIZE);
  ScoreStore s;
  EEPROM.get(0, s);
  if (s.magic == SCORE_MAGIC) {
    score = s.score;             // restore previous count
  } else {
    score = 0;                   // first boot / blank flash
    saveScore();
  }
}

// Fires the instant a device associates with our open AP. This is as far as a
// client gets before the (non-functional) connection drops, which is exactly
// what we want to count. Keep this callback tiny: just bump the counter and
// flag it. The actual flash write happens in loop(), NOT here -- writing flash
// from inside a WiFi/SDK callback can crash the ESP8266.
void onStationConnected(const WiFiEventSoftAPModeStationConnected& evt) {
  score++;
  scoreDirty = true;
}

// ---------------------------------------------------------------------------
//  WiFi: advertise the current lyric line as an open AP SSID
// ---------------------------------------------------------------------------
void setBeacon(const char* ssid) {
  WiFi.softAPdisconnect(true);
  delay(20);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid);
}

// ---------------------------------------------------------------------------
//  Print the lyric one word per line, starting at (3, 3), top-left, no box,
//  black text. Short words (1 letter by default) don't get their own line:
//  they attach to the PREVIOUS word if the combined line still fits under
//  LINE_MAX_CHARS, otherwise to the NEXT word if there is one. Overflow off
//  the bottom of the screen is fine.
// ---------------------------------------------------------------------------
void overlayText(const char* text) {
  display.setTextSize(1);
  display.setTextWrap(false);
  display.setTextColor(SSD1306_BLACK);

  const int startX = 3, startY = 3;
  const int SHORT_WORD_MAX_LEN = 1;   // words this short get merged with a neighbor
  const int LINE_MAX_CHARS     = 12;  // only merge onto previous if it still fits

  // 1) split into words
  String words[16];
  int wordCount = 0;
  String src = String(text);
  int start = 0;
  while (start <= (int)src.length() && wordCount < 16) {
    int sp = src.indexOf(' ', start);
    String w = (sp == -1) ? src.substring(start) : src.substring(start, sp);
    if (w.length() > 0) words[wordCount++] = w;
    if (sp == -1) break;
    start = sp + 1;
  }

  // 2) build display lines, merging short words onto a neighbor
  String lines[16];
  int lineCount = 0;
  int i = 0;
  while (i < wordCount && lineCount < 16) {
    String w = words[i];
    bool isShort = ((int)w.length() <= SHORT_WORD_MAX_LEN);

    if (isShort && lineCount > 0 &&
        (int)(lines[lineCount - 1].length() + 1 + w.length()) <= LINE_MAX_CHARS) {
      lines[lineCount - 1] = lines[lineCount - 1] + " " + w;   // attach to previous
      i += 1;
    } else if (isShort && i + 1 < wordCount) {
      lines[lineCount++] = w + " " + words[i + 1];             // attach to next
      i += 2;
    } else {
      lines[lineCount++] = w;                                  // own line
      i += 1;
    }
  }

  // 3) draw, one entry per line
  for (int k = 0; k < lineCount; k++) {
    display.setCursor(startX, startY + k * 8);
    display.print(lines[k]);
  }
}

// ---------------------------------------------------------------------------
//  Score in the bottom-right corner: size-2 font, black body, white outline.
//  Always shown, including "0". The white outline makes the black digits
//  legible whether they fall over a dark or light part of the animation.
// ---------------------------------------------------------------------------
void drawScore() {
  uint32_t v = score;                 // snapshot the volatile
  String s = String(v);

  display.setTextSize(2);
  display.setTextWrap(false);

  int16_t w = (int16_t)s.length() * 12;     // 6px advance * size 2
  int16_t x = SCREEN_WIDTH  - w - 2;         // small right margin
  int16_t y = SCREEN_HEIGHT - 16 - 1;        // 16px tall + 1px bottom margin
  if (x < 1) x = 1;

  // White outline: draw the string in white at the 8 surrounding offsets.
  display.setTextColor(SSD1306_WHITE);
  for (int8_t dx = -1; dx <= 1; dx++) {
    for (int8_t dy = -1; dy <= 1; dy++) {
      if (dx == 0 && dy == 0) continue;
      display.setCursor(x + dx, y + dy);
      display.print(s);
    }
  }

  // Black body on top.
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(x, y);
  display.print(s);

  display.setTextSize(1);             // restore default size
}

void advanceLine() {
  const char* line = lyrics[currentLine];
  setBeacon(line);
  currentLine = (currentLine + 1) % numLines;
}

void setup() {
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;);
  }
  Wire.setClock(400000L);             // fast I2C so a full-frame push fits in ~33ms

  loadScore();                        // restore the counter before anything else

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  // Register the connect counter. The handler object is global so it stays
  // alive (and registered) across the softAP restarts in setBeacon().
  stationConnectedHandler = WiFi.onSoftAPModeStationConnected(&onStationConnected);

  advanceLine();                      // set first SSID immediately
  lastLineChange = millis();
  lastFrameUs = micros();
}

void loop() {
  // Change the advertised SSID every 20 s, independent of frame rate.
  if (millis() - lastLineChange >= lineInterval) {
    advanceLine();
    lastLineChange = millis();
  }

  // Persist the score if a client connected since last loop. Done here (not in
  // the callback) so the flash write happens in a safe context.
  if (scoreDirty) {
    scoreDirty = false;
    saveScore();
  }

  // Draw a new frame only when the fixed period has elapsed (~29.9 fps).
  if ((unsigned long)(micros() - lastFrameUs) >= FRAME_PERIOD_US) {
    lastFrameUs += FRAME_PERIOD_US;            // advance by exact period (no drift)
    // If we've fallen more than one frame behind, resync so we don't burst.
    if ((unsigned long)(micros() - lastFrameUs) >= FRAME_PERIOD_US) {
      lastFrameUs = micros();
    }

    display.clearDisplay();
    display.drawBitmap(0, 0, frames[currentFrame], 128, 64, 1);

    int shownLine = (currentLine + numLines - 1) % numLines;
    overlayText(lyrics[shownLine]);

    drawScore();                               // drawn last so it sits on top

    display.display();

    currentFrame = (currentFrame + 1) % numFrames;
  }
}