#include <M5StickCPlus.h>
#include <driver/dac.h>
#include "Audio.h"
#include "WifiCredentials.h"
#include "BluetoothA2DPSink.h"
#include <EEPROM.h>

const uint8_t kPinI2S_BCLK = GPIO_NUM_0; // yellow (PCM5102A board: BCK)
const uint8_t kPinI2S_LRCK = GPIO_NUM_26; // brown (PCM5102A board: LRCK)
const uint8_t kPinI2S_SD = GPIO_NUM_25; // green (PCM5102A board: DIN)

// Own host name announced to the WiFi / Bluetooth network
const char* kDeviceName = "ESP32-Webradio";

/** Maximum audio volume that can be set in the 'esp32-audioI2S' library */
const uint8_t kVolumeMax = 21;

/** Width of the stream title sprite in pixels */
const int16_t kTitleSpriteWidth = 1000;

/** Web radio stream URLs */
const String kStationURLs[] = {
    "http://streams.radiobob.de/bob-national/mp3-192/streams.radiobob.de/",
    "http://stream.rockantenne.de/rockantenne/stream/mp3",
    "http://wdr-wdr2-ruhrgebiet.icecast.wdr.de/wdr/wdr2/ruhrgebiet/mp3/128/stream.mp3",
    "http://streams.br.de/bayern3_2.m3u",
    "http://play.antenne.de/antenne.m3u",
    "http://funkhaus-ingolstadt.stream24.net/radio-in.mp3"
};

/** Number of stations */
const uint8_t kNumStations = sizeof(kStationURLs) / sizeof(kStationURLs[0]);

/**
 * Instance of 'Audio' class from 'esp32-audioI2S' library for SPK hat and internal DAC
 * 
 * Constructor: Audio(bool internalDAC = false, i2s_dac_mode_t channelEnabled = I2S_DAC_CHANNEL_LEFT_EN);
 */
//Audio audio_ = Audio(true); // Use internal DAC channel 2 with output to GPIO_26

/**
 * Pointer tp instance of 'Audio' class from 'esp32-audioI2S' library for external DAC PCM5102A.
 * 
 * Constructor: Audio(bool internalDAC = false, i2s_dac_mode_t channelEnabled = I2S_DAC_CHANNEL_LEFT_EN);
 */
Audio *pAudio_ = nullptr;

TaskHandle_t pAudioTask_ = nullptr;

/**
 * Pointer to instance of the 'BluetoothA2DPSink' class from the 'ESP32-A2DP' library.
 */ 
BluetoothA2DPSink *pA2dp_ = nullptr;

enum DeviceMode {NONE = 0, RADIO = 1, A2DP = 2};
typedef enum DeviceMode t_DeviceMode;

t_DeviceMode deviceMode_ = RADIO;


// Content in audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferFilled_ = 0;

// Size of audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferSize_ = 0;

// Current station index
uint8_t stationIndex_ = 0;

// Flag to indicate the user wants to changed the station
bool stationChanged_ = true;

// Flag to indicate that audio is muted after tuning to a new station
bool stationChangedMute_ = true;

// Name of the current station as provided by the stream header data
String stationStr_ = "";

// Flag indicating the station name has changed
bool stationUpdatedFlag_ = false;

// Flag indicating that the connection to a host could not be established
bool connectionError_ = false;

// Sprite for rendering the station name on the display
TFT_eSprite stationSprite_ = TFT_eSprite(&M5.Lcd);

// Title of the current song as provided by the stream meta data
String titleStr_ = "";

// Flag indicating the song title has changed
bool titleUpdatedFlag_ = false;

// Sprite for rendering the song title on the screen
TFT_eSprite titleSprite_ = TFT_eSprite(&M5.Lcd);

// Width of the song title in pixels
int16_t titleTextWidth_ = 0;

// Position of the song title sprite on the screen (used for scrolling)
int16_t titlePosX_ = M5.Lcd.width();

// Audio volume to be set by the audio task
uint8_t volumeCurrent_ = 0;

// Volume as float value for fading
float_t volumeCurrentF_ = 0.0f;

// Flag indicating the volume needs to be set by the audio task
bool volumeCurrentChangedFlag_ = true;

// Audio volume that is set during normal operation
uint8_t volumeNormal_ = kVolumeMax;

// Time in milliseconds at which the connection to the chosen stream has been established
uint64_t timeConnect_ = 0;


void audioProcessing(void *p);

void startRadio() {
    log_d("Begin: free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (pAudio_ == nullptr) {
        // Show some information on the startup screen
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextFont(4);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(TFT_MAGENTA);
            
        M5.Lcd.setCursor(0, 10);
        M5.Lcd.println(" Hello!");

        M5.Lcd.setTextFont(2);
        M5.Lcd.setTextSize(1);
        M5.Lcd.setTextColor(TFT_DARKGREY);
        
        M5.Lcd.setCursor(0, 40);
        M5.Lcd.printf(" Host: %s\n", kDeviceName); // Own host name
        M5.Lcd.printf(" MAC: %s\n", WiFi.macAddress().c_str()); // Own network mac address

        M5.Lcd.println(" Connecting to WiFi...");
        M5.Lcd.printf(" SSID: %s\n", WifiCredentials::SSID); // WiFi network name

        // Initialize WiFi and connect to network
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(kDeviceName);
        WiFi.begin(WifiCredentials::SSID, WifiCredentials::PASSWORD);

        while (!WiFi.isConnected()) {
            delay(100);
        }

        // Display own IP address after connecting
        M5.Lcd.println(" Connected to WiFi");
        M5.Lcd.printf(" IP: %s", WiFi.localIP().toString().c_str());

        pAudio_ = new Audio(false); // Use external DAC

        // Setup audio
        pAudio_->setVolume(0); // 0...21
        pAudio_->setPinout(kPinI2S_BCLK, kPinI2S_LRCK, kPinI2S_SD);

        deviceMode_ = RADIO;

        // Start the audio processing task
        xTaskCreate(audioProcessing, "Audio processing task", 4096, nullptr, configMAX_PRIORITIES - 4, &pAudioTask_);

        // Wait some time before wiping out the startup screen
        vTaskDelay(2000 / portTICK_PERIOD_MS);

        M5.Lcd.fillScreen(TFT_BLACK);
    }
    else {
        log_w("'pAudio_' not cleaned up!");
    }

    log_d("End: free heap = %d, max alloc heap = %d, min free heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

void stopRadio() {
    log_d("Begin : free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    if (pAudio_ != nullptr) {
        deviceMode_ = NONE;
        vTaskDelay(100 / portTICK_PERIOD_MS);

        if (pAudioTask_ != nullptr) {
            vTaskDelete(pAudioTask_);
            pAudioTask_ = nullptr;
        }
        else {
            log_w("Cannot clean up 'pAudioTask_'!");
        }

        pAudio_->stopSong();

        delete pAudio_;

        pAudio_ = nullptr;

        // Set variables to default values
        audioBufferFilled_ = 0;
        audioBufferSize_ = 0;
        // stationIndex_ = 0;
        stationChanged_ = true;
        stationChangedMute_ = true;
        String stationStr_ = "";
        stationUpdatedFlag_ = false;
        connectionError_ = false;
        titleStr_ = "";
        titleUpdatedFlag_ = false;
        titleTextWidth_ = 0;
        titlePosX_ = M5.Lcd.width();
        volumeCurrent_ = 0;
        volumeCurrentF_ = 0.0f;
        volumeCurrentChangedFlag_ = true;

        M5.Lcd.fillScreen(TFT_BLACK);
    }
    else {
        log_w("Cannot clean up 'pAudio_'!");
    }

    log_d("End: free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void startA2dp() {
    log_d("Begin: free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    if (pA2dp_ == nullptr) {
        i2s_pin_config_t pinConfig = {
            .bck_io_num = kPinI2S_BCLK,
            .ws_io_num = kPinI2S_LRCK,
            .data_out_num = kPinI2S_SD,
            .data_in_num = I2S_PIN_NO_CHANGE
        };

        pA2dp_ = new BluetoothA2DPSink();

        pA2dp_->set_pin_config(pinConfig);
        pA2dp_->start(kDeviceName);

        deviceMode_ = A2DP;
    }
    else {
        log_w("'pA2dp_' not cleaned up!");
    }

    log_d("End: free heap = %d, max alloc heap = %d, min free heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

void stopA2dp() {
    if (pA2dp_ != nullptr) {
        deviceMode_ = NONE;

        delete pA2dp_;

        pA2dp_ = nullptr;
    }
    else {
        log_w("Cannot clean up 'pA2dp_'!");
    }
}

/**
 * Enable or disable the shutdown circuit of the amplifier.
 * Amplifier: M5Stack SPK hat with PAM8303.
 * - b = true  --> GPIO_0 = 0 : Shutdown enabled
 * - b = false --> GPIO_0 = 1 : Shutdown disabled
 */
void setAudioShutdown(bool b) {
    /*
    if (b) {
        gpio_set_level(GPIO_NUM_0, 0); // Enable shutdown circuit
    }
    else {
        gpio_set_level(GPIO_NUM_0, 1); // Disable shutdown circuit
    }
    */
}

/**
 * Function to be executed by the audio processing task.
 */
void audioProcessing(void *p) {
    while (true) {
        if (deviceMode_ != RADIO) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        // Process requested change of audio volume
        if (volumeCurrentChangedFlag_) {
            pAudio_->setVolume(volumeCurrent_);
            volumeCurrentChangedFlag_ = false; // Clear flag
        }
        
        // Proces requested station change
        if (stationChanged_) {
            pAudio_->stopSong();
            setAudioShutdown(true); // Turn off amplifier
            stationChangedMute_ = true; // Mute audio until stream becomes stable

            // Establish HTTP connection to requested stream URL
            const char *streamUrl = kStationURLs[stationIndex_].c_str();

            bool success = pAudio_->connecttohost( streamUrl );

            if (success) {
                stationChanged_ = false; // Clear flag
                connectionError_ = false; // Clear in case a connection error occured before

                timeConnect_ = millis(); // Store time in order to detect stream errors after connecting
            }
            else {
                stationChanged_ = false; // Clear flag
                connectionError_ = true; // Raise connection error flag
            }

            // Update buffer state variables
            audioBufferFilled_ = pAudio_->inBufferFilled(); // 0 after connecting
            audioBufferSize_ = pAudio_->inBufferFree() + audioBufferFilled_;
        }

        // After the buffer has been filled up sufficiently enable audio output
        if (stationChangedMute_) {
            if ( audioBufferFilled_ > 0.9f * audioBufferSize_) {
                setAudioShutdown(false);
                stationChangedMute_ = false;
                connectionError_ = false;
            }
            else {
                // If the stream does not build up within a few seconds something is wrong with the connection
                if ( millis() - timeConnect_ > 3000 ) {
                    if (!connectionError_) {
                        Serial.printf("Audio buffer low: %u of %u bytes.\n", audioBufferFilled_, audioBufferSize_);
                        connectionError_ = true; // Raise connection error flag
                    }
                }
            }
        }

        // Let 'esp32-audioI2S' library process the web radio stream data
        pAudio_->loop();

        audioBufferFilled_ = pAudio_->inBufferFilled(); // Update used buffer capacity
        
        vTaskDelay(1 / portTICK_PERIOD_MS); // Let other tasks execute
    }
}

void setup() {
    /*
    // Setup GPIO ports for SPK hat
    gpio_set_direction(GPIO_NUM_0, GPIO_MODE_OUTPUT); // Shutdown circuit of M5Stack SPK hat
    setAudioShutdown(true);

    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_OUTPUT_OD); // Configure GPIO_26 for DAC output (DAC_CHANNEL_2)
    //dac_output_enable(DAC_CHANNEL_2);
    //dac_output_voltage(DAC_CHANNEL_2, 0);
    */

    log_d("IDF version = %s", ESP.getSdkVersion());
    log_d("Total heap = %d", ESP.getHeapSize());
    log_d("Free heap = %d", ESP.getFreeHeap());
    log_d("Max alloc heap = %d", ESP.getMaxAllocHeap());
    
    if ( EEPROM.begin(1) ) {
        uint8_t mode = EEPROM.readByte(0);

        log_d("EEPROM.readByte(0) = %d", mode);

        if (mode == 2) {
            startA2dp();
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        else {
            startRadio();
        }
    }
    else {
        log_w("EEPROM.begin() returned 'false'!");
        startRadio();
    }

    // Initialize M5StickC
    M5.begin();
    M5.Lcd.setRotation(3);

    // Initialize sprite for station name
    stationSprite_.setTextFont(1);
    stationSprite_.setTextSize(2);
    stationSprite_.setTextColor(TFT_ORANGE);
    stationSprite_.setTextWrap(false);
    stationSprite_.createSprite(M5.Lcd.width(), stationSprite_.fontHeight());

    // Initialize sprite for stream info (artist/song etc.)
    titleSprite_.setTextFont(2);
    titleSprite_.setTextSize(1);
    titleSprite_.setTextColor(TFT_CYAN);
    titleSprite_.setTextWrap(false);
    titleSprite_.createSprite(kTitleSpriteWidth, titleSprite_.fontHeight());

    M5.Axp.ScreenBreath(9);
}

void loop() {
    // Let M5StickC update its state
    M5.update();

    if (M5.BtnB.wasPressed()) {
        if (deviceMode_ == RADIO) {
            EEPROM.writeByte(0, 2);
            EEPROM.commit();
        }
        else {
            EEPROM.writeByte(0, 1);
            EEPROM.commit();
        }
        ESP.restart();
    }

    if (deviceMode_ == RADIO) {

        // Button A: Switch to next station
        if (M5.BtnA.wasPressed()) {
            
            // Turn down volume
            volumeCurrent_ = 0;
            volumeCurrentF_ = 0.0f;
            volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

            // Advance station index to next station
            stationIndex_ = (stationIndex_ + 1) % kNumStations;
            stationChanged_ = true; // Raise flag for the audio task

            // Erase station name
            stationStr_ = "";
            stationUpdatedFlag_ = true; // Raise flag for display update routine

            // Erase stream info
            titleStr_ = "";
            titleUpdatedFlag_ = true; // Raise flag for display update routine
        }
        else {
            // Increase volume gradually after station change
            if (!stationChangedMute_ && volumeCurrent_ < volumeNormal_) {
                volumeCurrentF_ += 0.25;
                volumeCurrent_ = (uint8_t) volumeCurrentF_;
                volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

                M5.Lcd.setTextFont(1);
                M5.Lcd.setTextSize(1);
                M5.Lcd.setCursor(3, M5.Lcd.height() - M5.Lcd.fontHeight() - 3);
                M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
                M5.Lcd.printf("Vol: %2u", volumeCurrent_);
            }
        }

        if (connectionError_) {
            stationSprite_.fillSprite(TFT_RED);
            stationSprite_.setTextColor(TFT_WHITE);
            stationSprite_.setCursor(4, 0);
            stationSprite_.print("Stream unavailable");
            stationSprite_.pushSprite(0, 2); // Render sprite to screen

            vTaskDelay(200 / portTICK_PERIOD_MS); // Wait until next cycle
        }
        else {
            // Update the station name if flag is raised
            if (stationUpdatedFlag_) {
                stationSprite_.fillSprite(TFT_BLACK);
                stationSprite_.setTextColor(TFT_ORANGE);
                stationSprite_.setCursor(4, 0);
                stationSprite_.print(stationStr_);

                stationUpdatedFlag_ = false; // Clear update flag

                stationSprite_.pushSprite(0, 2); // Render sprite to screen
            }

            // Update the song title if flag is raised
            if (titleUpdatedFlag_) {
                titleSprite_.fillSprite(TFT_BLACK);
                titleSprite_.pushSprite(0, 40); // Wipe out the previous title from the screen

                titleSprite_.setCursor(0, 0);
                titleSprite_.print(titleStr_);

                titlePosX_ = M5.Lcd.width(); // Start scrolling at right side of screen
                titleTextWidth_ = min( titleSprite_.textWidth(titleStr_), kTitleSpriteWidth ); // width of the title required for scrolling

                titleUpdatedFlag_ = false; // Clear update flag
            }
            else {
                titlePosX_-= 1; // Move sprite one pixel to the left
                titleSprite_.pushSprite(titlePosX_, 40); // Render sprite to screen

                // After the sprite has passed by completely...
                if (titlePosX_ < -titleTextWidth_) {
                    titlePosX_ = M5.Lcd.width(); // ...let the sprite start again at the right side of the screen 
                }
            }

            vTaskDelay(20 / portTICK_PERIOD_MS); // Wait until next cycle
        }
    }
    else {
        // Not in radio mode
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

// optional
void audio_info(const char *info){
    Serial.print("info        "); Serial.println(info);
}
void audio_id3data(const char *info){  //id3 metadata
    // Serial.print("id3data     ");Serial.println(info);
}
void audio_eof_mp3(const char *info){  //end of file
    // Serial.print("eof_mp3     ");Serial.println(info);
}
void audio_showstation(const char *info){
    stationStr_ = info;
    stationUpdatedFlag_ = true; // Raise flag for the display update routine

    // Serial.print("station     ");Serial.println(info);
}
void audio_showstreamtitle(const char *info){
    titleStr_ = info;
    titleUpdatedFlag_ = true; // Raise flag for the display update routine

    // Serial.print("streamtitle ");Serial.println(info);
}
void audio_bitrate(const char *info){
    // Serial.print("bitrate     ");Serial.println(info);
}
void audio_commercial(const char *info){  //duration in sec
    // Serial.print("commercial  ");Serial.println(info);
}
void audio_icyurl(const char *info){  //homepage
    // Serial.print("icyurl      ");Serial.println(info);
}
void audio_lasthost(const char *info){  //stream URL played
    // Serial.print("lasthost    ");Serial.println(info);
}
void audio_eof_speech(const char *info){
    // Serial.print("eof_speech  ");Serial.println(info);
}