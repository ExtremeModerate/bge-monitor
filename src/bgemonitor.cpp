#include <Arduino.h>

#include <ESP8266WiFi.h> // https://github.com/esp8266/Arduino
#include <HttpClient.h>

#include <max6675.h>

// needed for library
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager

#include <PID_v1.h>
#include <Ticker.h>
#include "ThingSpeak.h"

// Define Variables we'll be connecting to with PID
double domeTarget, domeTempF, fanOutput;
double meatTarget, meatTempF;

// for LED status
Ticker ticker;

// Specify the links and initial tuning parameters
double aggKp = 4, aggKi = 0.2, aggKd = 1;
double consKp = 1, consKi = 0.05, consKd = 0.25;
double Kp = 2, Ki = 5, Kd = 1;
PID domePID(&domeTempF, &fanOutput, &domeTarget, Kp, Ki, Kd, DIRECT);

// for PID output control - vary the fan on/off by Output ms every x seconds
#define FANWINDOW 10000
unsigned long windowStartTime;

// thermocouple max6675 interface
int ktcSO  = 12;
int ktcCS  = 13;
int ktcCLK = 14;
MAX6675 ktc(ktcCLK, ktcCS, ktcSO);

#define FAN 2
int fanState = HIGH; // HIGH is off

// text updates via http://textbelt.com/text -d number=4042164197 -d "message=text"

// Name of the server we want to connect to
const char textHost[] = "textbelt.com";
// Path to download (this is the bit after the hostname in the URL
// that you want to download
const char textPath[]   = "/text";
const char textNumber[] = "4042164197";

// do all your forward declarations
void setFan(int mode);
double fahrenheit(double celcius);
double Kelvin(double celsius);
void tick();
void configModeCallback(WiFiManager *myWiFiManager);
void sendTextMessage(const char *phoneNumber, const char *message);
void sendTextMessage(const char *phoneNumber, double message);
void sendTextMessage(const char *phoneNumber, float message);

// ThingSpeak Channel number and API key
#define TSCHANNEL 1257165
// const char * myWriteAPIKey    = "A7CCN9ZP8KOR8MZ7";
#define TSAPIKEY  "XR8XPHTH5MX4AGQN"
WiFiClient client;
// TSINTERVAL = number of seconds between ThingSpeak updates
#define TSINTERVAL 15
unsigned long nextTSUpdate = 0;
// TEXTINTERVAL = number of MINUTES between text updates
#define TEXTINTERVAL 150
unsigned long nextTextUpdate = 0;


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
    int state = digitalRead(LED_BUILTIN); // get the current state of GPIO1 pin

    digitalWrite(LED_BUILTIN, !state); // set pin to the opposite state
}

// gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager)
{
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    // if you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());
    // entered config mode, make led toggle faster
    ticker.attach(0.2, tick);
}

// sendTextMessage sends message to a given phoneNumber via an http POST
void sendTextMessage(const char *phoneNumber, const char *message)
{
  const char phoneHeader[]   = "number=%s&";
  const char messageHeader[] = "message";

  char httpData[100] = "";
  WiFiClient c;
  HttpClient http(c);
  http.beginRequest();
  http.startRequest(textHost, 80, textPath, HTTP_METHOD_POST, HTTP_HEADER_USER_AGENT);
  unsigned int n=sprintf (httpData, "number=%s&message=%s", textNumber, "message");
  Serial.println(httpData);
  c.print(httpData);
  http.endRequest();

}

// This version does the same for a double message
void sendTextMessage(const char *phoneNumber, double message)
{
    char valueString[15]; // long range is -2147483648 to 2147483647, so 12 bytes including terminator

    ltoa(message, valueString, 10);
    sendTextMessage(phoneNumber, valueString);
}

// This version does the same with a float message
void sendTextMessage(const char *phoneNumber, float message)
{
    char valueString[20]; // range is -999999000000.00000 to 999999000000.00000, so 19 + 1 for the terminator

    // Supported range is -999999000000 to 999999000000
    if (0 == isinf(message) && (message > 999999000000 || message < -999999000000)) {
        // Out of range
        sprintf(valueString, "%s", "ERROR:OUT OF RANGE");
    } else {
        // assume that 5 places right of decimal should be sufficient for most applications
        sprintf(valueString, "%.5f", message);
    }
    sendTextMessage(phoneNumber, valueString);
}

void setup()
{
    // put your setup code here, to run once:
    Serial.begin(9600);

    // set led pin as output
    pinMode(LED_BUILTIN, OUTPUT);
    // start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);

    pinMode(FAN, OUTPUT);

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
    digitalWrite(LED_BUILTIN, LOW);
    sendTextMessage(textNumber, "hello");

    // initialize the variables we're linked to
    domeTarget = 450;

    // tell the PID to range between 0 and the full window size
    domePID.SetOutputLimits(0, FANWINDOW);

    // turn the PID on
    domePID.SetMode(AUTOMATIC);

    domePID.SetTunings(aggKp, aggKi, aggKd);
    // domePID.SetTunings(consKp, consKi, consKd);

    ThingSpeak.begin(client);
} // setup

void loop()
{
    // Write to ThingSpeak. There are up to 8 fields in a channel, allowing you to store up to 8 different
    // pieces of information in a channel.  Here, we write to field 1.

    domeTempF = ktc.readFarenheit();
    domePID.Compute();

    if (millis() - windowStartTime > FANWINDOW) { // time to shift the Relay Window
        windowStartTime += FANWINDOW;
    }

    if (fanOutput < millis() - windowStartTime) {
      digitalWrite(FAN, LOW);
      Serial.printf("FAN is ON\t%d\t%d\n", windowStartTime, millis());
    } else {
      digitalWrite(FAN, HIGH);
      Serial.printf("FAN is off\t%d\t%d\n", windowStartTime, millis());
    }
    delay(100);

    if (millis() >= nextTSUpdate) {
      nextTSUpdate = millis() + TSINTERVAL * 1000; // next time we should update ThingSpeak
      Serial.print("Dome F = ");
      Serial.print(domeTempF);
      Serial.print("\tdomeTarget = ");
      Serial.print(domeTarget);
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
        //  delay(60 * 1000); // ThingSpeak will only accept updates every 15 seconds.
      }
    }

    // if (millis() >= nextTextUpdate) {
    //     nextTextUpdate = millis() + TEXTINTERVAL * 60 * 1000; // next time we need to send a text update
    //     sendTextMessage(textNumber, domeTempF);
    // }
} // loop
