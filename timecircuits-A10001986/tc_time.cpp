/*
 * -------------------------------------------------------------------
 * CircuitSetup.us Time Circuits Display
 * 
 * Code based on Marmoset Electronics 
 * https://www.marmosetelectronics.com/time-circuits-clock
 * by John Monaco
 *
 * Enhanced/modified/written in 2022 by Thomas Winischhofer (A10001986)
 * -------------------------------------------------------------------
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * 
 */


/* Time travel:
 *  
 * Time travel works as follows:
 * 
 * To activate the time travel function, hold "0" on the keypad for 2 seconds. A sound
 * will activate, and you will travel in time: The "destination time" is now "present time", 
 * and your old present time is now stored in the "last time departured".
 * In order to select a new destination time, enter a date and a time through the keypad, 
 * either mmddyyyy, mmddyyyyhhmm or hhmm, then press ENTER. Note that there is no visual  
 * feed back while typing, like in the movie.
 * If the date or time is invalid, the sound will hint you to this.
 * 
 * To return to actual present time, hold "9" for 2 seconds.
 */

#include "tc_time.h"

bool autoIntDone = false;
bool autoReadjust = false;
bool alarmDone = false;
bool hourlySoundDone = false;
int8_t minNext;  

bool x;  // for tracking second changes
bool y;  

// The startup sequence
bool startup              = false;
bool startupSound         = false;
unsigned long startupNow  = 0;

// Pause autoInterval if user played with time travel
unsigned long pauseNow;                 
unsigned long pauseDelay = 30*60*1000;  // Pause for 30 minutes
bool          autoPaused = false;

// The timetravel sequence
unsigned long timetravelP1Now = 0;
unsigned long timetravelP1Delay = 0;
int           timeTravelP1 = 0;

// The timetravel re-entry sequence
unsigned long timetravelNow = 0;
bool          timeTraveled = false;

struct tm _timeinfo;  //for NTP
RTC_DS3231 rtc;       //for RTC IC

// For displaying times off the real time
uint64_t timeDifference = 0;
bool     timeDiffUp = false;  // true = add difference, false = subtract difference

// Persistent time travels:
// This controls the app's behavior as regards saving times to the EEPROM.
// If this is true, times are saved to the EEPROM, whenever
//  - the user enters a destination time for time travel and presses ENTER
//  - the user activates time travel (hold "0") 
//  - the user returns from a time travel (hold "9")
// True means that playing around with time travel is persistent, and even
// present time is kept over a power loss (if the battery backed RTC keeps
// the time). Downside is that the user's custom destination and last
// departure times are overwritten during a time travel.
// False means that time travel games are non-persistent, and the user's
// custom times (as set up in the keypad menu) are never overwritten.
// Also, "false" reduces flash wear considerably.
bool timetravelPersistent = true;

// Alarm/HourlySound based on RTC (or presentTime's display if false)
bool alarmRTC = true;

// For tracking idle time in menus
uint8_t timeout = 0;  

// The display objects
clockDisplay destinationTime(DEST_TIME_ADDR, DEST_TIME_PREF);
clockDisplay presentTime(PRES_TIME_ADDR, PRES_TIME_PREF);
clockDisplay departedTime(DEPT_TIME_ADDR, DEPT_TIME_PREF);

// Automatic times ("decorative mode")

int8_t autoTime = 0;  // Selects from time from array below

#ifndef TWPRIVATE //  ----------------- OFFICIAL

dateStruct destinationTimes[8] = {
    //YEAR, MONTH, DAY, HOUR, MIN    
    {1985, 10, 26,  1, 21},
    {1985, 10, 26,  1, 24},
    {1955, 11,  5,  6,  0},
    {1985, 10, 27, 11,  0},
    {2015, 10, 21, 16, 29},
    {1955, 11, 12,  6,  0},
    {1885,  1,  1,  0,  0},
    {1885,  9,  2, 12,  0}
}; 
dateStruct departedTimes[8] = {
    {1985, 10, 26,  1, 20},
    {1955, 11, 12, 22,  4},
    {1985, 10, 26,  1, 34},
    {1885,  9,  7,  9, 10},
    {1985, 10, 26, 11, 35},
    {1985, 10, 27,  2, 42},
    {1955, 11, 12, 21, 44},
    {1955, 11, 13, 12,  0}
};

#else //  --------------------------- TWPRIVATE

dateStruct destinationTimes[8] = {
    //YEAR, MONTH, DAY, HOUR, MIN
    {1985,  7, 23, 20,  1},       
    {1985, 11, 23, 16, 24},   
    {1986,  5, 26, 14, 12},    
    {1986,  8, 23, 11,  0},     
    {1986, 12, 24, 21, 22},   
    {1987,  3, 20, 19, 31},    
    {1987,  5, 26,  0,  0},      
    {1988, 12, 24, 22, 31}
}; 
dateStruct departedTimes[8] = {
    {2017,  7, 11, 10, 11},       
    {1988,  6,  3, 15, 30},    
    {1943,  3, 15,  7, 47},     
    {2016,  6, 22, 16, 11},    
    {1982,  5, 15,  9, 41},     
    {1943, 11, 25, 11, 11},   
    {1970,  5, 26,  8, 22},     
    {2021,  5,  5, 10,  9}
};    

#endif  // ------------------------------------

#ifdef FAKE_POWER_ON
OneButton fakePowerOnKey = OneButton(FAKE_POWER_BUTTON_PIN,
    true,    // Button is active LOW
    true     // Enable internal pull-up resistor
);
bool isFPBKeyChange = false;
bool isFPBKeyPressed = false;
bool waitForFakePowerButton = false;
#endif
bool FPBUnitIsOn = true;

const uint8_t monthDays[] = { 
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};
const unsigned int mon_yday[2][13] =
{
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 }, 
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};
const uint64_t mins1kYears[] = 
{
               0,  525074400, 1050674400, 1576274400, 2101874400, 
      2627474400, 3153074400, 3678674400, 4204274400, 4729874400 
};
const uint32_t hours1kYears[] = 
{
                  0,  525074400/60, 1050674400/60, 1576274400/60, 2101874400/60, 
      2627474400/60, 3153074400/60, 3678674400/60, 4204274400/60, 4729874400/60 
};

/*
 * time_boot()
 * 
 */
void time_boot() 
{
    // Start the displays early to clear them
    presentTime.begin();
    destinationTime.begin();
    departedTime.begin();
}

/*
 * time_setup()
 * 
 */
void time_setup() 
{
    bool validLoad = true;
    bool rtcbad = false;
    
    pinMode(SECONDS_IN_PIN, INPUT_PULLDOWN);  // for monitoring seconds 
    
    pinMode(STATUS_LED_PIN, OUTPUT);          // Status LED

    #ifdef FAKE_POWER_ON
    waitForFakePowerButton = ((int)atoi(settings.fakePwrOn) > 0) ? true : false;
    
    if(waitForFakePowerButton) {                        
        fakePowerOnKey.setClickTicks(10);     // millisec after single click is assumed (default 400)
        fakePowerOnKey.setPressTicks(50);     // millisec after press is assumed (default 800)        
        fakePowerOnKey.setDebounceTicks(50);  // millisec after safe click is assumed (default 50)
        fakePowerOnKey.attachLongPressStart(fpbKeyPressed);
        fakePowerOnKey.attachLongPressStop(fpbKeyLongPressStop);
    }
    #endif
    
    // RTC setup
    if(!rtc.begin()) {
        
        Serial.println(F("time_setup: Couldn't find RTC. Panic!"));
        
        // Setup pins for white LED
        pinMode(WHITE_LED_PIN, OUTPUT);

        // Blink forever
        while (1) {
            digitalWrite(WHITE_LED_PIN, HIGH);  
            delay(1000);
            digitalWrite(WHITE_LED_PIN, LOW);  
            delay(1000);          
        }           
        
    }

    if(rtc.lostPower() && WiFi.status() != WL_CONNECTED) {
      
        // Lost power and battery didn't keep time, so set current time to compile time
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        
        Serial.println(F("time_setup: RTC lost power, setting default time. Change battery!"));

        rtcbad = true;
    
    }

    RTCClockOutEnable();  // Turn on the 1Hz second output

    // Start the displays
    presentTime.begin();
    destinationTime.begin();
    departedTime.begin();

    // Initialize clock mode: 12 hour vs 24 hour
    bool mymode24 = (int)atoi(settings.mode24) ? true : false;
    presentTime.set1224(mymode24);
    destinationTime.set1224(mymode24);
    departedTime.set1224(mymode24);

    // Configure presentTime as a display that will hold real time
    presentTime.setRTC(true);  

    // Determine if user wanted Time Travels to be persistent
    timetravelPersistent = (int)atoi(settings.timesPers) ? true : false;

    // Load present time settings (yearOffs, timeDifference)
    presentTime.load();
    if(!timetravelPersistent) {
        timeDifference = 0;
    } 
    
    if(rtcbad) {        
        presentTime.setYearOffset(0);
        timeDifference = 0;
        // FIXME  .. anything else?
    }

    // Set RTC with NTP time
    if(getNTPTime()) {

        #ifdef TC_DBG
        Serial.print(F("time_setup: RTC set through NTP from "));
        Serial.println(settings.ntpServer);
        #endif

        // Save YearOffs to EEPROM if change is detected
        if(presentTime.getYearOffset() != presentTime.loadYOffs()) {
            presentTime.save();
        } 
        
    }

    // Load destination time (and set to default if invalid)
    if(!destinationTime.load()) {
        validLoad = false;  
        destinationTime.setYearOffset(0);
        destinationTime.setYear(destinationTimes[0].year);   
        destinationTime.setMonth(destinationTimes[0].month);
        destinationTime.setDay(destinationTimes[0].day);             
        destinationTime.setHour(destinationTimes[0].hour);
        destinationTime.setMinute(destinationTimes[0].minute);                   
        destinationTime.setBrightness((int)atoi(settings.destTimeBright));
        destinationTime.save();
    }

    // Load departed time (and set to default if invalid)
    if(!departedTime.load()) {
        validLoad = false; 
        departedTime.setYearOffset(0);
        departedTime.setYear(departedTimes[0].year);
        departedTime.setMonth(departedTimes[0].month);
        departedTime.setDay(departedTimes[0].day);        
        departedTime.setHour(departedTimes[0].hour);
        departedTime.setMinute(departedTimes[0].minute);     
        departedTime.setBrightness((int)atoi(settings.lastTimeBright));
        departedTime.save();
    }

    // Load autoInterval ("time rotation interval") from settings 
    loadAutoInterval();

    // Load alarm from alarmconfig file
    // Don't care if data invalid, alarm is off in that case
    loadAlarm();

    // If using auto times, load the first one
    if(autoTimeIntervals[autoInterval]) {                    
        destinationTime.setFromStruct(&destinationTimes[0]); 
        departedTime.setFromStruct(&departedTimes[0]);
        #ifdef TC_DBG
        Serial.println(F("time_setup: autointerval enabled"));
        #endif
    }

    alarmRTC = ((int)atoi(settings.alarmRTC) > 0) ? true : false;

    // Show "RESET" message if data loaded was invalid somehow
    if(!validLoad) {      
        destinationTime.showOnlyText("RESET");         
        delay(1000);
        allOff();
    }

    // Show "BATT" message if RTC battery is depleted
    if(rtcbad) {      
        destinationTime.showOnlyText("BATT");
        delay(3000);
        allOff();
    }

    if((int)atoi(settings.playIntro)) {
        const char *t1 = "             BACK";
        const char *t2 = "TO";
        const char *t3 = "THE FUTURE";  
        int oldBriDest = destinationTime.getBrightness();
        int oldBriPres = presentTime.getBrightness();
        int oldBriDep = departedTime.getBrightness();

        play_file("/intro.mp3", 1.0, true, 0);

        my2delay(1200);
        destinationTime.setBrightness(15);
        presentTime.setBrightness(0);
        departedTime.setBrightness(0);
        presentTime.off();
        departedTime.off();
        destinationTime.showOnlyText(t1);
        presentTime.showOnlyText(t2);
        departedTime.showOnlyText(t3);
        destinationTime.on();
        for(int i = 0; i < 14; i++) {
           my2delay(50);
           destinationTime.showOnlyText(&t1[i]);
        }
        my2delay(500);                                
        presentTime.on();
        departedTime.on();
        for(int i = 0; i <= 15; i++) {
            presentTime.setBrightness(i);
            departedTime.setBrightness(i);
            my2delay(100);
        }
        my2delay(1500);
        for(int i = 15; i >= 0; i--) {
            destinationTime.setBrightness(i);
            presentTime.setBrightness(i);
            departedTime.setBrightness(i);
            my2delay(20);
        }
        allOff();
        destinationTime.setBrightness(oldBriDest);
        presentTime.setBrightness(oldBriPres);
        departedTime.setBrightness(oldBriDep);

        waitAudioDoneIntro();
        stopAudio();
    }

    // Load the time for initial animation show
    presentTime.setDateTimeDiff(myrtcnow()); 

#ifdef FAKE_POWER_ON    
    if(waitForFakePowerButton) { 
        digitalWrite(WHITE_LED_PIN, HIGH);  
        delay(500);
        digitalWrite(WHITE_LED_PIN, LOW); 
        isFPBKeyChange = false; 
        FPBUnitIsOn = false; 
           
        #ifdef TC_DBG
        Serial.println(F("time_setup: waiting for fake power on"));
        #endif             
        
    } else {
#endif
        
        startup = true;
        startupSound = true; 
        FPBUnitIsOn = true;  
         
#ifdef FAKE_POWER_ON
    }
#endif

    #ifdef TC_DBG
    Serial.println(F("time_setup: Done."));
    #endif  
    
}

/*
 * time_loop()
 * 
 */
void time_loop() 
{              
    #ifdef TC_DBG
    int dbgLastMin;
    #endif

    #ifdef FAKE_POWER_ON
    if(waitForFakePowerButton) { 
        fakePowerOnKey.tick();
        
        if(isFPBKeyChange) {
            if(isFPBKeyPressed) {
                if(!FPBUnitIsOn) {                                 
                    startup = true;
                    startupSound = true;                    
                    FPBUnitIsOn = true;                    
                }
            } else {
                if(FPBUnitIsOn) {       
                    startup = false;
                    startupSound = false; 
                    timeTraveled = false;  
                    timeTravelP1 = 0;            
                    FPBUnitIsOn = false;
                    allOff();
                    stopAudio();
                }
            }
            isFPBKeyChange = false;
        }
    }
    #endif

    // Initiate startup delay, play startup sound
    if(startupSound) {
        startupNow = millis();
        play_file("/startup.mp3", 1.0, true, 0);
        startupSound = false;        
    }

    // Turn display on after startup delay
    if(startup && (millis() - startupNow >= STARTUP_DELAY)) {        
        animate();
        startup = false;        
        #ifdef TC_DBG
        Serial.println(F("time_loop: Startup animate triggered"));
        #endif
    }

    // Time travel animation
    if(timeTravelP1 && (millis() - timetravelP1Now >= timetravelP1Delay)) {  
        timeTravelP1++;
        timetravelP1Now = millis();
        switch(timeTravelP1) {
        case 2:            
            allOff();
            timetravelP1Delay = TT_P1_DELAY_P2;
            #ifdef TC_DBG
            Serial.println(F("long time travel phase 2"));
            #endif
            break;
        case 3:
            timetravelP1Delay = TT_P1_DELAY_P3;
            #ifdef TC_DBG
            Serial.println(F("long time travel phase 3"));
            #endif
            break;
        case 4:
            timetravelP1Delay = TT_P1_DELAY_P4;
            #ifdef TC_DBG
            Serial.println(F("long time travel phase 4"));
            #endif
            break;
        case 5:
            timetravelP1Delay = TT_P1_DELAY_P5;
            #ifdef TC_DBG
            Serial.println(F("long time travel phase 5"));
            #endif
            break;
        default:
            #ifdef TC_DBG
            Serial.println(F("long time travel phase 6 - re-entry"));
            #endif
            timeTravelP1 = 0;
            timeTravel(false);
        }
    }

    // Turn display back on after time traveling
    if(timeTraveled && (millis() - timetravelNow >= TIMETRAVEL_DELAY)) {                
        animate();
        timeTraveled = false;
        #ifdef TC_DBG
        Serial.println(F("time_loop: Display on after time travel"));
        #endif
    }

    y = digitalRead(SECONDS_IN_PIN);
    if(y != x) {      // different on half second
        if(y == 0) {  // flash colon on half seconds, lit on start of second

            // First, handle colon
            destinationTime.setColon(true);
            presentTime.setColon(true);
            departedTime.setColon(true);

            // RTC display update
            
            DateTime dt = myrtcnow(); 

            // Re-adjust time periodically using NTP
            if(dt.second() == 10 && dt.minute() == 1) {
              
                if(!autoReadjust) {     

                    uint64_t oldT;

                    autoReadjust = true;

                    if(timeDifference) {
                        oldT = dateToMins(dt.year() - presentTime.getYearOffset(), 
                                       dt.month(), dt.day(), dt.hour(), dt.minute());                
                    }
                       
                    if(getNTPTime()) {  

                        bool wasFakeRTC = false;                         

                        dt = myrtcnow();                         
                        
                        #ifdef TC_DBG
                        Serial.println(F("time_loop: RTC re-adjusted using NTP"));
                        #endif

                        if(timeDifference) {
                            uint64_t newT = dateToMins(dt.year() - presentTime.getYearOffset(), 
                                                    dt.month(), dt.day(), dt.hour(), dt.minute()); 
                            wasFakeRTC = (newT > oldT) ? (newT - oldT > 30) : (oldT - newT > 30);
                        
                            // User played with RTC; return to actual present time
                            if(wasFakeRTC) timeDifference = 0;
                        }
                        
                        // Save to EEPROM if change is detected, or if RTC was way off
                        if( (presentTime.getYearOffset() != presentTime.loadYOffs()) || wasFakeRTC ) {
                            if(timetravelPersistent) {
                                presentTime.save();
                            } else {
                                presentTime.saveYOffs();
                            }
                        }
                         
                    } else {                          
                        
                        Serial.println(F("time_loop: RTC re-adjustment via NTP failed"));

                        uint16_t myYear = dt.year();
                        
                        if(myYear > 2050) {

                            // Keep RTC within 2000-2050 
                            // (No need to re-calculate timeDifference,
                            // is based on actual present time (RTC-yoffs)
                            // and therefore stays the same
                             
                            int16_t  yoffs = 0;                             
                     
                            while(myYear > 2050) {
                                myYear -= 28;
                                yoffs -= 28;
                            }

                            presentTime.setYearOffset(yoffs);

                            dt = myrtcnow(); 
                    
                            rtc.adjust(DateTime(
                                myYear,
                                dt.month(),
                                dt.day(),
                                dt.hour(),
                                dt.minute(),
                                dt.second()                
                                )
                            );
                            
                            dt = myrtcnow(); 
                            
                            presentTime.setDateTimeDiff(dt);

                            // Save YearOffs to EEPROM if change is detected
                            if(presentTime.getYearOffset() != presentTime.loadYOffs()) {
                                if(timetravelPersistent) {
                                    presentTime.save();
                                } else {
                                    presentTime.saveYOffs();
                                }
                            } 

                        }
                    }
                }
                
            } else {
              
                autoReadjust = false;
            
            }
                
            if(dt.year() - presentTime.getYearOffset() > 9999) {  

                // RTC(+yearOffs) roll-over
            
                Serial.println(F("Rollover 9999->1 detected, adjusting RTC and yearOffset"));

                if(timeDifference) {

                    // Correct timeDifference
                    timeDifference = 5255474400 - timeDifference;
                    timeDiffUp = !timeDiffUp;

                }
            
                // For year 1, set year to 2017 and yearOffs to 2016
                presentTime.setYearOffset(2016);        

                // Update RTC
                dt = myrtcnow();                 
                rtc.adjust(DateTime(
                    2017,
                    dt.month(),
                    dt.day(),
                    dt.hour(),
                    dt.minute(),
                    dt.second()                
                    )
                );

                // If time travels are persistent, save new value
                if(timetravelPersistent) {
                    presentTime.save();
                } else {
                    presentTime.saveYOffs();
                }
                
                dt = myrtcnow(); 
                
            }     
            
            presentTime.setDateTimeDiff(dt);
                                      
            // Logging beacon
            #ifdef TC_DBG
            if((dt.second() == 0) && (dt.minute() != dbgLastMin)) {
                Serial.print(dt.year());
                Serial.print(F("/"));
                Serial.print(dt.month());
                Serial.print(F(" "));
                dbgLastMin = dt.minute();
                Serial.print(dbgLastMin);
                Serial.print(F("."));
                Serial.print(dt.second());
                Serial.print(F(" "));
                Serial.println(rtc.getTemperature());
            }
            #endif
            
            {
                int compHour = alarmRTC ? dt.hour()   : presentTime.getHour();
                int compMin  = alarmRTC ? dt.minute() : presentTime.getMinute();

                // Sound to play hourly (if available)
                // Follows setting for alarm as regards the options
                // of "real actual present time" vs whatever is currently
                // displayed on presentTime.

                if(compMin == 0) { 
                    if(presentTime.getNightMode() || 
                       !FPBUnitIsOn || 
                       startup || 
                       timeTraveled || 
                       timeTravelP1 || 
                       (alarmOnOff && (alarmHour == compHour) && (alarmMinute == compMin))) {
                        hourlySoundDone = true;
                    }                                
                    if(!hourlySoundDone) {                  
                        play_file("/hour.mp3", 1.0, false, 0); 
                        hourlySoundDone = true;
                    }            
                } else {
                    hourlySoundDone = false;
                }
                
                // Handle alarm
    
                if(alarmOnOff) {
                    if((alarmHour == compHour) && (alarmMinute == compMin) ) {
                        if(!alarmDone) {
                            play_file("/alarm.mp3", 1.0, false, 0); 
                            alarmDone = true; 
                        }
                    } else {
                        alarmDone = false;
                    } 
                }                      

            }
            
            // Handle autoInterval
            
            // Do this on previous minute:59
            minNext = (dt.minute() == 59) ? 0 : dt.minute() + 1;
            
            // Only do this on second 59, check if it's time to do so
            if(dt.second() == 59 && 
               (!autoPaused || (millis() - pauseNow >= pauseDelay)) &&
               autoTimeIntervals[autoInterval] &&
               (minNext % autoTimeIntervals[autoInterval] == 0)) {

                if(!autoIntDone) {

                    #ifdef TC_DBG
                    Serial.println(F("time_loop: autoInterval"));
                    #endif             

                    autoPaused = false;
                    
                    autoIntDone = true;     // Already did this, don't repeat
                    
                    // cycle through pre-programmed times
                    autoTime++;
                    if(autoTime > 7) {    // currently have 8 pre-programmed times
                        autoTime = 0;
                    }

                    // Show a preset dest and departed time
                    destinationTime.setFromStruct(&destinationTimes[autoTime]);
                    departedTime.setFromStruct(&departedTimes[autoTime]);

                    //destinationTime.setColon(true);
                    //presentTime.setColon(true);
                    //departedTime.setColon(true);

                    allOff();

                    // Blank on second 59, display when new minute begins
                    while(digitalRead(SECONDS_IN_PIN) == 0) {  // wait for the complete of this half second
                                                           // Wait for this half second to end
                        myloop();
                    }
                    while(digitalRead(SECONDS_IN_PIN) == 1) {  // second on next low
                                                           // Wait for the other half to end
                        myloop();                    
                    }

                    #ifdef TC_DBG
                    Serial.println(F("time_loop: Update Present Time"));
                    #endif
                    
                    dt = myrtcnow();                  // New time by now                         
                    presentTime.setDateTimeDiff(dt);  // will be at next minute
                    
                    if(FPBUnitIsOn) animate();        // show all with month showing last
                }
                
            } else {
                
                autoIntDone = false;
            
            }                      

        } else {  
          
            destinationTime.setColon(false);
            presentTime.setColon(false);
            departedTime.setColon(false);
            
        }                                          
                                         
        x = y;  

        if(timeTravelP1 > 1) {  
            int ii = 5, tt, obdt, obpt, obld;       
            switch(timeTravelP1) { 
            case 2:
                ((rand() % 10) > 8) ? presentTime.off() : presentTime.on();
                ((rand() % 10) > 8) ? destinationTime.off() : destinationTime.on();
                ((rand() % 10) > 8) ? departedTime.off() : departedTime.on();                
                break;
            case 3:
                presentTime.off();
                destinationTime.off();
                departedTime.off();
                break;
            case 4:                
                destinationTime.show();  
                presentTime.show();                       
                departedTime.show();
                obdt = destinationTime.getBrightness();
                obpt = presentTime.getBrightness();
                obld = departedTime.getBrightness();
                while(ii--) {
                    destinationTime.on();
                    ((rand() % 10) < 7) ? destinationTime.showOnlyText("MALFUNCTION") : destinationTime.show();
                    if(ii % 2) destinationTime.setBrightness((1+(rand() % 10)) & 0x0b);
                    presentTime.on();
                    if(ii % 2) presentTime.setBrightness((1+(rand() % 10)) & 0x0b);
                    departedTime.on();
                    ((rand() % 10) < 3) ? departedTime.showOnlyText("KHDW2011GIDUW") : departedTime.show();
                    if(ii % 2) departedTime.setBrightness((1+(rand() % 10)) & 0x0b);
                    mysdelay(5);
                    allOff();
                    mysdelay(10);
                }
                destinationTime.setBrightness(obdt);
                presentTime.setBrightness(obpt);
                departedTime.setBrightness(obld);
                break;       
            case 5:          
                obdt = destinationTime.getBrightness();
                while(ii--) {   
                    tt = rand() % 10; 
                    if(tt < 3)      { presentTime.lampTest(); }
                    else if(tt < 7) { presentTime.show(); presentTime.on(); }
                    else            { presentTime.off(); }
                    tt = (rand() + millis()) % 10;
                    if(tt < 2)      { destinationTime.lampTest(); }
                    else if(tt < 6) { destinationTime.show(); destinationTime.on(); }
                    else            { destinationTime.setBrightness(1+(rand() % 10)); }  
                    tt = (rand() + millis()) % 10; 
                    if(tt < 4)      { departedTime.lampTest(); }
                    else if(tt < 8) { departedTime.showOnlyText("00000000000000"); departedTime.on(); }
                    else            { departedTime.off(); }
                    mysdelay(5);
                }
                destinationTime.setBrightness(obdt);
                break;
            default:
                allOff();
            }
        }

        if(!startup && !timeTraveled && (timeTravelP1 <= 1) && FPBUnitIsOn) {
            presentTime.show();  
            destinationTime.show();       
            departedTime.show();
        }
    }

}


/* Time Travel: 
 *  
 *  -) copy present time into departed time (where it freezes)
 *  -) copy destination time to present time (where it continues to run as a clock)
 *
 *  This is called from tc_keypad.cpp 
 */

void timeTravel(bool makeLong) 
{
    int tyr = 0;
    int tyroffs = 0;    

    // Pause autoInterval-cycling so user can play undisturbed
    pauseAuto();

    if(makeLong) {
        #ifdef TC_DBG
        Serial.println(F("long time travel phase 1"));
        #endif
        play_file("/travelstart.mp3", 1.0, true, 0);
        timetravelP1Now = millis();
        timetravelP1Delay = TT_P1_DELAY_P1;
        timeTravelP1 = 1;
        return;
    }  
        
    timetravelNow = millis();
    timeTraveled = true;

    play_file("/timetravel.mp3", 1.0, true, 0);
    
    allOff();

    // Copy present time to last time departed
    departedTime.setYear(presentTime.getYear() - presentTime.getYearOffset());
    departedTime.setMonth(presentTime.getMonth());
    departedTime.setDay(presentTime.getDay());
    departedTime.setHour(presentTime.getHour());
    departedTime.setMinute(presentTime.getMinute());
    departedTime.setYearOffset(0);
    
    // We only save the new time to the EEPROM if user wants persistence.
    // Might not be preferred; first, this messes with the user's custom
    // times. Secondly, it wears the flash memory.
    if(timetravelPersistent) {
        departedTime.save();
    }

    // Calculate time difference between RTC and destination time

    DateTime dt = myrtcnow();
    uint64_t rtcTime = dateToMins(dt.year() - presentTime.getYearOffset(), 
                                  dt.month(), 
                                  dt.day(), 
                                  dt.hour(), 
                                  dt.minute());
      
    uint64_t newTime = dateToMins(
                destinationTime.getYear(),
                destinationTime.getMonth(),
                destinationTime.getDay(),
                destinationTime.getHour(),
                destinationTime.getMinute());

    if(rtcTime < newTime) {
        timeDifference = newTime - rtcTime;
        timeDiffUp = true;
    } else {
        timeDifference = rtcTime - newTime;
        timeDiffUp = false;
    }                

    // Save presentTime settings (timeDifference) if to be persistent
    if(timetravelPersistent) {
        presentTime.save();       
    }

    #ifdef TC_DBG
    Serial.println(F("timeTravel: Success, good luck!"));
    #endif
}

/*
 * Reset present time to actual present time
 * (aka "return from time travel")
 */
void resetPresentTime() 
{    
    timetravelNow = millis();
    timeTraveled = true; 
    if(timeDifference) {
        play_file("/timetravel.mp3", 1.0, true, 0);
    }
  
    allOff();
    
    // Copy "present" time to last time departed
    departedTime.setYear(presentTime.getYear() - presentTime.getYearOffset());
    departedTime.setMonth(presentTime.getMonth());
    departedTime.setDay(presentTime.getDay());
    departedTime.setHour(presentTime.getHour());
    departedTime.setMinute(presentTime.getMinute());
    departedTime.setYearOffset(0);

    // We only save the new time to the EEPROM if user wants persistence.
    // Might not be preferred; first, this messes with the user's custom
    // times. Secondly, it wears the flash memory.
    if(timetravelPersistent) {
        departedTime.save();
    }

    // Reset time, Yes, it's that simple.
    timeDifference = 0;

    // Save presentTime settings (timeDifference) if to be persistent
    if(timetravelPersistent) {
        presentTime.save();
    } 
}

// Pause autoInverval-updating for 30 minutes
// Subsequent calls re-start the pause; therefore, it
// is not advised to use different pause durations
void pauseAuto(void)
{
    if(autoTimeIntervals[autoInterval]) {
          pauseDelay = 30 * 60 * 1000;
          autoPaused = true;
          pauseNow = millis();
          #ifdef TC_DBG
          Serial.println(F("pauseAuto: autoInterval paused for 30 minutes"));
          #endif
    }
}

bool checkIfAutoPaused() 
{
    if(!autoPaused || (millis() - pauseNow >= pauseDelay)) {
        return false;
    }
    return true;
}

// choose your time zone from this list
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

/* ATTN:
 *  DateTime uses 1-12 for months
 *  tm (_timeinfo) uses 0-11 for months
 *  Hardware RTC uses 1-12 for months
 *  Xprintf treats "%B" substition as from timeinfo, hence 0-11
 */

/*
 * Get time from NTP
 * (Saves time to RTC)
 */
bool getNTPTime() 
{  
    uint16_t newYear;
    int16_t  newYOffs = 0;
    
    if(WiFi.status() == WL_CONNECTED) { 
      
        // if connected to wifi, get NTP time and set RTC
        
        configTime(0, 0, settings.ntpServer);
        
        setenv("TZ", settings.timeZone, 1);   // Set environment variable with time zone
        tzset();

        if(strlen(settings.ntpServer) == 0) {
            #ifdef TC_DBG            
            Serial.println(F("getNTPTime: NTP skipped, no server configured"));
            #endif
            return false;
        }

        int ntpRetries = 0;
        if(!getLocalTime(&_timeinfo)) {
          
            while(!getLocalTime(&_timeinfo)) {
                if(ntpRetries >= 20) {  
                    Serial.println(F("getNTPTime: Couldn't get NTP time"));
                    return false;
                } else {
                    ntpRetries++;
                }
                mydelay((ntpRetries >= 3) ? 300 : 50);
            }
            
        } 

        // Don't waste any time here...

        // Timeinfo:  Years since 1900
        // RTC:       0-99, 0 being 2000 
        //            (important for leap year compensation, which only works from 2000-2099, not 2100 on, 
        //            century bit has not influence on leap year comp., is buggy)   

        newYear = _timeinfo.tm_year + 1900; 
                       
        while(newYear > 2050) {
            newYear -= 28;
            newYOffs -= 28;
        }

        presentTime.setYearOffset(newYOffs);
                                                            
        presentTime.setDS3232time(_timeinfo.tm_sec, 
                                  _timeinfo.tm_min,
                                  _timeinfo.tm_hour, 
                                  _timeinfo.tm_wday + 1,  // We use Su=1...Sa=7 on HW-RTC, tm uses 0-6 (days since Sunday)
                                  _timeinfo.tm_mday,
                                  _timeinfo.tm_mon + 1,   // Month needs to be 1-12, timeinfo uses 0-11
                                  newYear - 2000); 
                                  
        #ifdef TC_DBG            
        Serial.print(F("getNTPTime: Result from NTP update: "));
        Serial.println(&_timeinfo, "%A, %B %d %Y %H:%M:%S");            
        Serial.println(F("getNTPTime: Time successfully set with NTP"));
        #endif
        
        return true;
            
    } else {
      
        Serial.println(F("getNTPTime: Time NOT set with NTP, WiFi not connected"));
        return false;
    
    }
}

/* 
 * Call this frequently while waiting for button press,  
 * increments timeout each second, returns true when maxtime reached.
 * 
 */
bool checkTimeOut() 
{    
    y = digitalRead(SECONDS_IN_PIN);
    if(x != y) {
        x = y;        
        if(y == 0) {
            timeout++;
        }
    }

    if(timeout >= maxTime) {
        return true;  
    }
    
    return false;
}

// Enable the 1Hz RTC output
void RTCClockOutEnable() 
{    
    uint8_t readValue = 0;
    
    Wire.beginTransmission(DS3231_I2CADDR);
    Wire.write((byte)0x0E);  // select control register
    Wire.endTransmission();

    Wire.requestFrom(DS3231_I2CADDR, 1);
    readValue = Wire.read();
    // enable squarewave and set to 1Hz,
    // Bit 2 INTCN - 0 enables OSC
    // Bit 3 and 4 - 0 0 sets 1Hz
    readValue = readValue & B11100011;  
    
    Wire.beginTransmission(DS3231_I2CADDR);
    Wire.write((byte)0x0E);  // select control register
    Wire.write(readValue);
    Wire.endTransmission();
}

// Determine if provided year is a leap year.
bool isLeapYear(int year) 
{
    if(year & 3 == 0) {
        if(year % 100 == 0) {
            if(year % 400 == 0) {
                return true;
            } else {
                return false;
            }
        } else {
            return true;
        }
    } else {
        return false;
    }
}

// Find number of days in a month
int daysInMonth(int month, int year) 
{    
    if(month == 2 && isLeapYear(year)) {
        return 29;
    }
    return monthDays[month - 1];
}  

/*
 * Internal replacement for RTC.now()
 * RTC sometimes loses sync and does not send data,
 * which is interpreted as 2165/165/165 etc
 * Check for this and retry in case.
 */
DateTime myrtcnow()
{
    DateTime dt = rtc.now();
    int retries = 0;

    while ((dt.month() < 1 || dt.month() > 12 ||
            dt.day()   < 1 || dt.day()   > 31 ||
            dt.hour() > 23 ||
            dt.minute() < 0 || dt.minute() > 59) &&
            retries < 30 ) {

            mydelay((retries < 5) ? 50 : 100);
            dt = rtc.now(); 
            retries++;
    }

    if(retries > 0) {
        Serial.print(F("myrtcnow: "));
        Serial.print(retries, DEC);
        Serial.println(F(" retries needed to read RTC"));
    }

    return dt;
}    

/* 
 *  Convert a date into "minutes since 1/1/1 0:0"
 */
uint64_t dateToMins(int year, int month, int day, int hour, int minute)
{
    uint64_t total64 = 0;
    uint32_t total32 = 0;
    int c = year, d = 1;

    total32 = hours1kYears[year / 1000];
    if(total32) d = (year / 1000) * 1000;
    
    while(c-- > d) {
        total32 += (isLeapYear(c) ? (8760+24) : 8760);
    }
    total32 += (mon_yday[isLeapYear(year) ? 1 : 0][month - 1] * 24);
    total32 += (day - 1) * 24;
    total32 += hour;
    total64 = (uint64_t)total32 * 60;
    total64 += minute;
    return total64;
}

/* 
 *  Convert "minutes since 1/1/1 0:0" into date
 */
void minsToDate(uint64_t total64, int& year, int& month, int& day, int& hour, int& minute)
{
    int c = 1, d = 9;
    int temp;
    uint32_t total32;
    
    year = month = day = 1;
    hour = 0; 
    minute = 0;

    while(d >= 0) {
        if(total64 > mins1kYears[d]) break;
        d--;
    }
    if(d > 0) {
        total64 -= mins1kYears[d];
        c = year = d * 1000;
    }    

    total32 = total64;
   
    while(1) {
        temp = isLeapYear(c++) ? ((8760+24)*60) : (8760*60);
        if(total32 < temp) break;
        year++;
        total32 -= temp;        
    }
    
    c = 1;
    temp = isLeapYear(year) ? 1 : 0;
    while(c < 12) {
        if(total32 < (mon_yday[temp][c]*24*60)) break;
        c++;
    }
    month = c;
    total32 -= (mon_yday[temp][c-1]*24*60);

    temp = total32 / (24*60);
    day = temp + 1;
    total32 -= (temp * (24*60));

    temp = total32 / 60;
    hour = temp;
    
    minute = total32 - (temp * 60);
}

/*
 * Callbacks for fake power switch
 */
#ifdef FAKE_POWER_ON
void fpbKeyPressed() 
{
    isFPBKeyPressed = true;
    isFPBKeyChange = true;
}  
void fpbKeyLongPressStop() 
{
    isFPBKeyPressed = false;
    isFPBKeyChange = true;
}    
#endif

void my2delay(unsigned long mydel) 
{  
    unsigned long startNow = millis();
    while(millis() - startNow < mydel) {
        delay(5);
        audio_loop();
    }     
}


void waitAudioDoneIntro()
{
  int timeout = 100;
  
  while(!checkAudioDone() && timeout--) {       
       audio_loop();
       delay(10);
  }
}
