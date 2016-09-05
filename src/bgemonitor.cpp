#include <Arduino.h>

#include <ESP8266WiFi.h> // https://github.com/esp8266/Arduino

#include <max6675.h>

// needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager

#include <PID_v1.h>
#include <Ticker.h>
#include "ThingSpeak.h"

// Define Variables we'll be connecting to with PID
double Setpoint, Input, Output;

// for LED status
Ticker ticker;

// Specify the links and initial tuning parameters
double aggKp = 4, aggKi = 0.2, aggKd = 1;
double consKp = 1, consKi = 0.05, consKd = 0.25;
double Kp = 2, Ki = 5, Kd = 1;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, DIRECT);

// for PID output control - vary the fan on/off by Output ms every x seconds
#define FANWINDOW 10000
unsigned long windowStartTime;

// thermocouple max6675 interface
int ktcSO  = 12;
int ktcCS  = 13;
int ktcCLK = 14;
MAX6675 ktc(ktcCLK, ktcCS, ktcSO);

#define FAN 5
int fanState = HIGH; // HIGH is off

// do all your forward declarations
void setFan(int mode);
double fahrenheit(double celcius);
double Kelvin(double celsius);
void tick();
void configModeCallback(WiFiManager * myWiFiManager);

// ThingSpeak Channel number and API key
#define TSCHANNEL 150647
// const char * myWriteAPIKey    = "A7CCN9ZP8KOR8MZ7";
#define TSAPIKEY  "A7CCN9ZP8KOR8MZ7"
WiFiClient client;

// Celsius to Fahrenheit conversion
double Fahrenheit(double celsius)
{
    return 1.8 * celsius + 32;
}

// Celsius to Kelvin conversion
double Kelvin(double celsius)
{
    return celsius + 273.15;
}

void tick()
{
    // toggle state
    int state = digitalRead(BUILTIN_LED); // get the current state of GPIO1 pin

    digitalWrite(BUILTIN_LED, !state); // set pin to the opposite state
}

// gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager * myWiFiManager)
{
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    // if you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());
    // entered config mode, make led toggle faster
    ticker.attach(0.2, tick);
}

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(9600);

    // set led pin as output
    pinMode(BUILTIN_LED, OUTPUT);
    // start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);

    // WiFiManager
    // Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    // reset settings - for testing only
    // wifiManager.resetSettings();

    // set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);

    // fetches ssid and pass and tries to connect
    // if it does not connect it starts an access point with the specified name
    // here  "AutoConnectAP"
    // and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect()) {
        Serial.println("failed to connect and hit timeout");
        // reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(1000);
    }

    // if you get here you have connected to the WiFi
    Serial.println("connected...yeey :)");
    ticker.detach();

    ticker.attach(5, tick);

    // keep LED on
    digitalWrite(BUILTIN_LED, LOW);

    ThingSpeak.begin(client);
}

void loop()
{
    // Write to ThingSpeak. There are up to 8 fields in a channel, allowing you to store up to 8 different
    // pieces of information in a channel.  Here, we write to field 1.

    double domeTempF = ktc.readFarenheit();
    double meatTempF = 0;
    double fanOutput = 0;

    Serial.print("Dome F = ");
    Serial.print(domeTempF);
    Serial.print("\t Meat F = ");
    Serial.print(meatTempF);
    Serial.print("\t fanOutput = ");
    Serial.print(fanOutput);
    Serial.println("");


    // Check if any reads failed and exit early (to try again).
    if (isnan(domeTempF) || isnan(meatTempF) || isnan(fanOutput)) {
        Serial.println("Error: bad data!");
        delay(1000);
    } else {
        ThingSpeak.setField(1, (float) domeTempF);
        ThingSpeak.setField(2, (float) meatTempF);
        ThingSpeak.setField(3, (float) fanOutput);

        ThingSpeak.writeFields(TSCHANNEL, TSAPIKEY);
        delay(60 * 1000); // ThingSpeak will only accept updates every 15 seconds.
    }
}
