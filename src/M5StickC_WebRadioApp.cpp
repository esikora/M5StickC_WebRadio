/**
    M5StickC_WebRadioApp:
    This application has been developed to use an M5StickC Plus device
    (ESP32) as internet radio or bluetooth (A2DP) audio receiver.
    
    Copyright (C) 2022 by Ernst Sikora
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <M5StickCPlus.h>
#include <driver/dac.h>
#include "Audio.h"
#include "WifiCredentials.h"
#include "BluetoothA2DPSink.h"
#include <EEPROM.h>
#include <HTTPClient.h>
#include "IftttHook.h"

const uint8_t kPinI2S_BCLK = GPIO_NUM_0; // yellow (PCM5102A board: BCK)
const uint8_t kPinI2S_LRCK = GPIO_NUM_26; // brown (PCM5102A board: LRCK)
const uint8_t kPinI2S_SD = GPIO_NUM_25; // green (PCM5102A board: DIN)

const uint8_t kPinButtonRed = GPIO_NUM_32; // dual button unit: red button
const uint8_t kPinButtonBlue = GPIO_NUM_33; // dual button unit: blue button

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
    "http://www.ndr.de/resources/metadaten/audio/m3u/ndr2.m3u",
    "http://streams.br.de/bayern1obb_2.m3u",
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

// Handle to the RTOS audio task
TaskHandle_t pAudioTask_ = nullptr;

/**
 * Instance of the 'BluetoothA2DPSink' class from the 'ESP32-A2DP' library.
 * Using a pointer and dynamic creation of the instance causes the ESP32 to crash when a2dp_.start() is called.
 */ 
BluetoothA2DPSink a2dp_ = BluetoothA2DPSink();

// Enumeration with possible device modes
enum DeviceMode {NONE = 0, RADIO = 1, A2DP = 2};

typedef enum DeviceMode t_DeviceMode;

// Current device mode (initialization as 'RADIO')
t_DeviceMode deviceMode_ = RADIO;

// Button object for red button
Button buttonRed = Button(kPinButtonRed, false, 40);

// Button object for blue button
Button buttonBlue = Button(kPinButtonBlue, false, 40);

// Content in audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferFilled_ = 0;

// Size of audio buffer (provided by esp32-audioI2S library)
uint32_t audioBufferSize_ = 0;

// Current station index
uint8_t stationIndex_ = 0;

// Flag to indicate the user wants to change the station
bool stationChanged_ = true;

// Flag to indicate that audio is muted after tuning to a new station
bool stationChangedMute_ = true;

// Name of the current station as provided by the stream header data
String stationStr_ = "";

// Flag indicating the station name has changed
bool stationDisplayFlag_ = false;

// audioProcessing: Flag indicating that the connection to a host could not be established
bool connectError_ = false;

// audioProcessing: Flag indicating that the current radio stream provides too little or no data
bool streamError_ = false;

// main: Status indicating the user has paused the current radio stream
bool userStationPause_ = false;

/* Flag indicating that 'userStationPause_' has changed
     main: set
     audioProcessing: clear
*/
bool userStationPauseChanged_ = false;

// Time at which the state of the power button has been read
unsigned long pwrBtnCheckTime_ = 0;

// Sprite for rendering the station name on the display
TFT_eSprite stationSprite_ = TFT_eSprite(&M5.Lcd);

// Info about current song as provided by the stream meta data or from AVRC data
String infoStr_ = "";

// Song artist provided by AVRC data (bluetooth)
String artistStr_ = "";

// Song title provided by AVRC data (bluetooth)
String titleStr_ = "";

// Flag indicating the song title has changed
bool infoDisplayFlag_ = false;

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
 * Function that is executed by the audio processing task in internet radio mode.
 */
void audioProcessing(void *p);

/**
 * Meta data callback function in bluetooth sink mode.
 * Creates the song info string from metadata received via AVRC.
 */
void avrc_metadata_callback(uint8_t id, const uint8_t *text);

// Forward declaration of the volume change callback in bluetooth sink mode
void avrc_volume_change_callback(int vol);

// Forward declaration of the connection state change callback in bluetooth sink mode
void a2dp_connection_state_changed(esp_a2d_connection_state_t state, void*);

/**
 * Callback for WiFi station disconnected event.
 */
void wifiCallbackStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);

/**
 * Shows a welcome message at startup of the device on the TFT display.
 */
void showWelcomeMessage() {
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
}

/**
 * Displays the current station name contained in 'stationStr_' on the TFT screen.
 */
void showStation() {
    stationSprite_.fillSprite(TFT_BLACK);

    if (deviceMode_ == RADIO) {
        stationSprite_.setTextColor(TFT_ORANGE);
    }
    else {
        stationSprite_.setTextColor(TFT_BLUE);
    }
    
    stationSprite_.setCursor(4, 0);
    stationSprite_.print(stationStr_);
    stationSprite_.pushSprite(0, 2); // Render sprite to screen
}

/**
 * Displays the current song information contained in 'infoStr_' on the TFT screen.
 * Each time the song info is updated, it starts scrolling from the right edge.
 */
void showSongInfo() {
    // Update the song title if flag is raised
    if (infoDisplayFlag_) {
        titleSprite_.fillSprite(TFT_BLACK);
        titleSprite_.pushSprite(0, 40); // Wipe out the previous title from the screen

        titleSprite_.setCursor(0, 0);
        titleSprite_.print(infoStr_);

        titlePosX_ = M5.Lcd.width(); // Start scrolling at right side of screen
        titleTextWidth_ = min( titleSprite_.textWidth(infoStr_), kTitleSpriteWidth ); // width of the title required for scrolling

        infoDisplayFlag_ = false; // Clear update flag
    }
    else {
        titlePosX_-= 1; // Move sprite one pixel to the left
        titleSprite_.pushSprite(titlePosX_, 40); // Render sprite to screen

        // After the sprite has passed by completely...
        if (titlePosX_ < -titleTextWidth_) {
            titlePosX_ = M5.Lcd.width(); // ...let the sprite start again at the right side of the screen 
        }
    }
}

/**
 * Displays the volume on the TFT screen.
 * 
 * @param volume Volume to be displayed on the TFT screen.
 */
void showVolume(uint8_t volume) {
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(3, M5.Lcd.height() - M5.Lcd.fontHeight() - 3);
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    M5.Lcd.printf("Vol: %03u", volume);
}

/**
 * Displays the play state on the TFT screen.
 * 
 * @param isPlaying true = 'playing', false = 'stopped'
 */
void showPlayState(bool isPlaying) {
    M5.Lcd.setTextFont(1);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor( (M5.Lcd.width() - M5.Lcd.textWidth("1234567")) / 2, M5.Lcd.height() - M5.Lcd.fontHeight() - 3);

    if (isPlaying) {
        M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Lcd.print("Playing");
    }
    else {
        M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
        M5.Lcd.print("Stopped");
    }
}

/**
 * Starts WiFi connection and waits for a specified amount of time for the WiFi status to become 'WL_CONNECTED'.
 * 
 * @param maxInterval Maximum waiting time in ms.
 */
bool connectWiFi(uint32_t maxInterval = 5000) {
    log_d("WiFi status before WiFi.begin = %u", WiFi.status());

    wl_status_t wifiStatus = WiFi.begin(WifiCredentials::SSID, WifiCredentials::PASSWORD);

    log_d("WiFi status after WiFi.begin = %u", wifiStatus);

    /*  wl_status_t:

        WL_IDLE_STATUS      = 0,
        WL_NO_SSID_AVAIL    = 1,
        WL_SCAN_COMPLETED   = 2,
        WL_CONNECTED        = 3,
        WL_CONNECT_FAILED   = 4,
        WL_CONNECTION_LOST  = 5,
        WL_DISCONNECTED     = 6
    */
    
    unsigned long startTime = millis();
    unsigned long passedTime = 0;

    while ( wifiStatus != WL_CONNECTED && (passedTime < maxInterval) ) {
        int i = 0;
        
        while ( wifiStatus != WL_CONNECTED && i < 10) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            wifiStatus = WiFi.status();
            ++i;
        }
        
        passedTime = millis() - startTime;

        log_d("WiFi status after %lu ms = %u", passedTime, wifiStatus);
    }

    return wifiStatus == WL_CONNECTED;
}

/**
 * Connects to the specified WiFi network and starts the device in internet radio mode.
 * Audio task is started.
 */
void startRadio() {
    log_d("Begin: free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (pAudio_ == nullptr) {
        showWelcomeMessage();
        M5.Lcd.printf(" MAC: %s\n", WiFi.macAddress().c_str()); // Own network mac address

        M5.Lcd.println(" Connecting to WiFi...");
        M5.Lcd.printf(" SSID: %s\n", WifiCredentials::SSID); // WiFi network name

        // Initialize WiFi and connect to network
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(kDeviceName);
        WiFi.onEvent(wifiCallbackStaDisconnected, SYSTEM_EVENT_STA_DISCONNECTED); // Register callback for disconnect events
        
        // Connect to WiFi station
        while ( !connectWiFi(10000) );

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

/**
 * Stops the internet radio including the audio tasks.
 */
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
        stationDisplayFlag_ = false;
        streamError_ = false;
        infoStr_ = "";
        infoDisplayFlag_ = false;
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

/**
 * Starts the device in bluetooth sink (A2DP) mode.
 */
void startA2dp() {
    log_d("Begin: free heap = %d, max alloc heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    i2s_pin_config_t pinConfig = {
        .bck_io_num = kPinI2S_BCLK,
        .ws_io_num = kPinI2S_LRCK,
        .data_out_num = kPinI2S_SD,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    a2dp_.set_pin_config(pinConfig);

    a2dp_.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
    a2dp_.set_avrc_metadata_callback(avrc_metadata_callback);
    //a2dp_.set_on_connection_state_changed(a2dp_connection_state_changed);
    //a2dp_.set_on_volumechange(avrc_volume_change_callback);
    
    showWelcomeMessage();
    M5.Lcd.println(" Starting bluetooth");

    a2dp_.start(kDeviceName);
    deviceMode_ = A2DP;
    
    esp_bt_controller_status_t btStatus = esp_bt_controller_get_status();

    if (btStatus == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        M5.Lcd.println(" Ok");
    }
    else {
        M5.Lcd.printf(" Error (%d)\n", (uint8_t) btStatus);
    }

    stationStr_ = "Bluetooth";
    stationDisplayFlag_ = true;

    vTaskDelay(2000 / portTICK_PERIOD_MS);

    M5.Lcd.fillScreen(TFT_BLACK);

    log_d("End: free heap = %d, max alloc heap = %d, min free heap = %d", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

/**
 * Currently the API does not support stopping the A2DP mode.
 */
void stopA2dp() {
    log_w("Not possible to stop and cleanup 'a2dp_'!");
}

/**
 * Sends the current content of 'infoStr_' to the IFTTT webhook specified by 'IftttHook::IFTTT_ADD_SONG'.
 */
void sendTitle() {
    String infoIfttt = infoStr_; // Create local copy of current info
    
    if (infoIfttt.isEmpty()) { // Prevent sending empty info
        log_d("Not sending title to IFTTT because it is empty.");
        return;
    }

    log_d("Sending title to IFTTT");

    if ( WiFi.status() == WL_CONNECTED ) {
        HTTPClient http;

        http.begin(IftttHook::IFTTT_ADD_SONG); // pass IFTTT webhook URL to HTTP client
        http.addHeader("Content-Type", "application/json");

        String requestBody = "{ \"value1\" : \"" + infoIfttt + "\" }"; // Create json payload

        log_d("Request body:\n%s\n", requestBody.c_str());

        int httpResponseCode = http.POST(requestBody); // Send data using POST method

        if (httpResponseCode > 0) { // Success
       
            String response = http.getString(); // Retrieve response
            
            log_d("HTTP response code: %d", httpResponseCode);
            log_v("HTTP response:\n%s\n", response.c_str());
        }
        else { // Fail
            log_w("Error occurred while sending HTTP POST: %s\n", http.errorToString(httpResponseCode).c_str());
        }
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

void connectToStation() {
    // Establish HTTP connection to requested stream URL
    const char *streamUrl = kStationURLs[stationIndex_].c_str();

    bool success = pAudio_->connecttohost( streamUrl ); // May fail due to wrong host address, socket error or timeout

    if (success) {
        stationChanged_ = false; // Clear flag
        streamError_ = false; // Clear in case a connection error occured before

        timeConnect_ = millis(); // Store time in order to detect stream errors after connecting
    }
    else {
        stationChanged_ = false; // Clear flag
        streamError_ = true; // Raise connection error flag

        log_d("Failed to connect to host '%s'. WiFi status: %u", streamUrl, WiFi.status());
    }

    // Update buffer state variables
    audioBufferFilled_ = pAudio_->inBufferFilled(); // 0 after connecting
    audioBufferSize_ = pAudio_->inBufferFree() + audioBufferFilled_;
}

void stopPlaying() {
    pAudio_->stopSong();
    setAudioShutdown(true); // Turn off amplifier
    stationChangedMute_ = true; // Mute audio until stream becomes stable
}

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
            stopPlaying();
            connectToStation();
        }

        if (userStationPauseChanged_) {
            if (userStationPause_) {
                stopPlaying();
            }
            else {
                connectToStation();
            }
            userStationPauseChanged_ = false;
        }

        // After the buffer has been filled up sufficiently enable audio output
        if (stationChangedMute_ && !userStationPause_) {
            if ( audioBufferFilled_ > 0.9f * audioBufferSize_) {
                setAudioShutdown(false);
                stationChangedMute_ = false;
                streamError_ = false;
            }
            else {
                // If the stream does not build up within a few seconds something is wrong with the connection
                if ( millis() - timeConnect_ > 3000 ) {
                    if (!streamError_) {
                        Serial.printf("Audio buffer low: %u of %u bytes.\n", audioBufferFilled_, audioBufferSize_);
                        streamError_ = true; // Raise connection error flag
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

    // Initialize M5StickC
    M5.begin();
    M5.Lcd.setRotation(3);
    
    if ( EEPROM.begin(1) ) {
        uint8_t mode = EEPROM.readByte(0);

        log_d("EEPROM.readByte(0) = %d", mode);

        if (mode == 2) {
            startA2dp();
        }
        else {
            startRadio();
        }
    }
    else {
        log_w("EEPROM.begin() returned 'false'!");
        startRadio();
    }

    // Update button state
    buttonBlue.read();
    buttonRed.read();
    M5.Axp.GetBtnPress();

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
    buttonBlue.read();
    buttonRed.read();

    // Button B: switch mode and reboot device (internet radio <-> a2dp sink)
    if (M5.BtnB.wasReleased()) {
        log_d("Button B press detected.")

        if (deviceMode_ == RADIO) {
            EEPROM.writeByte(0, 2); // Enter A2DP mode after restart
            EEPROM.commit();

            stopRadio(); // Close connections and clean up
        }
        else {
            EEPROM.writeByte(0, 1); // Enter internet radio mode after restart
            EEPROM.commit();
        }
        ESP.restart();
    }

    // Is the device mode 'internet radio' ?
    if (deviceMode_ == RADIO) {

        // Button A: Switch to next station
        if (M5.BtnA.wasPressed()) {

            log_d("Button A press detected.");
            
            if (userStationPause_) {
                
                log_d("Resume playing.");

                // WiFi may have become idle
                if (WiFi.status() == WL_CONNECTED) {
                    userStationPause_ = false;
                    userStationPauseChanged_ = true;
                }
                else {
                    if ( connectWiFi(10000) ) {
                        userStationPause_ = false;
                        userStationPauseChanged_ = true;

                        connectError_ = false;
                    }
                    else {
                        connectError_ = true;
                    }
                }
            }
            else {
                log_d("Change station.");

                // Turn down volume
                volumeCurrent_ = 0;
                volumeCurrentF_ = 0.0f;
                volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

                showVolume(volumeCurrent_);

                // Advance station index to next station
                stationIndex_ = (stationIndex_ + 1) % kNumStations;
                stationChanged_ = true; // Raise flag for the audio task

                // Erase station name
                stationStr_ = "";
                stationDisplayFlag_ = true; // Raise flag for display update routine

                // Erase stream info
                infoStr_ = "";
                infoDisplayFlag_ = true; // Raise flag for display update routine

                showPlayState(false);
            }
        }
        else {
            // Increase volume gradually after station change
            if (!stationChangedMute_ && volumeCurrent_ < volumeNormal_) {
                volumeCurrentF_ += 0.25;
                volumeCurrent_ = (uint8_t) volumeCurrentF_;
                volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

                showVolume(volumeCurrent_);
            }
        }

        unsigned long curTime = millis();

        // Check every 200 ms whether power button has been pressed
        if (curTime - pwrBtnCheckTime_ > 200) {
            uint8_t pwrBtnState = M5.Axp.GetBtnPress();

            pwrBtnCheckTime_ = curTime;

            if (pwrBtnState & 0x01) {
                log_d("Pwr button long press detected.");
            }

            if (pwrBtnState & 0x02) {
                log_d("Pwr button press detected.");
            }

            // Stop playing if (press XOR long press) has been detected
            if ( !(pwrBtnState & 0x01) != !(pwrBtnState & 0x02) ) { // if both occur simultaneously it is an i2c error

                log_d("Pause.");
                
                if (!userStationPause_) {
                    userStationPause_ = true; // Set status to 'pause'
                    userStationPauseChanged_ = true; // Raise flag that status has changed

                    // Turn down volume while paused
                    volumeCurrent_ = 0;
                    volumeCurrentF_ = 0.0f;
                    volumeCurrentChangedFlag_ = true; // Raise flag for the audio task

                    showVolume(volumeCurrent_); // Show volume on display

                    // Erase stream info
                    infoStr_ = "";
                    infoDisplayFlag_ = true; // Raise flag for display update routine

                    showPlayState(false);
                }
                else {
                    log_d("Already paused - nothing to do.");
                }
            }
        }

        // Notify user in case no data arrives through the stream
        if (connectError_ || streamError_) {
            if (connectError_) {
                stationSprite_.print("WiFi unavailable");
            }

            if (streamError_) {
                stationSprite_.print("Stream unavailable");
            }

            stationSprite_.fillSprite(TFT_RED);
            stationSprite_.setTextColor(TFT_WHITE);
            stationSprite_.setCursor(4, 0);

            stationSprite_.pushSprite(0, 2); // Render sprite to screen

            vTaskDelay(200 / portTICK_PERIOD_MS); // Wait until next cycle
        }
        else {
            // Update the station name if flag is raised
            if (stationDisplayFlag_) {
                showStation();
                stationDisplayFlag_ = false; // Clear update flag

                showPlayState(true);
            }

            // Update song info (usually artist and title)
            showSongInfo();

            // Send song info to IFTTT webhook after the blue button was pressed
            if (buttonBlue.wasPressed()) {
                log_d("Button 'blue' press detected.")

                sendTitle();
            }

            vTaskDelay(20 / portTICK_PERIOD_MS); // Wait until next cycle
        }
    }
    else {
        // Is the device in bluetooth a2dp sink mode?
        if (deviceMode_ == A2DP) {
            // Update the station name if flag is raised
            if (stationDisplayFlag_) {
                showStation();
                stationDisplayFlag_ = false; // Clear update flag
            }

            showSongInfo();

            /*if (volumeCurrentChangedFlag_) {
                showVolume(volumeCurrent_);
            }*/
            
            showPlayState(a2dp_.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED);
            vTaskDelay(20 / portTICK_PERIOD_MS); // Wait until next cycle
        }
        else {
            // Neither radio mode nor A2DP mode
            vTaskDelay(200 / portTICK_PERIOD_MS);
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
    stationDisplayFlag_ = true; // Raise flag for the display update routine

    // Serial.print("station     ");Serial.println(info);
}
void audio_showstreamtitle(const char *info){
    infoStr_ = info;
    infoDisplayFlag_ = true; // Raise flag for the display update routine

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

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
    switch (id) {
        case ESP_AVRC_MD_ATTR_TITLE:
            titleStr_ = (char*) text;
            break;
        
        case ESP_AVRC_MD_ATTR_ARTIST:
            artistStr_ = (char*) text;
            break;
    }    

    if ( artistStr_.isEmpty() ) {
        infoStr_ = titleStr_;
    }
    else {
        if ( titleStr_.isEmpty() ) {
            infoStr_ = artistStr_;
        }
        else {
            infoStr_ = artistStr_ + " - " + titleStr_;
        }
    }
    
    infoDisplayFlag_ = true; // Raise flag for the display update routine
    // Serial.printf("==> AVRC metadata rsp: attribute id 0x%x, %s\n", id, text);
}

void a2dp_connection_state_changed(esp_a2d_connection_state_t state, void*) {

    log_d("Connection state: %d", state);

    if (state != ESP_A2D_CONNECTION_STATE_CONNECTED) {
        infoStr_ = "not connected";
        infoDisplayFlag_ = true; // Raise flag for the display update routine
    }
}

void avrc_volume_change_callback(int vol) {
    volumeCurrent_ = vol;
    volumeCurrentChangedFlag_ = true;
}

void wifiCallbackStaDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    log_d("WiFi: Station disconnected. Reason: %u", info.disconnected.reason);
}