#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "AudioFileSourceSD.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#define SD_CS 5
#define SPI_MOSI 23  // SD Card
#define SPI_MISO 19
#define SPI_SCK 18
#define I2S_DOUT 25
#define I2S_BCLK 27  // I2S
#define I2S_LRC 26

#define TOUCH_VOL_UP 13
#define TOUCH_VOL_DOWN 32
#define TOUCH_CONTROL 4
#define TOUCH_THRESHOLD 30

AudioGeneratorMP3 *mp3;
AudioFileSourceID3 *id3;
AudioOutputI2S *out;
AudioFileSourceSD *file = NULL;
File root;
File currentFile;
float volume = 0.5;
bool isPaused = false;
String currentFileName = "";
int32_t pausePosition = 0;  // Store position when pausing

// Called when a metadata event occurs (i.e. an ID3 tag, an ICY block, etc.
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string) {
  (void)cbData;
  Serial.printf("ID3 callback for: %s = '", type);

  if (isUnicode) {
    string += 2;
  }

  while (*string) {
    char a = *(string++);
    if (isUnicode) {
      string++;
    }
    Serial.printf("%c", a);
  }
  Serial.printf("'\n");
  Serial.flush();
}

void resumePlayback() {
  if (isPaused && currentFileName.length() > 0) {
    file = new AudioFileSourceSD(currentFileName.c_str());
    if (pausePosition > 0) {
      file->seek(pausePosition, SEEK_SET);
    }
    id3 = new AudioFileSourceID3(file);
    id3->RegisterMetadataCB(MDCallback, (void *)"ID3TAG");
    mp3->begin(id3, out);
    isPaused = false;
    Serial.printf("Resumed playback from position %d\n", pausePosition);
  }
}

void pausePlayback() {
  if (!isPaused && file) {
    pausePosition = file->getPos();  // Store current position
    Serial.printf("Pausing at position %d\n", pausePosition);
    isPaused = true;
    mp3->stop();
  }
}

void playNextFile() {
  pausePosition = 0;  // Reset position for new file
  if (file) {
    delete file;
    delete id3;
    file = NULL;
    id3 = NULL;
  }

  do {
    if (currentFile) currentFile.close();
    currentFile = root.openNextFile();
    if (!currentFile) {
      root.rewindDirectory();
      currentFile = root.openNextFile();
    }
    if (currentFile) {
      Serial.printf("Found file: %s, size: %d bytes\n", currentFile.name(), currentFile.size());
    }
  } while (currentFile && (!strstr(currentFile.name(), ".mp3") || currentFile.size() <= 5000));  // Check both extension and size

  if (currentFile) {
    currentFileName = String("/music/") + currentFile.name();
    Serial.printf("Now playing: %s (size: %d bytes)\n", currentFileName.c_str(), currentFile.size());

    file = new AudioFileSourceSD(currentFileName.c_str());
    if (!file->isOpen()) {
      Serial.printf("Failed to open MP3 file: %s\n", currentFileName.c_str());
      return;
    }
    id3 = new AudioFileSourceID3(file);
    id3->RegisterMetadataCB(MDCallback, (void *)"ID3TAG");
    mp3->begin(id3, out);
  } else {
    Serial.println("No valid MP3 files found!");
  }
}

void adjustVolume() {
  static unsigned long lastVolumeUpTime = 0;
  static unsigned long lastVolumeDownTime = 0;
  unsigned long currentTime = millis();

  if (touchRead(TOUCH_VOL_UP) < TOUCH_THRESHOLD) {
    if (currentTime - lastVolumeUpTime > 100) {  // Reduced debounce time, no delay
      volume += 0.05;
      if (volume > 1.0) volume = 1.0;
      out->SetGain(volume);
      Serial.printf("Volume Up: %.2f\n", volume);
      lastVolumeUpTime = currentTime;
    }
  }

  if (touchRead(TOUCH_VOL_DOWN) < TOUCH_THRESHOLD) {
    if (currentTime - lastVolumeDownTime > 100) {  // Reduced debounce time, no delay
      volume -= 0.05;
      if (volume < 0.0) volume = 0.0;
      out->SetGain(volume);
      Serial.printf("Volume Down: %.2f\n", volume);
      lastVolumeDownTime = currentTime;
    }
  }
}


void handleTouchControl() {
  static unsigned long lastTouchTime = 0;
  static unsigned long touchStartTime = 0;
  static bool isTouching = false;
  unsigned long currentTime = millis();

  int touchValue = touchRead(TOUCH_CONTROL);

  if (touchValue < TOUCH_THRESHOLD && !isTouching) {
    if (currentTime - lastTouchTime > 200) {
      isTouching = true;
      touchStartTime = currentTime;
      Serial.println("Touch control detected!");
    }
  } else if (touchValue >= TOUCH_THRESHOLD && isTouching) {
    isTouching = false;
    unsigned long touchDuration = currentTime - touchStartTime;

    if (touchDuration < 500) {
      if (!isPaused) {
        pausePlayback();
      } else {
        resumePlayback();
      }
    } else if (touchDuration >= 500 && touchDuration < 2000) {
      Serial.println("Next track");
      isPaused = false;
      pausePosition = 0;
      mp3->stop();
      playNextFile();
    }
    lastTouchTime = currentTime;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  if (!SD.begin(SD_CS)) {
    Serial.println("Error talking to SD card!");
    while (true)
      ;
  }

  root = SD.open("/music");
  if (!root || !root.isDirectory()) {
    Serial.println("/music folder not found on SD card!");
    while (true)
      ;
  }

  audioLogger = &Serial;
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(volume);

  mp3 = new AudioGeneratorMP3();

  touchAttachInterrupt(TOUCH_VOL_UP, NULL, TOUCH_THRESHOLD);
  touchAttachInterrupt(TOUCH_VOL_DOWN, NULL, TOUCH_THRESHOLD);
  touchAttachInterrupt(TOUCH_CONTROL, NULL, TOUCH_THRESHOLD);

  playNextFile();
  delay(1500);
}

void loop() {
  if (mp3->isRunning() && !isPaused) {
    while (!mp3->loop()) {  // Process audio in tight loop
      adjustVolume();       // Check volume during audio processing
      handleTouchControl();
      mp3->stop();
      pausePosition = 0;
      playNextFile();
      break;
    }
  } else if (!isPaused && !mp3->isRunning()) {
    Serial.printf("MP3 done\n");
    pausePosition = 0;
    playNextFile();
  }

  adjustVolume();
  handleTouchControl();
}
