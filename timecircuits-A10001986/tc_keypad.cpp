/*
 * -------------------------------------------------------------------
 * CircuitSetup.us Time Circuits Display
 * (C) 2021-2022 John deGlavina https://circuitsetup.us
 * (C) 2022-2023 Thomas Winischhofer (A10001986)
 * https://github.com/realA10001986/Time-Circuits-Display-A10001986
 *
 * Keypad handling
 *
 * -------------------------------------------------------------------
 * License: MIT
 * 
 * Permission is hereby granted, free of charge, to any person 
 * obtaining a copy of this software and associated documentation 
 * files (the "Software"), to deal in the Software without restriction, 
 * including without limitation the rights to use, copy, modify, 
 * merge, publish, distribute, sublicense, and/or sell copies of the 
 * Software, and to permit persons to whom the Software is furnished to 
 * do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be 
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#include "tc_global.h"

#include <Arduino.h>

#include "input.h"
#include "tc_menus.h"
#include "tc_audio.h"
#include "tc_time.h"
#include "tc_keypad.h"
#include "tc_menus.h"
#include "tc_settings.h"
#include "tc_time.h"
#include "tc_wifi.h"

#define KEYPAD_ADDR     0x20    // I2C address of the PCF8574 port expander (keypad)

#define ENTER_DEBOUNCE    50    // enter button debounce time in ms
#define ENTER_PRESS_TIME 200    // enter button will register a short press
#define ENTER_HOLD_TIME 2000    // time in ms holding the enter button will count as a long press

#define ETT_DEBOUNCE      50    // external time travel button debounce time in ms
#define ETT_PRESS_TIME   200    // external time travel button will register a short press
#define ETT_HOLD_TIME   3000    // external time travel button will register a long press

// When ENTER button is pressed, turn off display for this many ms
// Must be sync'd to the sound file used (enter.mp3)
#define BADDATE_DELAY 400
#ifdef TWSOUND
#define ENTER_DELAY   500       // For TW sound files
#else
#define ENTER_DELAY   600
#endif

#define SPEC_DELAY   3000

#define EE1_DELAY2   3000
#define EE1_DELAY3   2000
#define EE2_DELAY     600
#define EE3_DELAY     500
#define EE4_DELAY    3000

static const char keys[4*3] = {
     '1', '2', '3',
     '4', '5', '6',
     '7', '8', '9',
     '*', '0', '#'
};

#ifdef GTE_KEYPAD
static const uint8_t rowPins[4] = {5, 0, 1, 3};
static const uint8_t colPins[3] = {4, 6, 2};
#else
static const uint8_t rowPins[4] = {1, 6, 5, 3};
static const uint8_t colPins[3] = {2, 0, 4};
#endif

static Keypad_I2C keypad((char *)keys, rowPins, colPins, 4, 3, KEYPAD_ADDR);

bool isEnterKeyPressed = false;
bool isEnterKeyHeld    = false;
static bool enterWasPressed = false;

static bool needDepTime = false;

#ifdef EXTERNAL_TIMETRAVEL_IN
bool                 isEttKeyPressed = false;
bool                 isEttKeyHeld = false;
static unsigned long ettNow = 0;
static bool          ettDelayed = false;
static unsigned long ettDelay = 0; // ms
static bool          ettLong = DEF_ETT_LONG;
#endif

static unsigned long timeNow = 0;

static unsigned long lastKeyPressed = 0;

#define DATELEN_ALL   12   // mmddyyyyHHMM  dt: month, day, year, hour, min
#define DATELEN_REM   10   // 77mmddHHMM    set reminder
#define DATELEN_DATE   8   // mmddyyyy      dt: month, day, year
#define DATELEN_QALM   6   // 11HHMM/888xxx 11, hour, min (alarm-set shortcut); 888xxx (mp)
#define DATELEN_INT    5   // xxxxx         reset
#define DATELEN_TIME   4   // HHMM          dt: hour, minute
#define DATELEN_CODE   3   // xxx           special codes
#define DATELEN_ALSH   2   // 11            show alarm time/wd
#define DATELEN_CMIN   DATELEN_ALSH     // min length of code entry
#define DATELEN_CMAX   DATELEN_QALM     // max length of code entry
#define DATELEN_MAX    DATELEN_ALL      // max length of possible entry

static char dateBuffer[DATELEN_MAX + 2];
char        timeBuffer[8];

static int dateIndex = 0;
static int timeIndex = 0;
static int yearIndex = 0;

bool menuActive = false;

static bool doKey = false;

static unsigned long enterDelay = 0;

static TCButton enterKey = TCButton(ENTER_BUTTON_PIN,
    false,    // Button is active HIGH
    false     // Disable internal pull-up resistor
);

#ifdef EXTERNAL_TIMETRAVEL_IN
static TCButton ettKey = TCButton(EXTERNAL_TIMETRAVEL_IN_PIN,
    true,     // Button is active LOW
    true      // Enable internal pull-up resistor
);
#endif

static void keypadEvent(char key, KeyState kstate);
static void recordKey(char key);
static void recordSetTimeKey(char key);
static void recordSetYearKey(char key);

static void enterKeyPressed();
static void enterKeyHeld();

#ifdef EXTERNAL_TIMETRAVEL_IN
static void ettKeyPressed();
static void ettKeyHeld();
#endif

static void setupWCMode();
static void buildRemString(char *buf);
static void buildRemOffString(char *buf);
static void mykpddelay(unsigned int mydel);

/*
 * keypad_setup()
 */
void keypad_setup()
{
    // Set up the keypad
    keypad.begin();

    keypad.addEventListener(keypadEvent);

    // Set custom delay function
    // Called between i2c key scan iterations
    // (calls audio_loop() while waiting)
    keypad.setCustomDelayFunc(mykpddelay);

    keypad.setScanInterval(20);
    keypad.setHoldTime(ENTER_HOLD_TIME);

    // Set up pin for white LED
    pinMode(WHITE_LED_PIN, OUTPUT);
    digitalWrite(WHITE_LED_PIN, LOW);

    // Set up Enter button
    enterKey.setPressTicks(ENTER_PRESS_TIME);
    enterKey.setLongPressTicks(ENTER_HOLD_TIME);
    enterKey.setDebounceTicks(ENTER_DEBOUNCE);
    enterKey.attachPress(enterKeyPressed);
    enterKey.attachLongPressStart(enterKeyHeld);

#ifdef EXTERNAL_TIMETRAVEL_IN
    // Set up External time travel button
    ettKey.setPressTicks(ETT_PRESS_TIME);
    ettKey.setLongPressTicks(ETT_HOLD_TIME);
    ettKey.setDebounceTicks(ETT_DEBOUNCE);
    ettKey.attachPress(ettKeyPressed);
    ettKey.attachLongPressStart(ettKeyHeld);

    ettDelay = (int)atoi(settings.ettDelay);
    if(ettDelay > ETT_MAX_DEL) ettDelay = ETT_MAX_DEL;

    ettLong = ((int)atoi(settings.ettLong) > 0);
#endif

    dateBuffer[0] = '\0';
    timeBuffer[0] = '\0';
}

/*
 * scanKeypad(): Scan keypad keys
 */
bool scanKeypad()
{
    return keypad.scanKeypad();
}

/*
 *  The keypad event handler
 */
static void keypadEvent(char key, KeyState kstate)
{
    bool mpWasActive = false;
    bool playBad = false;
    
    if(!FPBUnitIsOn || startup || timeTravelP0 || timeTravelP1 || timeTravelRE)
        return;

    pwrNeedFullNow();

    switch(kstate) {
    case TCKS_PRESSED:
        if(key != '#' && key != '*') {
            play_keypad_sound(key);
            doKey = true;
        }
        break;
        
    case TCKS_HOLD:
        if(keypadInMenu) break;    // Don't do anything while in menu

        switch(key) {
        case '0':    // "0" held down -> time travel
            doKey = false;
            // Complete timeTravel, with speedo
            timeTravel(true, true);
            break;
        case '9':    // "9" held down -> return from time travel
            doKey = false;
            resetPresentTime();
            break;
        case '1':    // "1" held down -> toggle alarm on/off
            doKey = false;
            switch(toggleAlarm()) {
            case -1:
                playBad = true;
                break;
            case 0:
                play_file("/alarmoff.mp3", PA_CHECKNM|PA_ALLOWSD|PA_DYNVOL);
                break;
            case 1:
                play_file("/alarmon.mp3", PA_CHECKNM|PA_ALLOWSD|PA_DYNVOL);
                break;
            }
            break;
        case '4':    // "4" held down -> toggle night-mode on/off
            doKey = false;
            if(toggleNightMode()) {
                manualNightMode = 1;
                play_file("/nmon.mp3", PA_ALLOWSD|PA_DYNVOL);
            } else {
                manualNightMode = 0;
                play_file("/nmoff.mp3", PA_ALLOWSD|PA_DYNVOL);
            }
            manualNMNow = millis();
            break;
        case '3':    // "3" held down -> play audio file "key3.mp3"
            doKey = false;
            play_file("/key3.mp3", PA_CHECKNM|PA_INTRMUS|PA_ALLOWSD|PA_DYNVOL);
            break;
        case '6':    // "6" held down -> play audio file "key6.mp3"
            doKey = false;
            play_file("/key6.mp3", PA_CHECKNM|PA_INTRMUS|PA_ALLOWSD|PA_DYNVOL);
            break;
        case '7':    // "7" held down -> re-enable/re-connect WiFi
            doKey = false;
            if(!wifiOnWillBlock()) {
                play_file("/ping.mp3", PA_CHECKNM|PA_ALLOWSD);
            } else {
                if(haveMusic) mpWasActive = mp_stop();
                play_file("/ping.mp3", PA_CHECKNM|PA_INTRMUS|PA_ALLOWSD);
                waitAudioDone();
            }
            // Enable WiFi / even if in AP mode / with CP
            wifiOn(0, true, false);
            syncTrigger = true;
            // Restart mp if it was active before
            if(mpWasActive) mp_play();   
            break;
        case '2':    // "2" held down -> musicplayer prev
            doKey = false;
            if(haveMusic) {
                mp_prev(mpActive);
            } else playBad = true;
            break;
        case '5':    // "5" held down -> musicplayer start/stop
            doKey = false;
            if(haveMusic) {
                if(mpActive) {
                    mp_stop();
                } else {
                    mp_play();
                }
            } else playBad = true;
            break;
        case '8':   // "8" held down -> musicplayer next
            doKey = false;
            if(haveMusic) {
                mp_next(mpActive);
            } else playBad = true;
            break;
        }
        if(playBad) {
            play_file("/baddate.mp3", PA_CHECKNM|PA_ALLOWSD);
        }
        break;
        
    case TCKS_RELEASED:
        if(doKey) {
            if(keypadInMenu) {
                if(isYearUpdate) {
                    recordSetYearKey(key);
                } else {
                    recordSetTimeKey(key);
                }
            } else {
                recordKey(key);
            }
        }
        break;
    }
}

void resetKeypadState()
{
    doKey = false;
}

static void enterKeyPressed()
{
    isEnterKeyPressed = true;
    pwrNeedFullNow();
}

static void enterKeyHeld()
{
    isEnterKeyHeld = true;
    pwrNeedFullNow();
}

#ifdef EXTERNAL_TIMETRAVEL_IN
static void ettKeyPressed()
{
    isEttKeyPressed = true;
    pwrNeedFullNow();
}

static void ettKeyHeld()
{
    isEttKeyHeld = true;
    pwrNeedFullNow();
}
#endif

static void recordKey(char key)
{
    dateBuffer[dateIndex++] = key;
    dateBuffer[dateIndex] = '\0';
    // Don't wrap around, overwrite end of date instead
    if(dateIndex >= DATELEN_MAX) dateIndex = DATELEN_MAX - 1;  
    lastKeyPressed = millis();
}

static void recordSetTimeKey(char key)
{
    timeBuffer[timeIndex++] = key;
    timeBuffer[timeIndex] = '\0';
    timeIndex &= 0x1;
}

static void recordSetYearKey(char key)
{
    timeBuffer[yearIndex++] = key;
    timeBuffer[yearIndex] = '\0';
    yearIndex &= 0x3;
}

void resetTimebufIndices()
{
    timeIndex = yearIndex = 0;
    // Do NOT clear the timeBuffer, might be pre-set
}

void enterkeyScan()
{
    enterKey.scan();  // scan the enter button

#ifdef EXTERNAL_TIMETRAVEL_IN
    ettKey.scan();    // scan the ext. time travel button
#endif
}

static uint8_t read2digs(uint8_t idx)
{
   return ((dateBuffer[idx] - '0') * 10) + (dateBuffer[idx+1] - '0');
}

/*
 * keypad_loop()
 */
void keypad_loop()
{
    char spTxt[16];
    #define EE1_KL2 12
    char spTxtS2[EE1_KL2] = { 181, 224, 179, 231, 199, 140, 197, 129, 197, 140, 194, 133 };
    const char *tmr = "TIMER   ";

    enterkeyScan();

    // Discard keypad input after 2 minutes of inactivity
    if(millis() - lastKeyPressed >= 2*60*1000) {
        dateBuffer[0] = '\0';
        dateIndex = 0;
    }

    // Bail out if sequence played or device is fake-"off"
    if(!FPBUnitIsOn || startup || timeTravelP0 || timeTravelP1 || timeTravelRE) {

        isEnterKeyHeld = false;
        isEnterKeyPressed = false;
        #ifdef EXTERNAL_TIMETRAVEL_IN
        isEttKeyPressed = false;
        isEttKeyHeld = false;
        #endif

        return;

    }

#ifdef EXTERNAL_TIMETRAVEL_IN
    if(isEttKeyHeld) {
        resetPresentTime();
        isEttKeyPressed = isEttKeyHeld = false;
    } else if(isEttKeyPressed) {
        if(!ettDelay) {
            timeTravel(ettLong, true);
            ettDelayed = false;
        } else {
            ettNow = millis();
            ettDelayed = true;
            startBeepTimer();
        }
        isEttKeyPressed = isEttKeyHeld = false;
    }
    if(ettDelayed) {
        if(millis() - ettNow >= ettDelay) {
            timeTravel(ettLong, true);
            ettDelayed = false;
        }
    }
#endif

    // If enter key is held, go into keypad menu
    if(isEnterKeyHeld) {

        isEnterKeyHeld = false;
        isEnterKeyPressed = false;
        cancelEnterAnim();
        cancelETTAnim();

        timeIndex = yearIndex = 0;
        timeBuffer[0] = '\0';

        menuActive = true;

        enter_menu();

        isEnterKeyHeld = false;
        isEnterKeyPressed = false;

        #ifdef EXTERNAL_TIMETRAVEL_IN
        // No external tt while in menu mode,
        // so reset flag upon menu exit
        isEttKeyPressed = isEttKeyHeld = false;
        #endif

        menuActive = false;

    }

    // if enter key is merely pressed, copy dateBuffer to destination time (if valid)
    if(isEnterKeyPressed) {

        int  strLen = strlen(dateBuffer);
        bool invalidEntry = false;
        bool validEntry = false;
        uint16_t enterInterruptsMusic = 0;

        isEnterKeyPressed = false;
        enterWasPressed = true;

        cancelETTAnim();

        // Turn on white LED
        digitalWrite(WHITE_LED_PIN, HIGH);

        // Turn off destination time
        destinationTime.off();

        timeNow = millis();

        if(strLen != DATELEN_ALL  &&
           strLen != DATELEN_REM  &&
           strLen != DATELEN_DATE &&
           (strLen < DATELEN_CMIN ||
            strLen > DATELEN_CMAX) ) {

            invalidEntry = true;

        } else if(strLen == DATELEN_ALSH) {

            char atxt[16];
            uint16_t flags = 0;
            uint8_t code = atoi(dateBuffer);
            
            if(code == 11) {

                int al = getAlarm();
                if(al >= 0) {
                    const char *alwd = getAlWD(alarmWeekday);
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "%-7s %02d%02d", alwd, al >> 8, al & 0xff);
                    #else
                    sprintf(atxt, "%-8s %02d%02d", alwd, al >> 8, al & 0xff);
                    #endif
                    flags = CDT_COLON;
                } else {
                    #ifdef IS_ACAR_DISPLAY
                    strcpy(atxt, "ALARM  UNSET");
                    #else
                    strcpy(atxt, "ALARM   UNSET");
                    #endif
                }

                destinationTime.showTextDirect(atxt, CDT_CLEAR|flags);
                specDisp = 10;
                validEntry = true;

            } else if(code == 44) {

                if(!ctDown) {
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "%s OFF", tmr);
                    #else
                    sprintf(atxt, "%s  OFF", tmr);
                    #endif
                } else {
                    unsigned long el = ctDown - (millis()-ctDownNow);
                    uint8_t mins = el/(1000*60);
                    uint8_t secs = (el-(mins*1000*60))/1000;
                    if((long)el < 0) mins = secs = 0;
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "%s%02d%02d", tmr, mins, secs);
                    #else
                    sprintf(atxt, "%s %02d%02d", tmr, mins, secs);
                    #endif
                    flags = CDT_COLON;
                }

                destinationTime.showTextDirect(atxt, CDT_CLEAR|flags);
                specDisp = 10;
                validEntry = true;

            } else if(code == 77) {

                uint16_t flags = 0;

                if(!remMonth && !remDay) {
                    buildRemOffString(atxt);
                } else {
                    buildRemString(atxt);
                    flags = CDT_COLON;
                }

                destinationTime.showTextDirect(atxt, CDT_CLEAR|flags);
                specDisp = 10;
                validEntry = true;

            } else if((code == 88 || code == 55) && haveMusic) {

                if(mpActive) {
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "PLAYING  %03d", mp_get_currently_playing());
                    #else
                    sprintf(atxt, "PLAYING   %03d", mp_get_currently_playing());
                    #endif
                } else {
                    strcpy(atxt, "STOPPED");
                }

                destinationTime.showTextDirect(atxt, CDT_CLEAR);
                specDisp = 10;
                validEntry = true;
              
            } else
                invalidEntry = true;

        } else if(strLen == DATELEN_CODE) {

            uint16_t code = atoi(dateBuffer);
            bool rcModeState;
            char atxt[16];
            uint16_t flags = 0;
            
            if(code == 113 && (!haveRcMode || !haveWcMode)) {
                code = haveRcMode ? 111 : 112;
            }
            
            switch(code) {
            #ifdef TC_HAVETEMP
            case 111:               // 111+ENTER: Toggle rc-mode
                if(haveRcMode) {
                    toggleRcMode();
                    if(tempSens.haveHum() || isWcMode()) {
                        departedTime.off();
                        needDepTime = true;
                    }
                    validEntry = true;
                } else {
                    invalidEntry = true;
                }
                break;
            #endif
            case 112:               // 112+ENTER: Toggle wc-mode
                if(haveWcMode) {
                    toggleWcMode();
                    if(WcHaveTZ2 || isRcMode()) {
                        departedTime.off();
                        needDepTime = true;
                    }
                    setupWCMode();
                    destShowAlt = depShowAlt = 0; // Reset TZ-Name-Animation
                    validEntry = true;
                } else {
                    invalidEntry = true;
                }
                break;
            case 113:               // 113+ENTER: Toggle rc+wc mode
                // Dep Time display needed in any case:
                // Either for TZ2 or TEMP
                departedTime.off();
                needDepTime = true;
                rcModeState = toggleRcMode();
                enableWcMode(rcModeState);
                setupWCMode();
                destShowAlt = depShowAlt = 0; // Reset TZ-Name-Animation
                validEntry = true;
                break;
            case 222:               // 222+ENTER: Turn shuffle off
            case 555:               // 555+ENTER: Turn shuffle on
                if(haveMusic) {
                    mp_makeShuffle((code == 555));
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "SHUFFLE  %s", (code == 555) ? " ON" : "OFF");
                    #else
                    sprintf(atxt, "SHUFFLE   %s", (code == 555) ? " ON" : "OFF");
                    #endif
                    destinationTime.showTextDirect(atxt);
                    specDisp = 10;
                    validEntry = true;
                } else {
                    invalidEntry = true;
                }
                break;
            case 888:               // 888+ENTER: Goto song #0
                if(haveMusic) {
                    mp_gotonum(0, mpActive);
                    #ifdef IS_ACAR_DISPLAY
                    strcpy(atxt, "NEXT     000");
                    #else
                    strcpy(atxt, "NEXT      000");
                    #endif
                    destinationTime.showTextDirect(atxt);
                    specDisp = 10;
                    validEntry = true;
                } else {
                    invalidEntry = true;
                }
                break;
            case 440:
                #ifdef IS_ACAR_DISPLAY
                sprintf(atxt, "%s OFF", tmr);
                #else
                sprintf(atxt, "%s  OFF", tmr);
                #endif
                destinationTime.showTextDirect(atxt);
                ctDown = 0;
                specDisp = 10;
                validEntry = true;
                break;
            case 770:
                remMonth = remDay = remHour = remMin = 0;
                saveReminder();
                buildRemOffString(atxt);
                destinationTime.showTextDirect(atxt);
                specDisp = 10;
                validEntry = true;
                break;
            case 777:
                if(!remMonth && !remDay) {
                  
                    buildRemOffString(atxt);
                    
                } else {
                  
                    // This does not take DST into account if the next reminder
                    // is due in the following year. Calculation is off by tzDiff
                    // (one hour) if DST borders are crossed for an odd number of
                    // times.
                    DateTime dt;
                    myrtcnow(dt);
                    int  yr = dt.year() - presentTime.getYearOffset();
                    bool sameYear = true;
                    int  locDST = 0;
                    uint32_t locMins = mins2Date(yr, dt.month(), dt.day(), dt.hour(), dt.minute());
                    uint32_t tgtMins = mins2Date(yr, remMonth ? remMonth : dt.month(), remDay, remHour, remMin);
                    if(tgtMins < locMins) {
                        if(remMonth) {
                            tgtMins = mins2Date(yr + 1, remMonth, remDay, remHour, remMin);
                            tgtMins += (365*24*60);
                            if(isLeapYear(yr)) tgtMins += 24*60;
                            sameYear = false;
                        } else {
                            if(dt.month() == 12) {
                                tgtMins = mins2Date(yr + 1, 1, remDay, remHour, remMin);
                                tgtMins += (365*24*60);
                                if(isLeapYear(yr)) tgtMins += 24*60;
                                sameYear = false;
                            } else {
                                tgtMins = mins2Date(yr, dt.month() + 1, remDay, remHour, remMin);
                            }
                        }
                    }
                    tgtMins -= locMins;
                    if(sameYear && couldDST[0] && ((locDST = presentTime.getDST()) >= 0)) {
                        int curMins;
                        int tgtDST = timeIsDST(0, yr, remMonth ? remMonth : dt.month() + 1, remDay, remHour, remMin, curMins);
                        if(!locDST && tgtDST)      tgtMins += getTzDiff();
                        else if(locDST && !tgtDST) tgtMins -= getTzDiff();
                    }
                    uint16_t days = tgtMins / (24*60);
                    uint16_t hours = (tgtMins % (24*60)) / 60;
                    uint16_t minutes = tgtMins - (days*24*60) - (hours*60);

                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "    %3dd%2d%02d", days, hours, minutes);
                    #else
                    sprintf(atxt, "     %3dd%2d%02d", days, hours, minutes);
                    #endif
                    flags = CDT_COLON;
                }
                destinationTime.showTextDirect(atxt, CDT_CLEAR|flags);
                specDisp = 10;
                validEntry = true;
                break;
            case 000:
            case 001:
            case 002:
            case 003:
                setBeepMode(code);
                #ifdef IS_ACAR_DISPLAY
                sprintf(atxt, "BEEP MODE  %1d", beepMode);
                #else
                sprintf(atxt, "BEEP MODE   %1d", beepMode);
                #endif
                destinationTime.showTextDirect(atxt);
                enterDelay = ENTER_DELAY;
                specDisp = 10;
                // Play no sound, ie no xxvalidEntry
                break;
            default:
                invalidEntry = true;
            }

        } else if(strLen == DATELEN_INT) {

            if(!(strncmp(dateBuffer, "64738", 5))) {
                mp_stop();
                stopAudio();
                allOff();
                #ifdef TC_HAVESPEEDO
                if(useSpeedo) speedo.off();
                #endif
                destinationTime.resetBrightness();
                destinationTime.showTextDirect("REBOOTING");
                destinationTime.on();
                delay(ENTER_DELAY);
                digitalWrite(WHITE_LED_PIN, LOW);
                esp_restart();
            }

            invalidEntry = true;

        } else if(strLen == DATELEN_QALM) {

            char atxt[16];
            uint8_t code = read2digs(0);

            if(code == 11) {

                uint8_t aHour, aMin;

                aHour = read2digs(2);
                aMin  = read2digs(4);
                if(aHour <= 23 && aMin <= 59) {
                    const char *alwd = getAlWD(alarmWeekday);
                    if( (alarmHour != aHour)  ||
                        (alarmMinute != aMin) ||
                        !alarmOnOff ) {
                        alarmHour = aHour;
                        alarmMinute = aMin;
                        alarmOnOff = true;
                        saveAlarm();
                    }
                    #ifdef IS_ACAR_DISPLAY
                    sprintf(atxt, "%-7s %02d%02d", alwd, alarmHour, alarmMinute);
                    #else
                    sprintf(atxt, "%-8s %02d%02d", alwd, alarmHour, alarmMinute);
                    #endif
                    destinationTime.showTextDirect(atxt, CDT_COLON);
                    specDisp = 10;
                    validEntry = true;
                } else {
                    invalidEntry = true;
                }

            } else if(code == 77) {

                uint8_t sMon  = read2digs(2);
                uint8_t sDay  = read2digs(4);

                if((sMon <= 12) && (sDay >= 1 && sDay <= 31)) {

                    if(!sMon || sDay <= daysInMonth(sMon, 2000)) {

                        if( (remMonth != sMon) || (remDay != sDay) ) {
                        
                            remMonth = sMon;
                            remDay = sDay;

                            // If current hr and min are zero
                            // assume unset, set default 9am.
                            if(!remHour && !remMin) {
                                remHour = 9;
                            }
                            
                            saveReminder();
                        }

                        buildRemString(atxt);

                        destinationTime.showTextDirect(atxt, CDT_CLEAR|CDT_COLON);
                        specDisp = 10;

                        validEntry = true;
                    }
                } 

                invalidEntry = !validEntry; 
            
            } else if(haveMusic && !strncmp(dateBuffer, "888", 3)) {

                uint16_t num = ((dateBuffer[3] - '0') * 100) + read2digs(4);
                num = mp_gotonum(num, mpActive);
                #ifdef IS_ACAR_DISPLAY
                sprintf(atxt, "NEXT     %03d", num);
                #else
                sprintf(atxt, "NEXT      %03d", num);
                #endif
                destinationTime.showTextDirect(atxt);
                specDisp = 10;
                validEntry = true;

            } else {

                invalidEntry = true;

            }

        } else if(strLen == DATELEN_TIME && read2digs(0) == 44) {

            char atxt[16];
            uint8_t mins;
            uint16_t flags = 0;
            
            mins = read2digs(2);
            if(!mins) {
                #ifdef IS_ACAR_DISPLAY
                sprintf(atxt, "%s OFF", tmr);
                #else
                sprintf(atxt, "%s  OFF", tmr);
                #endif
                ctDown = 0;
            } else {
                #ifdef IS_ACAR_DISPLAY
                sprintf(atxt, "%s%02d00", tmr, mins);
                #else
                sprintf(atxt, "%s %02d00", tmr, mins);
                #endif
                ctDown = mins * 60 * 1000;
                ctDownNow = millis();
                flags = CDT_COLON;
            }

            destinationTime.showTextDirect(atxt, CDT_CLEAR|flags);
            specDisp = 10;
            validEntry = true;

        } else if(strLen == DATELEN_REM) {

            if(read2digs(0) == 77) {

                char atxt[16];

                uint8_t sMon  = read2digs(2);
                uint8_t sDay  = read2digs(4);
                uint8_t sHour = read2digs(6);
                uint8_t sMin  = read2digs(8);

                if((sMon <= 12) && (sDay >= 1 && sDay <= 31) && (sHour <= 23) && (sMin <= 59)) {

                    if(!sMon || sDay <= daysInMonth(sMon, 2000)) {

                        if( (remMonth != sMon) || (remDay != sDay) || 
                            (remHour != sHour) || (remMin != sMin) ) {
                        
                            remMonth = sMon;
                            remDay = sDay;
                            remHour = sHour;
                            remMin = sMin;
                        
                            saveReminder();
                        }

                        buildRemString(atxt);

                        destinationTime.showTextDirect(atxt, CDT_CLEAR|CDT_COLON);
                        specDisp = 10;

                        validEntry = true;
                    }
                } 
            } 
            
            invalidEntry = !validEntry;

        } else {

            int _setMonth = -1, _setDay = -1, _setYear = -1;
            int _setHour = -1, _setMin = -1, temp1, temp2;
            int special = 0;
            uint32_t spTmp;
            #ifdef IS_ACAR_DISPLAY
            #define EE1_KL1 12
            char spTxtS1[EE1_KL1] = { 207, 254, 206, 255, 206, 247, 206, 247, 199, 247, 207, 247 };
            #else
            #define EE1_KL1 13
            char spTxtS1[EE1_KL1] = { 181, 244, 186, 138, 187, 138, 179, 138, 179, 131, 179, 139, 179 };
            #endif

            #ifdef TC_DBG
            Serial.printf("Date entered: [%s]\n", dateBuffer);
            #endif

            temp1 = read2digs(0);
            temp2 = read2digs(2);
            
            // Convert dateBuffer to date
            if(strLen == DATELEN_TIME) {
                _setHour  = temp1;
                _setMin   = temp2;
            } else {
                _setMonth = temp1;
                _setDay   = temp2;
                _setYear  = ((int)read2digs(4) * 100) + read2digs(6);
                if(strLen == DATELEN_ALL) {
                    _setHour = read2digs(8);
                    _setMin  = read2digs(10);
                }

                // Check month
                if (_setMonth < 1)  _setMonth = 1;
                if (_setMonth > 12) _setMonth = 12;

                // Check day
                if(_setDay < 1)     _setDay = 1;
                if(_setDay > daysInMonth(_setMonth, _setYear)) {
                    // set to max day in that month
                    _setDay = daysInMonth(_setMonth, _setYear); 
                }

                // Year: There is no year "0", for crying out loud.
                // Having said that, we allow it anyway, let the people have
                // the full movie experience.
                //if(_setYear < 1) _setYear = 1;

                spTmp = (uint32_t)_setYear << 16 | _setMonth << 8 | _setDay;
                if((spTmp ^ getHrs1KYrs(7)) == 70667637) {
                    special = 1;
                    spTxt[EE1_KL1] = '\0';
                    for(int i = EE1_KL1-1; i >= 0; i--) {
                        spTxt[i] = spTxtS1[i] ^ (i == 0 ? 0xff : spTxtS1[i-1]);
                    }
                } else if((spTmp ^ getHrs1KYrs(8)) == 59572453)  {
                    if(_setHour >= 9 && _setHour <= 12) {
                        special = 2;
                    }
                } else if((spTmp ^ getHrs1KYrs(6)) == 97681642)  {
                    special = 3;
                } else if((spTmp ^ getHrs1KYrs(8)) == 65998071)  {
                    special = 4;
                }
            }

            // Hour and min are checked in clockdisplay

            // Normal date/time: ENTER-sound interrupts musicplayer
            enterInterruptsMusic = PA_INTRMUS;

            switch(special) {
            case 1:
                destinationTime.showTextDirect(spTxt, CDT_CLEAR|CDT_COLON);
                specDisp = 1;
                validEntry = true;
                break;
            case 2:
                play_file("/ee2.mp3", PA_CHECKNM|PA_INTRMUS);
                enterDelay = EE2_DELAY;
                break;
            case 3:
                play_file("/ee3.mp3", PA_CHECKNM|PA_INTRMUS);
                enterDelay = EE3_DELAY;
                break;
            case 4:
                play_file("/ee4.mp3", PA_CHECKNM|PA_INTRMUS);
                enterDelay = EE4_DELAY;
                break;
            default:
                validEntry = true;
            }

            // Copy date to destination time
            if(_setYear >= 0) destinationTime.setYear(_setYear);   // ny0: >
            if(_setMonth > 0) destinationTime.setMonth(_setMonth);
            if(_setDay > 0)   destinationTime.setDay(_setDay);
            if(_setHour >= 0) destinationTime.setHour(_setHour);
            if(_setMin >= 0)  destinationTime.setMinute(_setMin);

            // We only save the new time to NVM if user wants persistence.
            // Might not be preferred; first, this messes with the user's custom
            // times. Secondly, it wears the flash memory.
            if(timetravelPersistent) {
                destinationTime.save();
            }

            // Disable rc&wc modes
            #ifdef TC_HAVETEMP
            if(isRcMode() && (tempSens.haveHum() || isWcMode())) {
                departedTime.off();
                needDepTime = true;
            }
            #endif
            enableRcMode(false);
            if(isWcMode() && WcHaveTZ1) {
                // If WC mode is enabled and we have a TZ for red display,
                // we need to disable WC mode in order to keep the new dest
                // time on display. In that case, and if we have a TZ for the
                // yellow display, we also restore the yellow time to either 
                // the stored value or the current auto-int step, otherwise 
                // the current yellow WC time would remain but become stale,
                // which is confusing.
                // If there is no TZ for red display, no need to disable WC
                // mode at this time; let timetravel() take care of this.
                if(WcHaveTZ2) {
                    // Restore NVM time if either time cycling is off, or
                    // if paused; latter only if we have the last
                    // time stored. Otherwise we have no previous time.
                    if(autoTimeIntervals[autoInterval] == 0 || (timetravelPersistent && checkIfAutoPaused())) {
                        departedTime.load();
                    } else {
                        departedTime.setFromStruct(&departedTimes[autoTime]);
                    }
                    departedTime.off();
                    needDepTime = true;
                }
                enableWcMode(false);
            }

            // Pause autoInterval-cycling so user can play undisturbed
            pauseAuto();

            // Beep auto mode: Restart timer
            startBeepTimer();
        }

        if(validEntry) {
            play_file("/enter.mp3", PA_CHECKNM|enterInterruptsMusic|PA_ALLOWSD);
            enterDelay = ENTER_DELAY;
        } else if(invalidEntry) {
            play_file("/baddate.mp3", PA_CHECKNM|enterInterruptsMusic|PA_ALLOWSD);
            enterDelay = BADDATE_DELAY;
            if(!enterInterruptsMusic && mpActive) {
                destinationTime.showTextDirect("ERROR", CDT_CLEAR);
                specDisp = 10;
            }
        }

        // Prepare for next input
        dateIndex = 0;
        dateBuffer[0] = '\0';
    }

    // Turn everything back on after entering date
    // (might happen in next iteration of loop)

    if(enterWasPressed && (millis() - timeNow > enterDelay)) {

        switch(specDisp) {
        case 0:
            break;
        case 2:
        case 10:
            specDisp++;
            if(specDisp == 3) destinationTime.onCond();
            else              { destinationTime.resetBrightness(); destinationTime.on(); }
            digitalWrite(WHITE_LED_PIN, LOW);
            timeNow = millis();
            enterWasPressed = true;
            enterDelay = (specDisp == 3) ? EE1_DELAY2 : SPEC_DELAY;
            break;
        case 3:
            specDisp++;
            spTxt[EE1_KL2] = '\0';
            for(int i = EE1_KL2-1; i >= 0; i--) {
                spTxt[i] = spTxtS2[i] ^ (i == 0 ? 0xff : spTxtS2[i-1]);
            }
            destinationTime.showTextDirect(spTxt);
            timeNow = millis();
            enterWasPressed = true;
            enterDelay = EE1_DELAY3;
            play_file("/ee1.mp3", PA_CHECKNM|PA_INTRMUS);
            break;
        case 4:
        case 11:
            specDisp = 0;
            break;
        default:
            specDisp++;
        }

        if(!specDisp) {

            #ifdef TC_HAVEMQTT
            // We overwrite dest time display here, so restart 
            // MQTT message afterwards.
            if(mqttDisp) {
                mqttOldDisp = mqttIdx = 0;
            }
            #endif

            // Animate display

            #ifdef TC_HAVETEMP
            if(isRcMode()) {

                if(!isWcMode() || !WcHaveTZ1) {
                    destinationTime.showTempDirect(tempSens.readLastTemp(), tempUnit, true);
                } else {
                    destinationTime.showAnimate1();
                }
                if(needDepTime) {
                    if(isWcMode() && WcHaveTZ1) {
                        departedTime.showTempDirect(tempSens.readLastTemp(), tempUnit, true);
                    } else if(!isWcMode() && tempSens.haveHum()) {
                        departedTime.showHumDirect(tempSens.readHum(), true);
                    } else {
                        departedTime.showAnimate1();
                    }
                }

                mydelay(80);

                if(!isWcMode() || !WcHaveTZ1) {
                    destinationTime.showTempDirect(tempSens.readLastTemp(), tempUnit);
                } else {
                    destinationTime.showAnimate2();
                }
                if(needDepTime) {
                    if(isWcMode() && WcHaveTZ1) {
                        departedTime.showTempDirect(tempSens.readLastTemp(), tempUnit);
                    } else if(!isWcMode() && tempSens.haveHum()) {
                        departedTime.showHumDirect(tempSens.readHum());
                    } else {
                        departedTime.showAnimate2();
                    }
                }
              
            } else {
            #endif

                destinationTime.showAnimate1();
                if(needDepTime) {
                    departedTime.showAnimate1();
                }
                mydelay(80);
                destinationTime.showAnimate2();
                if(needDepTime) {
                    departedTime.showAnimate2();
                }

            #ifdef TC_HAVETEMP
            }
            #endif

            destShowAlt = depShowAlt = 0;     // Reset TZ-Name-Animation

            digitalWrite(WHITE_LED_PIN, LOW); // turn off white LED

            enterWasPressed = false;          // reset flags

            needDepTime = false;

        }

    }
}

void cancelEnterAnim(bool reenableDT)
{
    if(enterWasPressed) {
        enterWasPressed = false;
        
        digitalWrite(WHITE_LED_PIN, LOW);
        
        if(reenableDT) {
            #ifdef TC_HAVETEMP
            if(isRcMode() && (!isWcMode() || !WcHaveTZ1)) {
                destinationTime.showTempDirect(tempSens.readLastTemp(), tempUnit);
            } else
            #endif
                destinationTime.show();
            destinationTime.onCond();
            if(needDepTime) {
                #ifdef TC_HAVETEMP
                if(isRcMode()) {
                    if(isWcMode() && WcHaveTZ1) {
                        departedTime.showTempDirect(tempSens.readLastTemp(), tempUnit);
                    } else if(!isWcMode() && tempSens.haveHum()) {
                        departedTime.showHumDirect(tempSens.readHum());
                    } else {
                        departedTime.show();
                    }
                } else
                #endif
                    departedTime.show();
                departedTime.onCond();
            }
        }
        
        needDepTime = false;
        specDisp = 0;
    }
}

void cancelETTAnim()
{
    #ifdef EXTERNAL_TIMETRAVEL_IN
    ettDelayed = false;
    #endif
}

bool keypadIsIdle()
{
    return (!lastKeyPressed || (millis() - lastKeyPressed >= 2*60*1000));
}

static void setupWCMode()
{
    if(isWcMode()) {
        DateTime dt;
        myrtcnow(dt);
        setDatesTimesWC(dt);
    } else if(autoTimeIntervals[autoInterval] == 0 || (timetravelPersistent && checkIfAutoPaused())) {
        // Restore NVM time if either time cycling is off, or
        // if paused; latter only if we have the last
        // time stored. Otherwise we have no previous time.
        if(WcHaveTZ1) destinationTime.load();
        if(WcHaveTZ2) departedTime.load();
    } else {
        if(WcHaveTZ1) destinationTime.setFromStruct(&destinationTimes[autoTime]);
        if(WcHaveTZ2) departedTime.setFromStruct(&departedTimes[autoTime]);
    }
}

static void buildRemString(char *buf)
{
    if(remMonth) {
        #ifdef IS_ACAR_DISPLAY
        sprintf(buf, "%02d%02d    %02d%02d", remMonth, remDay, remHour, remMin);
        #else
        sprintf(buf, "%3s%02d    %02d%02d", destinationTime.getMonthString(remMonth), 
                                         remDay, remHour, remMin);
        #endif
    } else {
        #ifdef IS_ACAR_DISPLAY
        sprintf(buf, "  %02d    %02d%02d", remDay, remHour, remMin);
        #else
        sprintf(buf, "   %02d    %02d%02d", remDay, remHour, remMin);
        #endif
    }
}

static void buildRemOffString(char *buf)
{
    #ifdef IS_ACAR_DISPLAY
    strcpy(buf, "REMINDER OFF");
    #else
    strcpy(buf, "REMINDER  OFF");
    #endif
}

/*
 * Custom delay function for key scan in keypad_i2c
 */
static void mykpddelay(unsigned int mydel)
{
    unsigned long startNow = millis();
    audio_loop();
    while(millis() - startNow < mydel) {
        delay(1);
        ntp_short_loop();
        audio_loop();
    }
}

/*
 * Beep
 */

void setBeepMode(int mode)
{
    switch(mode) {
    case 0:
        muteBeep = true;
        beepMode = 0;
        beepTimer = false;
        break;
    case 1:
        muteBeep = false;
        beepMode = 1;
        beepTimer = false;
        break;
    case 2:
        if(beepMode == 1) {
            beepTimerNow = millis();
            beepTimer = true;
        }
        beepMode = 2;
        beepTimeout = BEEPM2_SECS*1000;
        break;
    case 3:
        if(beepMode == 1) {
            beepTimerNow = millis();
            beepTimer = true;
        }
        beepMode = 3;
        beepTimeout = BEEPM3_SECS*1000;
        break;
    }
}
         
/*
 * Un-mute beep and start beep timer
 */
void startBeepTimer()
{
    if(beepMode >= 2) {
        beepTimer = true;
        beepTimerNow = millis();
        muteBeep = false;
    }
}

/*
 * Night mode
 */

static void setNightMode(bool nm)
{
    destinationTime.setNightMode(nm);
    presentTime.setNightMode(nm);
    departedTime.setNightMode(nm);
    #ifdef TC_HAVESPEEDO
    if(useSpeedo) speedo.setNightMode(nm);
    #endif
}

void nightModeOn()
{
    setNightMode(true);
    leds_off();
}

void nightModeOff()
{
    setNightMode(false);
    leds_on();
}

bool toggleNightMode()
{
    if(destinationTime.getNightMode()) {
        nightModeOff();
        return false;
    }
    nightModeOn();
    return true;
}

/*
 * LEDs (TCD control board 1.3)
 */
 
void leds_on()
{
    if(FPBUnitIsOn && !destinationTime.getNightMode()) {
        digitalWrite(LEDS_PIN, HIGH);
    }
}

void leds_off()
{
    digitalWrite(LEDS_PIN, LOW);
}
