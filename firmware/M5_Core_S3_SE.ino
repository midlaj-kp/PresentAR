#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <time.h>

using namespace websockets;

// =======================================================================
// Wi-Fi Credentials & WebSocket
const char* ssid = "XXX";
const char* password = "XXXXX";
WebsocketsServer wsServer;
WebsocketsClient activeClient;

// =======================================================================
// Tile Configuration
#define TILE_W 140
#define TILE_H 60
#define PADDING 8

// =======================================================================
// SD Card Pins and Audio Config
#define SD_SPI_SCK_PIN  36
#define SD_SPI_MISO_PIN 35
#define SD_SPI_MOSI_PIN 37
#define SD_SPI_CS_PIN   4

// Recording params
static constexpr uint32_t requested_sample_rate = 16000;
static constexpr size_t   record_length = 320;
static int16_t rec_buffer[record_length];
static uint32_t activeSampleRate = requested_sample_rate;

// =======================================================================
// State Management
enum ScreenState { SHOWING_PROJECTS, PRESENTING_MODE, SAVED_RECORDINGS };
ScreenState currentState = SHOWING_PROJECTS;

enum RecState { REC_IDLE, REC_RECORDING, REC_PAUSED };
RecState recState = REC_IDLE;

// =======================================================================
// Presentation & Pointer State
unsigned long lastTapTime = 0;
int currentPointerX = -1, currentPointerY = -1;
int prevPointerX = -1, prevPointerY = -1;
int pointerRadius = 15, pointerRingWidth = 5;
int zoomLevel = 1;
unsigned long blinkTimer = 0;
bool blinkOn = false;

// Swipe / press origins
static int pressStartX = -1;
static int pressStartY = -1;
static int lastSwipeX = -1;
static int lastSwipeY = -1;

int swipeStartX = -1, swipeStartY = -1;

// Recording control gesture state
unsigned long recHoldStart = 0;
bool recHoldActive = false;
bool recHoldHandled = false;

// Control lock (only blocks exiting PRESENTING_MODE)
bool controlsLocked = false;
bool lockHoldActive = false;
bool lockHoldHandled = false;
unsigned long lockHoldStart = 0;

// For scroll handling in zoom mode
static int lastScrollTouchX = -1;
static int lastScrollTouchY = -1;

// NEW: Flag to differentiate scroll gesture from other holds/swipes
bool pointerScrolling = false;

// =======================================================================
// Project & Tile State
struct TouchButton {
  int16_t x, y, w, h;
  String title;
  int id;
};
std::vector<TouchButton> projectButtons;
std::vector<String> projectTitles;
int projectListScroll = 0;
int projectListRowsPerPage = 0;
const int projectListCols = 2;
const int projectListTileW = TILE_W, projectListTileH = TILE_H;
const int projectListMargin = 12;
const int projectListStartY = 55;
int tileIndex = 0;
String currentProjectTitle = "";

// =======================================================================
// Recording
File audioFile;
#define MAX_RECORDS 3
struct RecordingMeta {
  String path;
  String displayName;
  String datetime;
  size_t bytes;
};
std::vector<RecordingMeta> recordFiles;
int selectedRecordingIdx = -1;

DynamicJsonDocument doc(4096);
static bool timeSynced = false;

// =======================================================================
// Playback
File playbackFile;
bool playbackActive = false;
bool playbackPaused = false;
static const size_t PLAYBACK_CHUNK = 1024;
uint8_t playbackBuf[PLAYBACK_CHUNK];
unsigned long lastPlaybackFeed = 0;
static const uint16_t PLAYBACK_FEED_INTERVAL_MS = 4;

// =======================================================================
// Sending (WAV)
bool sendingActive = false;
File sendingFile;
bool sendingHeaderSent = false;
uint32_t sendingSeq = 0;
static const size_t SEND_CHUNK_RAW = 1200;
uint8_t sendRawBuf[SEND_CHUNK_RAW];
String sendingFilenameBase;
size_t sendingFileSize = 0;
size_t sendingBytesSent = 0;
bool sendingSuccess = false;
unsigned long sendingSuccessTime = 0; // NEW: timestamp to auto-revert success state

// Forward declarations
void printCommand(const char* command, int value = -1);
void printPointerCommand(int x, int y);
void refreshRecordFiles();
void drawPresentationScreen();
void updateRecIndicator();
void handleWsMessage(WebsocketsMessage msg);
void simpleSlideChange(bool forward);
void centerPointer();
void enforceMaxRecords();
void drawLockIndicator();
void drawSavedRecordingsScreen();
void handlePlayback();
void stopPlayback();
void startPlayback(int idx);
void startSendWav(int idx);
void handleSending();
void stopSending();
void pausePlayback();
void resumePlayback();

// Pointer send helpers
void sendPointerJump(int x, int y);
void sendPointerScroll(int dx, int dy, int x, int y);
void sendLockState();
void sendPointerSelect(); // ADDED

// =======================================================================
// Base64 Encoder
String base64Encode(const uint8_t* data, size_t len) {
  static const char* table =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
    if (i + 2 < len) v |= data[i + 2];
    out += table[(v >> 18) & 0x3F];
    out += table[(v >> 12) & 0x3F];
    if (i + 1 < len) out += table[(v >> 6) & 0x3F]; else out += '=';
    if (i + 2 < len) out += table[v & 0x3F]; else out += '=';
  }
  return out;
}

// =======================================================================
// WAV Header
void buildWavHeader(uint8_t header[44], uint32_t dataSize, uint32_t sampleRate) {
  uint32_t chunkSize = 36 + dataSize;
  uint16_t audioFormat = 1;
  uint16_t numChannels = 1;
  uint32_t byteRate = sampleRate * numChannels * 2;
  uint16_t blockAlign = numChannels * 2;
  uint16_t bitsPerSample = 16;
  memcpy(header, "RIFF", 4);
  header[4] = chunkSize & 0xFF;
  header[5] = (chunkSize >> 8) & 0xFF;
  header[6] = (chunkSize >> 16) & 0xFF;
  header[7] = (chunkSize >> 24) & 0xFF;
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  uint32_t subchunk1Size = 16;
  header[16] = subchunk1Size & 0xFF;
  header[17] = (subchunk1Size >> 8) & 0xFF;
  header[18] = (subchunk1Size >> 16) & 0xFF;
  header[19] = (subchunk1Size >> 24) & 0xFF;
  header[20] = audioFormat & 0xFF;
  header[21] = (audioFormat >> 8) & 0xFF;
  header[22] = numChannels & 0xFF;
  header[23] = (numChannels >> 8) & 0xFF;
  header[24] = sampleRate & 0xFF;
  header[25] = (sampleRate >> 8) & 0xFF;
  header[26] = (sampleRate >> 16) & 0xFF;
  header[27] = (sampleRate >> 24) & 0xFF;
  header[28] = byteRate & 0xFF;
  header[29] = (byteRate >> 8) & 0xFF;
  header[30] = (byteRate >> 16) & 0xFF;
  header[31] = (byteRate >> 24) & 0xFF;
  header[32] = blockAlign & 0xFF;
  header[33] = (blockAlign >> 8) & 0xFF;
  header[34] = bitsPerSample & 0xFF;
  header[35] = (bitsPerSample >> 8) & 0xFF;
  memcpy(header + 36, "data", 4);
  header[40] = dataSize & 0xFF;
  header[41] = (dataSize >> 8) & 0xFF;
  header[42] = (dataSize >> 16) & 0xFF;
  header[43] = (dataSize >> 24) & 0xFF;
}

// =======================================================================
// Time helpers
void ensureTimeSynced() {
  if (timeSynced) return;
  for (int i = 0; i < 30; ++i) {
    time_t now = time(NULL);
    if (now > 1700000000) { timeSynced = true; break; }
    delay(200);
  }
}

String makeTimestampFilename() {
  ensureTimeSynced();
  time_t now = time(NULL);
  struct tm *tt = localtime(&now);
  char buf[40];
  if (tt) {
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tt);
  } else {
    uint32_t ms = millis();
    snprintf(buf, sizeof(buf), "UNSET_%lu", (unsigned long)ms);
  }
  String base = String(buf);
  String fname = "/records/" + base + ".raw";
  int counter = 1;
  while (SD.exists(fname)) {
    fname = "/records/" + base + "_" + String(counter) + ".raw";
    counter++;
  }
  return fname;
}

// =======================================================================
// Pointer helpers
void centerPointer() {
  currentPointerX = CoreS3.Display.width() / 2;
  currentPointerY = CoreS3.Display.height() / 2;
  prevPointerX = currentPointerX;
  prevPointerY = currentPointerY;
}

// =======================================================================
// UI basics
void displayMessage(const char* title, const char* message, uint16_t color) {
  CoreS3.Display.fillScreen(BLACK);
  CoreS3.Display.setTextColor(color, BLACK);
  CoreS3.Display.setTextDatum(middle_center);
  CoreS3.Display.drawString(title, CoreS3.Display.width()/2, CoreS3.Display.height()/2 - 20, 4);
  CoreS3.Display.drawString(message, CoreS3.Display.width()/2, CoreS3.Display.height()/2 + 20, 2);
}

void drawProjectTiles(const std::vector<String>& titles) {
  CoreS3.Display.fillScreen(BLACK);
  projectButtons.clear();
  CoreS3.Display.setTextDatum(top_center);
  CoreS3.Display.setTextColor(WHITE);
  CoreS3.Display.setTextSize(3);
  CoreS3.Display.drawString("Presentations", CoreS3.Display.width()/2, 6);
  CoreS3.Display.setTextSize(1);

  int availableHeight = CoreS3.Display.height() - projectListStartY - 20;
  projectListRowsPerPage = availableHeight / (projectListTileH + projectListMargin);

  int startIndex = projectListScroll * projectListCols;
  int tilesPerPage = projectListRowsPerPage * projectListCols;
  int endIndex = min((int)titles.size(), startIndex + tilesPerPage);

  for (int i = startIndex; i < endIndex; ++i) {
    int rel = i - startIndex;
    int col = rel % projectListCols;
    int row = rel / projectListCols;
    int16_t x = projectListMargin + col * (projectListTileW + projectListMargin);
    int16_t y = projectListStartY + row * (projectListTileH + projectListMargin);

    CoreS3.Display.fillRoundRect(x, y, projectListTileW, projectListTileH, 8, WHITE);
    CoreS3.Display.drawRoundRect(x, y, projectListTileW, projectListTileH, 8, BLACK);
    CoreS3.Display.setTextDatum(middle_center);
    CoreS3.Display.setTextColor(BLACK, WHITE);
    CoreS3.Display.setTextSize(2);
    CoreS3.Display.drawString(titles[i], x + projectListTileW/2, y + projectListTileH/2);

    projectButtons.push_back({x, y, projectListTileW, projectListTileH, titles[i], i});
  }

  if (projectListScroll > 0) {
    CoreS3.Display.fillTriangle(
      CoreS3.Display.width()/2 - 10, projectListStartY - 24,
      CoreS3.Display.width()/2 + 10, projectListStartY - 24,
      CoreS3.Display.width()/2,     projectListStartY - 6,
      WHITE);
  }
  if (endIndex < (int)titles.size()) {
    int bottomY = projectListStartY + projectListRowsPerPage * (projectListTileH + projectListMargin);
    CoreS3.Display.fillTriangle(
      CoreS3.Display.width()/2 - 10, bottomY + 4,
      CoreS3.Display.width()/2 + 10, bottomY + 4,
      CoreS3.Display.width()/2,      bottomY + 20,
      WHITE);
  }
}

void drawArrows() {
  CoreS3.Display.fillTriangle(
    15, CoreS3.Display.height()/2,
    45, CoreS3.Display.height()/2 - 20,
    45, CoreS3.Display.height()/2 + 20,
    TFT_WHITE);
  CoreS3.Display.fillTriangle(
    CoreS3.Display.width() - 15, CoreS3.Display.height()/2,
    CoreS3.Display.width() - 45, CoreS3.Display.height()/2 - 20,
    CoreS3.Display.width() - 45, CoreS3.Display.height()/2 + 20,
    TFT_WHITE);
}

void drawPointer() {
  if (currentPointerX >= 0 && currentPointerY >= 0) {
    CoreS3.Display.fillCircle(currentPointerX, currentPointerY, pointerRadius, TFT_RED);
    CoreS3.Display.fillCircle(currentPointerX, currentPointerY, pointerRadius - pointerRingWidth, TFT_BLACK);
    prevPointerX = currentPointerX;
    prevPointerY = currentPointerY;
  }
}

void erasePointer() {
  if (prevPointerX >= 0 && prevPointerY >= 0) {
    CoreS3.Display.fillCircle(prevPointerX, prevPointerY, pointerRadius, TFT_BLACK);
    drawArrows();
    displayZoomValue();
    updateRecIndicator();
    drawLockIndicator();
  }
}

void displayZoomValue() {
  CoreS3.Display.setTextSize(2);
  CoreS3.Display.setTextColor(TFT_WHITE);
  int rectX = CoreS3.Display.width()/2 - 60;
  int rectY = 0;
  CoreS3.Display.fillRect(rectX, rectY, 120, 35, TFT_BLACK);
  if (zoomLevel > 1) {
    CoreS3.Display.setCursor(rectX + 10, rectY + 5);
    CoreS3.Display.printf("ZOOM: %dx", zoomLevel);
  }
}

void drawLockIndicator() {
  int x = 18;
  int y = 18;
  CoreS3.Display.fillRect(x - 14, y - 14, 28, 28, TFT_BLACK);
  if (controlsLocked) {
    CoreS3.Display.drawRoundRect(x - 10, y - 6, 20, 18, 4, TFT_YELLOW);
    CoreS3.Display.fillRect(x - 6, y - 10, 12, 8, TFT_BLACK);
    CoreS3.Display.drawRect(x - 6, y - 10, 12, 8, TFT_YELLOW);
    CoreS3.Display.fillCircle(x, y + 3, 3, TFT_YELLOW);
  }
}

// =======================================================================
// Recording management
static bool micInitialized = false;
void ensureMic() {
  if (!micInitialized) {
    CoreS3.Mic.begin();
    micInitialized = true;
  }
}

void stopPlayback() {
  if (playbackActive) {
    playbackFile.close();
    playbackActive = false;
    playbackPaused = false;
    CoreS3.Speaker.stop();
  }
}

void pausePlayback() {
  if (playbackActive && !playbackPaused) {
    playbackPaused = true;
    CoreS3.Speaker.stop();
  }
}

void resumePlayback() {
  if (playbackActive && playbackPaused) {
    playbackPaused = false;
  }
}

void stopSending() {
  if (sendingActive) {
    sendingFile.close();
    sendingActive = false;
    sendingHeaderSent = false;
  }
}

void startRecording() {
  if (recState == REC_RECORDING) return;
  stopPlayback();
  stopSending();

  if (CoreS3.Speaker.isEnabled()) {
    CoreS3.Speaker.stop();
    CoreS3.Speaker.end();
  }
  CoreS3.Mic.begin();

  if (!SD.exists("/records")) SD.mkdir("/records");
  String fname = makeTimestampFilename();
  audioFile = SD.open(fname, FILE_WRITE);
  if (!audioFile) {
    Serial.println("Failed to open file for recording");
    return;
  }
  recState = REC_RECORDING;
  blinkTimer = millis();
  blinkOn = true;
  printCommand("record_start");
  Serial.println("Recording started: " + fname);
}

void stopRecording() {
  if (recState == REC_IDLE) return;
  if (audioFile) { audioFile.flush(); audioFile.close(); }
  recState = REC_IDLE;
  blinkOn = false;
  printCommand("record_stop");
  Serial.println("Recording stopped");
  if (CoreS3.Mic.isEnabled()) {
    CoreS3.Mic.end();
    micInitialized = false;
  }
  CoreS3.Speaker.begin();
  updateRecIndicator();
  refreshRecordFiles();
  enforceMaxRecords();
  if (currentState == SAVED_RECORDINGS) drawSavedRecordingsScreen();
}

void pauseRecording() {
  if (recState != REC_RECORDING) return;
  recState = REC_PAUSED;
  blinkOn = false;
  printCommand("record_pause");
  Serial.println("Recording paused");
}

void resumeRecording() {
  if (recState != REC_PAUSED) return;
  recState = REC_RECORDING;
  blinkTimer = millis();
  blinkOn = true;
  printCommand("record_resume");
  Serial.println("Recording resumed");
}

void updateRecIndicator() {
  if (currentState != PRESENTING_MODE) return;
  int r = 10;
  int cx = CoreS3.Display.width() - 15;
  int cy = 15;
  CoreS3.Display.fillCircle(cx, cy, r + 2, TFT_BLACK);
  if (recState == REC_IDLE) return;
  if (recState == REC_RECORDING) {
    unsigned long now = millis();
    if (now - blinkTimer > 400) { blinkTimer = now; blinkOn = !blinkOn; }
    if (blinkOn) CoreS3.Display.fillCircle(cx, cy, r, TFT_RED);
    else CoreS3.Display.drawCircle(cx, cy, r, TFT_RED);
  } else if (recState == REC_PAUSED) {
    CoreS3.Display.fillCircle(cx, cy, r, TFT_GREEN);
  }
}

void handleRecordingStream() {
  if (recState != REC_RECORDING) return;
  if (!audioFile) return;
  if (CoreS3.Mic.record(rec_buffer, record_length, requested_sample_rate)) {
    audioFile.write((uint8_t*)rec_buffer, record_length * sizeof(int16_t));
  }
}

// =======================================================================
// File listing & retention
String baseName(const String& path) {
  int idx = path.lastIndexOf('/');
  if (idx >= 0) return path.substring(idx + 1);
  return path;
}

void refreshRecordFiles() {
  recordFiles.clear();
  if (!SD.exists("/records")) return;
  File dir = SD.open("/records");
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return;
  }
  File f;
  while ((f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      String name = f.name();
      if (name.endsWith(".raw")) {
        size_t sz = f.size();
        RecordingMeta m;
        m.path = String("/records/") + baseName(name);
        m.displayName = baseName(name);
        m.datetime = m.displayName;
        m.bytes = sz;
        recordFiles.push_back(m);
      }
    }
    f.close();
  }
  dir.close();
  std::sort(recordFiles.begin(), recordFiles.end(),
            [](const RecordingMeta& a, const RecordingMeta& b) {
              return a.displayName > b.displayName;
            });
}

void enforceMaxRecords() {
  while ((int)recordFiles.size() > MAX_RECORDS) {
    RecordingMeta victim = recordFiles.back();
    recordFiles.pop_back();
    SD.remove(victim.path);
    Serial.println("Deleted old recording: " + victim.path);
  }
}

// =======================================================================
// Saved Recordings Screen
void drawSavedRecordingsScreen() {
  CoreS3.Display.fillScreen(TFT_BLACK);
  CoreS3.Display.setTextDatum(top_center);
  CoreS3.Display.setTextColor(TFT_WHITE);
  CoreS3.Display.setTextSize(2);
  CoreS3.Display.drawString("Recent Recording", CoreS3.Display.width()/2, 5);
  CoreS3.Display.setTextSize(1);
  CoreS3.Display.setTextDatum(top_left);

  if (sendingSuccess && (millis() - sendingSuccessTime > 2000)) {
    sendingSuccess = false;
  }

  if (recordFiles.empty()) {
    CoreS3.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    CoreS3.Display.setCursor(10, 60);
    CoreS3.Display.print("No recordings.");
  } else {
    const RecordingMeta &rec = recordFiles[0];
    int xStart = 10;
    int usableW = CoreS3.Display.width() - 20;
    int infoY = 40;
    int infoH = 60;

    CoreS3.Display.drawRoundRect(xStart, infoY, usableW, infoH, 8, TFT_WHITE);

    CoreS3.Display.setTextColor(TFT_CYAN, TFT_BLACK);
    CoreS3.Display.setCursor(xStart + 10, infoY + 8);
    CoreS3.Display.printf("%s", rec.displayName.c_str());

    CoreS3.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
    CoreS3.Display.setCursor(xStart + 10, infoY + 32);
    CoreS3.Display.printf("Size: %uB", (unsigned)rec.bytes);

    int btnY = infoY + infoH + 15;
    int btnH = 90;
    int gap = 12;
    int btnW = (usableW - gap) / 2;

    CoreS3.Display.fillRoundRect(xStart, btnY, btnW, btnH, 10, TFT_DARKGREEN);
    CoreS3.Display.drawRoundRect(xStart, btnY, btnW, btnH, 10, TFT_WHITE);
    CoreS3.Display.setTextDatum(middle_center);
    CoreS3.Display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    CoreS3.Display.setTextSize(2);
    if (!playbackActive) {
      CoreS3.Display.drawString("PLAY", xStart + btnW/2, btnY + btnH/2);
    } else {
      if (playbackPaused) CoreS3.Display.drawString("PLAY", xStart + btnW/2, btnY + btnH/2);
      else CoreS3.Display.drawString("PAUSE", xStart + btnW/2, btnY + btnH/2);
    }

    uint16_t sendColor = TFT_MAGENTA;
    if (sendingActive) sendColor = TFT_ORANGE;
    else if (sendingSuccess) sendColor = TFT_DARKGREEN;

    int sendX = xStart + btnW + gap;
    CoreS3.Display.fillRoundRect(sendX, btnY, btnW, btnH, 10, sendColor);
    CoreS3.Display.drawRoundRect(sendX, btnY, btnW, btnH, 10, TFT_WHITE);
    CoreS3.Display.setTextColor(TFT_WHITE, sendColor);

    if (sendingActive) CoreS3.Display.drawString("SENDING", sendX + btnW/2, btnY + btnH/2);
    else if (sendingSuccess) CoreS3.Display.drawString("SUCCESS", sendX + btnW/2, btnY + btnH/2);
    else CoreS3.Display.drawString("SEND", sendX + btnW/2, btnY + btnH/2);

    CoreS3.Display.setTextDatum(top_left);
  }

  CoreS3.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  CoreS3.Display.setCursor(10, CoreS3.Display.height() - 18);
  CoreS3.Display.print("Swipe to navigate screens");
}

// =======================================================================
// Playback
void startPlayback(int idx) {
  if (idx < 0 || idx >= (int)recordFiles.size()) return;
  stopPlayback();
  stopSending();
  if (recState == REC_RECORDING) return;

  if (CoreS3.Mic.isEnabled() && recState == REC_IDLE) {
    CoreS3.Mic.end();
    micInitialized = false;
  }
  if (!CoreS3.Speaker.isEnabled()) CoreS3.Speaker.begin();

  playbackFile = SD.open(recordFiles[idx].path, FILE_READ);
  if (!playbackFile) {
    Serial.println("Playback open failed");
    return;
  }
  playbackActive = true;
  playbackPaused = false;
  lastPlaybackFeed = 0;
  Serial.println("Playing: " + recordFiles[idx].path);
}

void handlePlayback() {
  if (!playbackActive || playbackPaused) return;
  if (!playbackFile) { playbackActive = false; return; }
  if (millis() - lastPlaybackFeed < PLAYBACK_FEED_INTERVAL_MS) return;

  if (playbackFile.available()) {
    size_t toRead = min((size_t)PLAYBACK_CHUNK, (size_t)playbackFile.available());
    size_t got = playbackFile.read(playbackBuf, toRead);
    if (got > 0) {
      CoreS3.Speaker.playRaw(
        playbackBuf,
        got,
        requested_sample_rate,
        false,
        16,
        true,
        true
      );
      lastPlaybackFeed = millis();
    }
  } else {
    stopPlayback();
    if (currentState == SAVED_RECORDINGS) drawSavedRecordingsScreen();
  }
}

// =======================================================================
// Sending (WAV over WebSocket)
void sendChunkJson(const String& filename, uint32_t seq, const String& b64, bool eof) {
  StaticJsonDocument<512> d;
  d["command"] = "audio_chunk";
  d["filename"] = filename;
  d["seq"] = seq;
  d["data"] = b64;
  d["eof"] = eof;
  String s;
  serializeJson(d, s);
  if (activeClient.available()) activeClient.send(s);
  Serial.println(s);
}

void startSendWav(int idx) {
  if (idx < 0 || idx >= (int)recordFiles.size()) return;
  stopSending();
  stopPlayback();
  if (recState == REC_RECORDING) {
    Serial.println("Cannot send while recording");
    return;
  }
  sendingFile = SD.open(recordFiles[idx].path, FILE_READ);
  if (!sendingFile) {
    Serial.println("Send open failed");
    return;
  }
  sendingFileSize = sendingFile.size();
  sendingActive = true;
  sendingHeaderSent = false;
  sendingSeq = 0;
  sendingBytesSent = 0;
  sendingFilenameBase = recordFiles[idx].displayName;
  sendingSuccess = false;
}

void handleSending() {
  if (!sendingActive) return;
  if (!sendingFile) { sendingActive = false; return; }

  uint8_t outBuf[44 + SEND_CHUNK_RAW];
  size_t outLen = 0;

  if (!sendingHeaderSent) {
    uint8_t header[44];
    buildWavHeader(header, sendingFileSize, requested_sample_rate);
    memcpy(outBuf, header, 44);
    outLen += 44;
    sendingHeaderSent = true;
  }

  size_t remainingAudio = sendingFileSize - sendingBytesSent;
  size_t capacityForAudio = sizeof(outBuf) - outLen;
  size_t toRead = min(remainingAudio, capacityForAudio);
  size_t got = 0;
  if (toRead > 0) {
    got = sendingFile.read(outBuf + outLen, toRead);
    outLen += got;
    sendingBytesSent += got;
  }

  bool eof = (sendingBytesSent >= sendingFileSize);
  String b64 = base64Encode(outBuf, outLen);
  sendChunkJson(sendingFilenameBase + ".wav", sendingSeq++, b64, eof);

  if (eof) {
    sendingFile.close();
    sendingActive = false;
    sendingHeaderSent = false;
    sendingSuccess = true;
    sendingSuccessTime = millis();
    Serial.println("Send complete");
    if (currentState == SAVED_RECORDINGS) drawSavedRecordingsScreen();
  }
}

// =======================================================================
// Presentation screen
void drawPresentationScreen() {
  CoreS3.Display.fillScreen(TFT_BLACK);
  CoreS3.Display.setTextDatum(top_center);
  CoreS3.Display.setTextColor(TFT_WHITE);
  CoreS3.Display.setTextSize(2);
  CoreS3.Display.drawString(currentProjectTitle, CoreS3.Display.width()/2, 5);
  drawArrows();
  displayZoomValue();
  centerPointer();
  drawPointer();
  updateRecIndicator();
  drawLockIndicator();
  sendPointerJump(currentPointerX, currentPointerY);
}

// =======================================================================
void printCommand(const char* command, int value) {
  StaticJsonDocument<200> d;
  d["command"] = command;
  if (strcmp(command, "zoom") == 0 || strstr(command, "record"))
    d["value"] = value;
  serializeJson(d, Serial); Serial.println();
  if (activeClient.available()) {
    String s; serializeJson(d, s);
    activeClient.send(s);
  }
}

void printPointerCommand(int x, int y) {
  StaticJsonDocument<200> d;
  d["command"] = "pointer";
  d["x"] = x;
  d["y"] = y;
  serializeJson(d, Serial); Serial.println();
  if (activeClient.available()) {
    String s; serializeJson(d, s);
    activeClient.send(s);
  }
}

// Modified: send only pointer_jump (no separate legacy pointer to avoid duplication)
void sendPointerJump(int x, int y) {
  StaticJsonDocument<200> d;
  d["command"] = "pointer_jump";
  d["x"] = x;
  d["y"] = y;
  String s;
  serializeJson(d, s);
  if (activeClient.available()) activeClient.send(s);
  Serial.println(s);
}

// Modified: send only pointer_scroll (no extra pointer)
void sendPointerScroll(int dx, int dy, int x, int y) {
  StaticJsonDocument<200> d;
  d["command"] = "pointer_scroll";
  d["dx"] = dx;
  d["dy"] = dy;
  d["x"] = x;
  d["y"] = y;
  String s;
  serializeJson(d, s);
  if (activeClient.available()) activeClient.send(s);
  Serial.println(s);
}

void sendLockState() {
  if (!activeClient.available()) return;
  StaticJsonDocument<128> d;
  d["command"] = "lock";
  d["locked"] = controlsLocked;
  String s;
  serializeJson(d, s);
  activeClient.send(s);
  Serial.println(s);
}

// ADDED: pointer select command (triggered by power button)
void sendPointerSelect() {
  if (currentPointerX < 0 || currentPointerY < 0) {
    centerPointer();
  }
  StaticJsonDocument<200> d;
  d["command"] = "pointer_select";
  d["x"] = currentPointerX;
  d["y"] = currentPointerY;
  String s;
  serializeJson(d, s);
  if (activeClient.available()) activeClient.send(s);
  Serial.println(s);
}

void simpleSlideChange(bool forward) {
  CoreS3.Display.fillScreen(TFT_DARKGREEN);
  delay(60);
  CoreS3.Display.fillScreen(TFT_BLACK);
  drawArrows();
  displayZoomValue();
  centerPointer();
  drawPointer();
  updateRecIndicator();
  drawLockIndicator();
}

// =======================================================================
// WebSocket message handler
void handleWsMessage(WebsocketsMessage msg) {
  String receivedData = msg.data();
  projectButtons.clear();
  tileIndex = 0;

  doc.clear();
  DeserializationError err = deserializeJson(doc, receivedData);
  if (err) {
    projectTitles.clear();
    projectTitles.push_back(receivedData);
    drawProjectTiles(projectTitles);
    currentState = SHOWING_PROJECTS;
    return;
  }

  if (doc.is<JsonArray>()) {
    projectTitles.clear();
    for (JsonVariant v : doc.as<JsonArray>()) {
      projectTitles.push_back(v.as<String>());
    }
    drawProjectTiles(projectTitles);
    currentState = SHOWING_PROJECTS;
  } else if (doc.containsKey("command")) {
    if (String(doc["command"]) == "clear") {
      projectButtons.clear();
      projectTitles.clear();
      tileIndex = 0;
    }
  } else {
    String message = doc.as<String>();
    projectTitles.clear();
    projectTitles.push_back(message);
    drawProjectTiles(projectTitles);
    currentState = SHOWING_PROJECTS;
  }
}

void setup() {
  auto cfg = M5.config();
  CoreS3.begin(cfg);
  Serial.begin(115200);

  CoreS3.Speaker.begin();

  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
    CoreS3.Display.drawString("SD Card failed!", 160, 60);
    while (1) { delay(100); }
  }
  if (!SD.exists("/records")) SD.mkdir("/records");
  refreshRecordFiles();
  enforceMaxRecords();

  delay(250);
  CoreS3.Display.setRotation(1);
  CoreS3.Display.setTextSize(2);
  CoreS3.Display.clear();
  CoreS3.Display.println("Connecting WiFi...");

  displayMessage("Wi-Fi", "Connecting...", YELLOW);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  ensureTimeSynced();

  String ip_address = "IP: " + WiFi.localIP().toString();
  wsServer.listen(8080);

  displayMessage("Ready!", ip_address.c_str(), WHITE);
  delay(1000);
  currentState = SHOWING_PROJECTS;
  drawProjectTiles(projectTitles);
}

// =======================================================================
// Main loop
void loop() {
  CoreS3.update();
  auto touch = CoreS3.Touch.getDetail();

  // Power button click -> pointer_select
  // (Assumes CoreS3.BtnPWR is available in this environment)
  if (CoreS3.BtnPWR.wasClicked()) { // ADDED
    sendPointerSelect();
  }

  if (!activeClient.available()) {
    WebsocketsClient client = wsServer.accept();
    if (client.available()) {
      Serial.println("Client connected!");
      activeClient = client;
      activeClient.onMessage(handleWsMessage);
      displayMessage("Connected!", "Waiting for project list...", GREEN);
    }
  }
  if (activeClient.available()) activeClient.poll();

  if (touch.wasPressed()) {
    pressStartX = touch.x;
    pressStartY = touch.y;
    lastSwipeX = touch.x;
    lastSwipeY = touch.y;
    pointerScrolling = false;
    lastScrollTouchX = -1;
    lastScrollTouchY = -1;

    if (currentState == PRESENTING_MODE) {
      swipeStartX = touch.x;
      swipeStartY = touch.y;

      if (zoomLevel > 1) {
        lastScrollTouchX = touch.x;
        lastScrollTouchY = touch.y;
      }

      int cornerW = 90;
      int cornerH = 90;
      int cornerX0 = CoreS3.Display.width() - cornerW;
      int cornerY0 = CoreS3.Display.height() - cornerH;
      if (touch.x >= cornerX0 && touch.y >= cornerY0) {
        recHoldActive = true;
        recHoldStart  = millis();
        recHoldHandled = false;
      } else {
        recHoldActive = false;
      }

      int centerX = CoreS3.Display.width()/2;
      int centerY = CoreS3.Display.height()/2;
      int lockRadius = 90;
      if (abs(touch.x - centerX) < lockRadius && abs(touch.y - centerY) < lockRadius) {
        if (zoomLevel == 1) {
          lockHoldActive = true;
          lockHoldStart = millis();
          lockHoldHandled = false;
        } else {
          lockHoldActive = false;
        }
      } else {
        lockHoldActive = false;
      }
    }
  }

  if (currentState == PRESENTING_MODE && touch.isPressed()) {
    if (!pointerScrolling && zoomLevel == 1 && lockHoldActive && !lockHoldHandled && (millis() - lockHoldStart >= 2000)) {
      lockHoldHandled = true;
      controlsLocked = !controlsLocked;
      drawLockIndicator();
      sendLockState();
    }
    if (!pointerScrolling && recHoldActive && !recHoldHandled && (millis() - recHoldStart >= 2000)) {
      recHoldHandled = true;
      if (recState == REC_IDLE) startRecording();
      else stopRecording();
      updateRecIndicator();
    }
  }

  if (touch.wasReleased()) {
    int dx = touch.x - pressStartX;
    int dy = touch.y - pressStartY;

    if (currentState == SHOWING_PROJECTS) {
      if (abs(dy) > abs(dx) && abs(dy) > 40) {
        int maxScroll = max(0,
          ((int)projectTitles.size() + 1) / 2 - projectListRowsPerPage);
        if (dy < 0 && projectListScroll < maxScroll) { projectListScroll++; drawProjectTiles(projectTitles); }
        else if (dy > 0 && projectListScroll > 0) { projectListScroll--; drawProjectTiles(projectTitles); }
      } else if (abs(dx) > abs(dy) && abs(dx) > 80) {
        if (dx < 0) {
          if (!projectTitles.empty()) {
            currentProjectTitle = projectTitles[0];
            drawPresentationScreen();
            currentState = PRESENTING_MODE;
          }
        } else {
          currentState = SAVED_RECORDINGS;
          refreshRecordFiles();
          drawSavedRecordingsScreen();
        }
      }
      if (abs(dx) < 20 && abs(dy) < 20) {
        for (const auto& btn : projectButtons) {
            if (pressStartX >= btn.x && pressStartX <= (btn.x + btn.w) &&
                pressStartY >= btn.y && pressStartY <= (btn.y + btn.h)) {
              currentProjectTitle = btn.title;
              DynamicJsonDocument cmdDoc(256);
              cmdDoc["command"] = "start";
              cmdDoc["title"] = btn.title;
              String cmdString; serializeJson(cmdDoc, cmdString);
              if (activeClient.available()) activeClient.send(cmdString);
              drawPresentationScreen();
              currentState = PRESENTING_MODE;
              break;
            }
        }
      }
    } else if (currentState == PRESENTING_MODE) {
      if (!pointerScrolling) {
        if ((recState == REC_RECORDING || recState == REC_PAUSED) &&
            pressStartY >= 0 && pressStartY < 80 &&
            touch.y > CoreS3.Display.height() - 100 &&
            (touch.y - pressStartY) > 120) {
          if (recState == REC_RECORDING) pauseRecording();
          else resumeRecording();
          updateRecIndicator();
        } else {
          if (pressStartX >= 0 && pressStartX <= 50 &&
              pressStartY >= CoreS3.Display.height()/2 - 30 &&
              pressStartY <= CoreS3.Display.height()/2 + 30) {
            printCommand("prev_slide");
            simpleSlideChange(false);
            zoomLevel = 1;
            drawPresentationScreen();
          } else if (pressStartX >= CoreS3.Display.width() - 50 &&
                     pressStartX <= CoreS3.Display.width() &&
                     pressStartY >= CoreS3.Display.height()/2 - 30 &&
                     pressStartY <= CoreS3.Display.height()/2 + 30) {
            printCommand("next_slide");
            simpleSlideChange(true);
            zoomLevel = 1;
            drawPresentationScreen();
          }
        }

        if (abs(dx) > abs(dy) && abs(dx) > 80) {
          if (dx > 0) {
            if (!controlsLocked) {
              currentState = SHOWING_PROJECTS;
              drawProjectTiles(projectTitles);
            }
          } else {
            if (!controlsLocked) {
              currentState = SAVED_RECORDINGS;
              refreshRecordFiles();
              drawSavedRecordingsScreen();
            }
          }
        }
      }
    } else if (currentState == SAVED_RECORDINGS) {
      if (abs(dx) > abs(dy) && abs(dx) > 80) {
        if (dx > 0) {
          currentState = PRESENTING_MODE;
          drawPresentationScreen();
        } else {
          currentState = SHOWING_PROJECTS;
          drawProjectTiles(projectTitles);
        }
      } else if (abs(dx) < 20 && abs(dy) < 20) {
        if (!recordFiles.empty()) {
          int xStart = 10;
          int usableW = CoreS3.Display.width() - 20;
          int infoY = 40;
          int infoH = 60;
          int btnY = infoY + infoH + 15;
          int btnH = 90;
          int gap = 12;
          int btnW = (usableW - gap) / 2;
          int playX = xStart;
          int sendX = xStart + btnW + gap;

          if (pressStartY >= btnY && pressStartY <= btnY + btnH) {
            if (pressStartX >= playX && pressStartX <= playX + btnW) {
              if (!playbackActive) {
                startPlayback(0);
              } else {
                if (playbackPaused) resumePlayback();
                else pausePlayback();
              }
              drawSavedRecordingsScreen();
            } else if (pressStartX >= sendX && pressStartX <= sendX + btnW) {
              if (!sendingActive && !sendingSuccess) {
                startSendWav(0);
                drawSavedRecordingsScreen();
              }
            }
          }
        }
      }
    }

    recHoldActive = false;
    recHoldHandled = false;
    lockHoldActive = false;
    lockHoldHandled = false;
    pressStartX = pressStartY = -1;
    lastSwipeX = lastSwipeY = -1;
    swipeStartX = swipeStartY = -1;
    lastScrollTouchX = lastScrollTouchY = -1;
    pointerScrolling = false;
  }

  if (currentState == PRESENTING_MODE) {
    static unsigned long lastClickTime = 0;
    static bool waitingForSecondTap = false;

    if (touch.wasClicked()) {
      unsigned long now = millis();
      if (waitingForSecondTap && (now - lastClickTime < 300)) {
        if (zoomLevel < 5) zoomLevel++;
        else zoomLevel = 1;
        printCommand("zoom", zoomLevel);
        displayZoomValue();
        waitingForSecondTap = false;
      } else {
        if (zoomLevel == 1) {
          erasePointer();
          currentPointerX = touch.x;
          currentPointerY = touch.y;
          drawPointer();
          sendPointerJump(currentPointerX, currentPointerY);
          updateRecIndicator();
        }
        lastClickTime = now;
        waitingForSecondTap = true;
      }
    } else if (waitingForSecondTap && (millis() - lastClickTime > 300)) {
      waitingForSecondTap = false;
    }

    if (touch.isHolding()) {
      if (zoomLevel == 1) {
        if (currentPointerX != touch.x || currentPointerY != touch.y) {
          erasePointer();
          currentPointerX = touch.x;
          currentPointerY = touch.y;
          drawPointer();
          sendPointerJump(currentPointerX, currentPointerY);
          updateRecIndicator();
        }
      } else {
        if (lastScrollTouchX >= 0 && lastScrollTouchY >= 0) {
          int dx = touch.x - lastScrollTouchX;
          int dy = touch.y - lastScrollTouchY;
          if (dx != 0 || dy != 0) {
            if (!pointerScrolling && (abs(dx) > 2 || abs(dy) > 2)) {
              pointerScrolling = true;
              recHoldActive = false;
              lockHoldActive = false;
            }
            erasePointer();
            int maxX = CoreS3.Display.width() - 1;
            int maxY = CoreS3.Display.height() - 1;
            int newX = currentPointerX + dx;
            if (newX < 0) newX = 0;
            else if (newX > maxX) newX = maxX;
            int newY = currentPointerY + dy;
            if (newY < 0) newY = 0;
            else if (newY > maxY) newY = maxY;
            currentPointerX = newX;
            currentPointerY = newY;
            drawPointer();
            sendPointerScroll(dx, dy, currentPointerX, currentPointerY);
            updateRecIndicator();
            lastScrollTouchX = touch.x;
            lastScrollTouchY = touch.y;
          }
        } else {
          lastScrollTouchX = touch.x;
          lastScrollTouchY = touch.y;
        }
      }
    }

    handleRecordingStream();
    updateRecIndicator();
    drawLockIndicator();
  }

  handlePlayback();
  handleSending();

  delay(6);
}