#include <M5StickCPlus.h>
#include <driver/dac.h>
#include "Audio.h"
#include "WifiCredentials.h"
#include "BluetoothA2DPSink.h"

const uint8_t kPinI2S_BCLK = GPIO_NUM_0; // yellow (PCM5102A board: BCK)
const uint8_t kPinI2S_LRCK = GPIO_NUM_26; // brown (PCM5102A board: LRCK)
const uint8_t kPinI2S_SD = GPIO_NUM_25; // green (PCM5102A board: DIN)

// Own host name announced to the WiFi network
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
 * Instance of 'Audio' class from 'esp32-audioI2S' library for external DAC PCM5102A
 * 
 * Constructor: Audio(bool internalDAC = false, i2s_dac_mode_t channelEnabled = I2S_DAC_CHANNEL_LEFT_EN);
 */
Audio audio_ = Audio(false); // Use external DAC

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

/**
 * Instance of the 'BluetoothA2DPSink' class from the 'ESP32-A2DP' library.
 */ 
BluetoothA2DPSink a2dp_;

enum DeviceMode {RADIO = 0, A2DP = 1};
typedef enum DeviceMode t_DeviceMode;

t_DeviceMode deviceMode_ = RADIO;

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
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        // Process requested change of audio volume
        if (volumeCurrentChangedFlag_) {
            audio_.setVolume(volumeCurrent_);
            volumeCurrentChangedFlag_ = false; // Clear flag
        }
        
        // Proces requested station change
        if (stationChanged_) {
            audio_.stopSong();
            setAudioShutdown(true); // Turn off amplifier
            stationChangedMute_ = true; // Mute audio until stream becomes stable

            // Establish HTTP connection to requested stream URL
            const char *streamUrl = kStationURLs[stationIndex_].c_str();

            bool success = audio_.connecttohost( streamUrl );

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
            audioBufferFilled_ = audio_.inBufferFilled(); // 0 after connecting
            audioBufferSize_ = audio_.inBufferFree() + audioBufferFilled_;
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
        audio_.loop();

        audioBufferFilled_ = audio_.inBufferFilled(); // Update used buffer capacity
        
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

    // Initialize M5StickC
    M5.begin();
    M5.Lcd.setRotation(3);

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

    // Setup audio
    audio_.setVolume(0); // 0...21
    audio_.setPinout(kPinI2S_BCLK, kPinI2S_LRCK, kPinI2S_SD);

    /*
    audio_.forceMono(true); // mono sound on SPK hat
    */

    // Start the audio processing task
    xTaskCreate(audioProcessing, "Audio processing task", 4096, nullptr, configMAX_PRIORITIES - 1, nullptr);

    // Wait some time before wiping out the startup screen
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Axp.ScreenBreath(9);
}

void loop() {
    // Let M5StickC update its state
    M5.update();

    if (M5.BtnB.wasPressed()) {
        if (deviceMode_ == RADIO) {
            deviceMode_ = A2DP;

            vTaskDelay(100 / portTICK_PERIOD_MS);
            
            audio_.stopSong();

            i2s_pin_config_t my_pin_config = {
                .bck_io_num = kPinI2S_BCLK,
                .ws_io_num = kPinI2S_LRCK,
                .data_out_num = kPinI2S_SD,
                .data_in_num = I2S_PIN_NO_CHANGE
            };

            a2dp_.set_pin_config(my_pin_config);
            a2dp_.start(kDeviceName);
        }
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