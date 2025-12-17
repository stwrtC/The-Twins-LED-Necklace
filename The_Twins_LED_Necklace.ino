#include <vector>
#include <string>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#ifdef ARDUINO_ARCH_RP2350
#undef PICO_BUILD
#endif
#include "AnimatedGIF.h"
#define USE_TFT_ESPI_LIBRARY
#include "lv_xiao_round_screen.h"
#include "twinslogo_seeed.h"
#include <PNGdec.h>

AnimatedGIF gif;
// Don't redeclare tft - it's already declared in lv_xiao_round_screen.h
PNG png;  // PNG decoder instance
TFT_eSprite sprite = TFT_eSprite(&tft);  // Sprite instance

struct Background {
  const uint8_t *data;
  size_t size;
};

const Background backgrounds[] = {
  { (const uint8_t *)twinslogo_seeed, sizeof(twinslogo_seeed) },
};

const int numBackgrounds = sizeof(backgrounds) / sizeof(backgrounds[0]);  // Added missing constant
int currentBackground = 0;  // Added missing variable

// PNG callback function for drawing to sprite
void pngDrawToSprite(PNGDRAW *pDraw) {
  uint16_t lineBuffer[240];  // Assuming max width of 240
  
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  sprite.pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer);
}

// State management
enum DisplayMode {
  MODE_GIF,
  MODE_IMAGE
};
DisplayMode currentMode = MODE_GIF;
bool modeChanged = false;
unsigned long lastTouchTime = 0;
const unsigned long touchDebounce = 300;  // ms to prevent multiple touches

// rule: loop GIF at least during 3s, maximum 5 times, and don't loop/animate longer than 30s per GIF
const int maxLoopIterations = 1;    // stop after this amount of loops
const int maxLoopsDuration = 3000;  // ms, max cumulated time after the GIF will break loop
const int maxGifDuration = 240000;  // ms, max GIF duration

// used to center image based on GIF dimensions
static int xOffset = 0;
static int yOffset = 0;

static int totalFiles = 0;  // GIF files count
static int currentFile = 0;  
static int lastFile = -1;

char GifComment[256];

static File FSGifFile;              // temp gif file holder
static File GifRootFolder;          // directory listing
std::vector<std::string> GifFiles;  // GIF files path
#define DISPLAY_WIDTH 240

static void MyCustomDelay(unsigned long ms) {
  delay(ms);
  // log_d("delay %d\n", ms);
}

static void *GIFOpenFile(const char *fname, int32_t *pSize) {
  // log_d("GIFOpenFile( %s )\n", fname );
  FSGifFile = SD.open(fname);
  if (FSGifFile) {
    *pSize = FSGifFile.size();
    return (void *)&FSGifFile;
  }
  return NULL;
}

static void GIFCloseFile(void *pHandle) {
  File *f = static_cast<File *>(pHandle);
  if (f != NULL)
    f->close();
}

static int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t iBytesRead;
  iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos - 1;  // <-- ugly work-around
  if (iBytesRead <= 0)
    return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
}

static int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition) {
  int i = micros();
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  i = micros() - i;
  // log_d("Seek time = %d us\n", i);
  return pFile->iPos;
}

static void TFTDraw(int x, int y, int w, int h, uint16_t *lBuf) {
  tft.pushRect(x + xOffset, y + yOffset, w, h, lBuf);
}

// Draw a line of image directly on the LCD
void GIFDraw(GIFDRAW *pDraw) {
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y, iWidth;

  iWidth = pDraw->iWidth;
  if (iWidth > DISPLAY_WIDTH)
    iWidth = DISPLAY_WIDTH;
  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y;  // current line

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) {  // restore to background color
    for (x = 0; x < iWidth; x++) {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) {  // if transparency used
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + iWidth;
    x = 0;
    iCount = 0;  // count non-transparent pixels
    while (x < iWidth) {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent) {  // done, stop
          s--;                     // back up to treat it like transparent
        } else {                   // opaque
          *d++ = usPalette[c];
          iCount++;
        }
      }              // while looking for opaque pixels
      if (iCount) {  // any opaque pixels?
        TFTDraw(pDraw->iX + x, y, iCount, 1, (uint16_t *)usTemp);
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd) {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }
      if (iCount) {
        x += iCount;  // skip these
        iCount = 0;
      }
    }
  } else {
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x = 0; x < iWidth; x++)
      usTemp[x] = usPalette[*s++];
    TFTDraw(pDraw->iX, y, iWidth, 1, (uint16_t *)usTemp);
  }
} /* GIFDraw() */

int gifPlay(char *gifPath) {  // 0=infinite
  gif.begin(BIG_ENDIAN_PIXELS);
  if (!gif.open(gifPath, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw)) {
    // log_n("Could not open gif %s", gifPath );
    return maxLoopsDuration;
  }

  int frameDelay = 0;  // store delay for the last frame
  int then = 0;        // store overall delay
  bool showcomment = false;

  // center the GIF !!
  int w = gif.getCanvasWidth();
  int h = gif.getCanvasHeight();
  xOffset = (tft.width() - w) / 2;
  yOffset = (tft.height() - h) / 2;

  if (lastFile != currentFile) {
    // log_n("Playing %s [%d,%d] with offset [%d,%d]", gifPath, w, h, xOffset, yOffset );
    lastFile = currentFile;
    showcomment = true;
  }

  while (gif.playFrame(true, &frameDelay)) {
    if (showcomment)
      if (gif.getComment(GifComment))
        // log_n("GIF Comment: %s", GifComment);
        then += frameDelay;
    if (then > maxGifDuration) {  // avoid being trapped in infinite GIF's
      // log_w("Broke the GIF loop, max duration exceeded");
      break;
    }
  }

  gif.close();
  return then;
}

int getGifInventory(const char *basePath) {
  int amount = 0;
  GifRootFolder = SD.open(basePath);
  if (!GifRootFolder) {
    // log_n("Failed to open directory");
    return 0;
  }

  if (!GifRootFolder.isDirectory()) {
    // log_n("Not a directory");
    return 0;
  }

  File file = GifRootFolder.openNextFile();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  int textPosX = tft.width() / 2 - 16;
  int textPosY = tft.height() / 2 - 10;

  tft.drawString("GIF Files:", textPosX - 40, textPosY - 20);

  while (file) {
    if (!file.isDirectory()) {
      GifFiles.push_back(std::string(file.name()));
      amount++;
      tft.drawString(String(amount), textPosX, textPosY);
      file.close();
    }
    file = GifRootFolder.openNextFile();
  }
  GifRootFolder.close();
  // log_n("Found %d GIF files", amount);
  return amount;
}

void setup() {
  Serial.begin(115200);
  // while (!Serial) ;
  // pinMode(D6, OUTPUT);
  // digitalWrite(D6, HIGH);
  
  // Initialize display (tft is already declared in lv_xiao_round_screen.h)
  tft.begin();
  tft.setRotation(2);

  int attempts = 0;
  int maxAttempts = 50;
  int delayBetweenAttempts = 300;
  bool isblinked = false;

  pinMode(D2, OUTPUT);
  while (!SD.begin(D2)) {
    // log_n("SD Card mount failed! (attempt %d of %d)", attempts, maxAttempts );
    isblinked = !isblinked;
    attempts++;
    if (isblinked) {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
      tft.setTextColor(TFT_BLACK, TFT_WHITE);
    }
    tft.drawString("INSERT SD", tft.width() / 2, tft.height() / 2);

    if (attempts > maxAttempts) {
      // log_n("Giving up");
      break;  // Added missing break to prevent infinite loop
    }
    delay(delayBetweenAttempts);
  }

  // log_n("SD Card mounted!");
  Serial.println("SD Card mounted!");

  tft.fillScreen(TFT_BLACK);
  sprite.createSprite(240, 240);  // Match display size
  
  // Initialize touch if available
  #ifdef TOUCH_INT
  pinMode(TOUCH_INT, INPUT_PULLUP);
  #endif
  Wire.begin();

  totalFiles = getGifInventory("/gif");  // scan the SD card GIF folder
  
  if (totalFiles == 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No GIF files found!", tft.width() / 2 - 60, tft.height() / 2);
    while(1) delay(1000);  // Stop execution if no files found
  }
}

void loop() {
  // Only proceed if we have GIF files
  if (totalFiles == 0) {
    delay(1000);
    return;
  }
  
  tft.fillScreen(TFT_BLACK);

  const char *fileName = GifFiles[currentFile % totalFiles].c_str();  // Fixed potential overflow
  const char *fileDir = "/gif/";
  char *filePath = (char *)malloc(strlen(fileName) + strlen(fileDir) + 1);
  
  if (filePath == NULL) {
    Serial.println("Memory allocation failed!");
    return;
  }
  
  strcpy(filePath, fileDir);
  strcat(filePath, fileName);

  int loops = maxLoopIterations;           // max loops
  int durationControl = maxLoopsDuration;  // force break loop after xxx ms

  while (loops-- > 0 && durationControl > 0) {
    durationControl -= gifPlay((char *)filePath);
    gif.reset();
  }
  free(filePath);
  
  currentFile++;  // Moved increment after GIF playback

  // Handle touch input - Fixed the condition check
  #ifdef TOUCH_INT
  if (chsc6x_is_pressed() && currentMode == MODE_GIF) {
    Serial.println("touched");
    unsigned long currentTime = millis();
    if (currentTime - lastTouchTime > touchDebounce) {
      lastTouchTime = currentTime;
      
      currentMode = MODE_IMAGE;  // Switch to image mode
      
      int16_t rc = png.openFLASH((uint8_t *)backgrounds[currentBackground].data,
                                 backgrounds[currentBackground].size,
                                 pngDrawToSprite);
      if (rc == PNG_SUCCESS) {
        sprite.fillSprite(TFT_BLACK);  // Clear sprite first
        png.decode(NULL, 0);
        sprite.pushSprite(0, 0);
        currentBackground = (currentBackground + 1) % numBackgrounds;
        
        // Wait for touch release and another touch to return to GIF mode
        while (chsc6x_is_pressed()) delay(50);  // Wait for release
        delay(1000);  // Show image for at least 1 second
        
        // Wait for next touch to return to GIF mode
        while (!chsc6x_is_pressed()) delay(50);
        while (chsc6x_is_pressed()) delay(50);  // Wait for release
        
        currentMode = MODE_GIF;  // Return to GIF mode
      } else {
        Serial.println("Failed to open PNG file!");
      }
    }
  }
  #endif
}