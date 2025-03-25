#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>

// Configurable defines
String    wifi_ssid;
String    wifi_password;
String    time_zone;
uint32_t  loop_period;
String    google_api_key;
String    google_calendar_id;


// Mux pin mapping
const int MUX_SIO = 15;
const int MUX_S3 = 2;
const int MUX_S2 = 4;
const int MUX_S1 = 16;
const int MUX_S0 = 17;
const int MUX_EN = 5;

// To keep track of which events have been parsed already
// Another option would be to tag the event as 'triggered' but modifying events
// require proper oath API stuff and I cba
uint32_t last_succesfull_update = 0;
uint32_t current_time = 0;
Preferences preferences;

String timeToISOString(time_t t);
void connectToWiFi();
time_t getRTCTimeUTC();
void setupNTP();
time_t syncRTC();
int getTasksFromCalendar(DynamicJsonDocument* doc);
void parseCalendarEvents(const DynamicJsonDocument& originalDoc, DynamicJsonDocument& parsedDoc);
void triggerSliderSlot(int slot);
void writeNumberToMemory(const char* key, uint32_t number);
uint32_t readNumberFromMemory(const char* key, uint32_t defaultValue);
void writeStringToMemory(const char* key, const char* value);
String readStringFromMemory(const char* key, const char* defaultValue);
void preBootConfiguration();
void toggleMuxPort(int portNumber, int durationMs);


// Run once at start
void setup() {
  // Setup mux pins
  pinMode(MUX_SIO, OUTPUT);
  pinMode(MUX_S3, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_EN, OUTPUT);

  // Initialize serial console
  Serial.begin(115200);
  preBootConfiguration();

  // Load config from memory
  wifi_ssid          = readStringFromMemory("wifi_ssid",          "not set");
  wifi_password      = readStringFromMemory("wifi_pass",          "not set");
  time_zone          = readStringFromMemory("time_zone",          "not set");
  google_api_key     = readStringFromMemory("google_api_key",     "not set");
  google_calendar_id = readStringFromMemory("google_cal_id",      "not set");
  loop_period        = readNumberFromMemory("loop_period",        18000);

  // Check if any configuration value is "not set"
  while (wifi_ssid == "not set" || wifi_password == "not set" ||
        time_zone == "not set" || google_api_key == "not set" ||
        google_calendar_id == "not set") {
    Serial.println("Configuration incomplete. Please set all configuration values using preBootConfiguration mode.");
    preBootConfiguration();  // Enter configuration mode again
    // Reload config from memory
    Serial.println();
    Serial.println();
    Serial.println();
    wifi_ssid          = readStringFromMemory("wifi_ssid", "not set");
    wifi_password      = readStringFromMemory("wifi_pass", "not set");
    time_zone          = readStringFromMemory("time_zone", "not set");
    google_api_key     = readStringFromMemory("google_api_key", "not set");
    google_calendar_id = readStringFromMemory("google_cal_id", "not set");
  }

  // Connect and continue
  connectToWiFi();
  setupNTP();
  last_succesfull_update = readNumberFromMemory("last_up_time", 0);
}

// Repeats forever
void loop() {
  // connect to wifi
  // Sync and print time (needs to do this in the loop or we will miss summer time changes)
  Serial.println("Current time: ");
  Serial.println(syncRTC());
  
  // Retrieve and print tasks from Google Calendar
  DynamicJsonDocument tasks(8192);
  DynamicJsonDocument parsedTasks(2048);
  int attempts = 0;
  while (attempts < 10.) {
    if (getTasksFromCalendar(&tasks) == 0) {
      // Filter for untriggered tasks in the last TASK_WINDOW_SIZE seconds
      parseCalendarEvents(tasks, parsedTasks);
      serializeJsonPretty(parsedTasks, Serial);
      Serial.println();
      if(parsedTasks["taskCount"] > 0){
        // Trigger slot and update calendar event
        for(int i = 0; i < parsedTasks["taskCount"]; i++){
            Serial.println();
            Serial.print("Processing event ");
            Serial.println(parsedTasks["tasks"][i]["event_name"].as<String>());
            triggerSliderSlot(parsedTasks["tasks"][i]["slider_slot"]);
        }

        // Since this only gets called when there is at least one task to be triggered
        // This function doesnt get called so ofte, so it wont deteriorate the flash memory
        // as quickly as if it was called every time we check for a LOOP_TIME block of tasks
        last_succesfull_update = current_time + 60; // Skip one minute to make sure we dont read the same task twice, it rounds so can cause problems
        writeNumberToMemory("last_up_time", last_succesfull_update);
      }
      break; // Exit the retry loop after successful parsing.
    } else {
      Serial.println("Failed to get tasks from calendar. Retrying...");
      attempts++;
      delay(1000);
    }
  }
  if (attempts == 3) {
    Serial.println("Failed to get tasks from calendar after 3 attempts.");
  }
  
  // Sleep for loop_period before repeating
  delay(loop_period * 1000);
}

// Function to convert time_t to ISO8601 string for api call
String timeToISOString(time_t t) {
  struct tm * timeinfo = gmtime(&t);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", timeinfo);
  return String(buffer);
}

void connectToWiFi() {
  WiFi.begin(wifi_ssid, wifi_password);
  Serial.println("Connecting to wifi...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println("Connected");
}

// returns time in utc
time_t getRTCTimeUTC() {
  return time(NULL);
}

// setup online clock
void setupNTP(){
  // set and calcualte time zone
  setenv("TZ", time_zone.c_str(), 1);
  tzset();
  // setup ntp server
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// Update RTC with online clock, returns current time in utc
time_t syncRTC() {
  Serial.println("Attempting to get online time...");
  struct tm timeinfo;
  for (int i = 0; i < 3; i++) {
    if (getLocalTime(&timeinfo)) {
      time_t onlineTime = mktime(&timeinfo);
      if (onlineTime != 0) {
        Serial.print("Online time obtained: ");
        Serial.println(timeToISOString(onlineTime));
        struct timeval tv = { .tv_sec = onlineTime, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
          Serial.println("RTC successfully updated.");
        } else {
          Serial.println("Failed to set RTC time.");
        }
        return getRTCTimeUTC();
      }
    }
    Serial.println("Retrying to get online time...");
    delay(1000);
  }
  Serial.println("Failed to get online time after 3 attempts. Using previous RTC time.");
  time_t previousTime = getRTCTimeUTC();
  Serial.print("Previous RTC time: ");
  Serial.println(timeToISOString(previousTime));
  return previousTime;
}

int getTasksFromCalendar(DynamicJsonDocument* doc) {
  current_time = getRTCTimeUTC();
  String timeMin = timeToISOString(last_succesfull_update);
  String timeMax = timeToISOString(current_time);

  Serial.print("Getting events from [");
  Serial.print(timeMin);
  Serial.print("] to [");
  Serial.print(timeMax);
  Serial.println("]");

  // https://developers.google.com/calendar/api/v3/reference/events/list
  String url = "https://www.googleapis.com/calendar/v3/calendars/" + 
                String(google_calendar_id) +
               "/events?key=" + String(google_api_key) +
               "&singleEvents=true" +
               "&maxResults=60" +
               "&timeMin=" + timeMin +
               "&timeMax=" + timeMax;
  Serial.println(url);

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  String payload;
  if (httpCode > 0) {
    payload = http.getString();
  } else {
    http.end();
    (*doc)["error"] = "HTTP GET failed";
    return 1;
  }
  http.end();

  DeserializationError error = deserializeJson(*doc, payload);
  if (error) {
    doc->clear();
    (*doc)["error"] = "JSON parse error";
    return 1;
  }
  return 0;
}

void parseCalendarEvents(DynamicJsonDocument& originalDoc, DynamicJsonDocument& parsedDoc) {
  int untriggered = 0;
  parsedDoc.clear();
  JsonArray parsedItems = parsedDoc.createNestedArray("tasks");
  parsedDoc["taskCount"] = 0;

  if (!originalDoc.containsKey("items")) {
    Serial.println("Error: 'items' array not found in the original document.");
    parsedDoc["error"] = "'items' array not found";
    return;
  }

  JsonArray items = originalDoc["items"];

  for (JsonObject item : items) {
    if (!item.containsKey("id") || !item.containsKey("summary") || !item.containsKey("description")) {
      Serial.println("Warning: Skipping an event due to missing 'id', 'summary', or 'description'.");
      continue;
    }

    JsonObject parsedEvent = parsedItems.createNestedObject();

    const char* description = item["description"];
    int slider_slot = -1;
    bool triggered = true;

    // Find "slider_slot:" and extract the value
    const char* slider_slotStr = strstr(description, "slider_slot:");
    if (slider_slotStr != nullptr) {
      slider_slot = atoi(slider_slotStr + 13); // +8 to skip "slider_slot: "
    }

    // Find "triggered:" and extract the value
    const char* triggeredStr = strstr(description, "triggered:");
    if (triggeredStr != nullptr) {
      triggered = (atoi(triggeredStr + 11) != 0);  // +11 to skip "triggered: "
    }
    
    String serializedEvent;

    serializeJson(item, serializedEvent);

    
    // Mark item as triggered (Will only be pushed to google calendars later)
    int n = serializedEvent.indexOf("triggered:") + 11;
    if (n < serializedEvent.length() && n >= 0) {
        serializedEvent.setCharAt(n, '1');
    }
    parsedEvent["slider_slot"] = slider_slot;
    parsedEvent["event_name"] = item["summary"].as<String>();
    parsedEvent["event_id"] = item["id"].as<String>();
    parsedEvent["event_json"] = serializedEvent;
    untriggered++;
  }
  parsedDoc["taskCount"] = untriggered;
}

// Function to write a uint32_t number to non-volatile memory
void writeNumberToMemory(const char* key, uint32_t number) {
  // Open the namespace "storage" for writing
  if (!preferences.begin("storage", false)) {
    Serial.println("Failed to open preferences for writing\n");
    return;
  }
  
  // Write the number with the specified key
  if (preferences.putUInt(key, number)) {
    Serial.printf("Successfully written to \"%s\" [%u]\n", key, number);
  } else {
    Serial.printf("Failed to write \"%u\" to key \"%s\"\n", number, key);
  }
  
  // Close the preferences
  preferences.end();
}

// Function to read a uint32_t number from non-volatile memory
uint32_t readNumberFromMemory(const char* key, uint32_t defaultValue) {
  // Open the namespace "storage" for reading
  if (!preferences.begin("storage", true)) {
    Serial.println("Failed to open preferences for reading");
    return defaultValue;
  }
  uint32_t number = preferences.getUInt(key, defaultValue);
  preferences.end();
  Serial.printf("Returning value %u fo key \"%s\"\n", number, key);
  return number;
}
void writeStringToMemory(const char* key, const char* value) {
  if (!preferences.begin("storage", false)) {
    Serial.println("Failed to open preferences for writing");
    return;
  }
  if (preferences.putString(key, value)) {
    Serial.printf("Successfully written to \"%s\" [%s]\n", key, value);
  } else {
    Serial.printf("Failed to write \"%s\" to key \"%s\"\n",value, key);
  }
  preferences.end();
}

// Function to read a string value from non-volatile memory
String readStringFromMemory(const char* key, const char* defaultValue) {
  if (!preferences.begin("storage", true)) {
    Serial.println("Failed to open preferences for reading");
    return String(defaultValue);
  }
  String value = preferences.getString(key, defaultValue);
  preferences.end();
  Serial.printf("Returning value \"%s\" for key \"%s\"\n", value.c_str(), key);
  return value;
}

void preBootConfiguration() {
  Serial.println("Waiting for serial input for 5 seconds. Send any character to enter pre-boot configuration mode...");
  unsigned long startTime = millis();
  bool interactiveMode = false;
  
  // Wait up to 5 seconds for any serial input
  while (millis() - startTime < 5000) {
    if (Serial.available() > 0) {
      interactiveMode = true;
      break;
    }
  }
  
  if (!interactiveMode) {
    Serial.println("No serial input received. Continuing boot...");
    return;
  }
  
  // As soon as any character is detected, print the helper list.
  Serial.println("Entering pre-boot configuration mode.");
  Serial.println("Available commands:");
  Serial.println("  get <key>            -- Retrieve the value of a configuration key.");
  Serial.println("  set <key> <value>    -- Set the value of a configuration key.");
  Serial.println("Valid keys:");
  Serial.println("  wifi_ssid            (string)         Your wifi name");
  Serial.println("  wifi_pass            (string)         Your wifi password");
  Serial.println("  time_zone            (string)         Your time zone in POSIX timezone string format");
  Serial.println("  google_api_key       (string)         Google API key");
  Serial.println("  google_cal_id        (string)         Google calendar ID (found on your calendar)");
  Serial.println("  last_up_time         (number)         Time in UTC, probs ignore this unless you are debugging code");
  Serial.println("  loop_period          (number)         Time seconds it takes to update the board, probs ignore this...");
  Serial.println("Type 'exit' to leave configuration mode.");
  
  // Interactive command loop
  while (true) {
    if (Serial.available() > 0) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      
      if (input.equalsIgnoreCase("exit")) {
        Serial.println("Exiting pre-boot configuration mode.");
        break;
      }
      
      // Expect command format: <command> <key> [value]
      int firstSpace = input.indexOf(' ');
      if (firstSpace == -1) {
        Serial.println("Invalid command format. Use 'get <key>' or 'set <key> <value>'.");
        continue;
      }
      
      String command = input.substring(0, firstSpace);
      command.trim();
      String remainder = input.substring(firstSpace + 1);
      remainder.trim();
      
      if (command.equalsIgnoreCase("get")) {
        String key = remainder;
        if (key.equals("wifi_ssid") || key.equals("wifi_pass") ||
            key.equals("time_zone") || key.equals("google_api_key") ||
            key.equals("google_cal_id")) {
          String value = readStringFromMemory(key.c_str(), "Not set");
          Serial.print(key);
          Serial.print(" = ");
          Serial.println(value);
        }
        else if (key.equals("last_up_time")) {
          uint32_t num = readNumberFromMemory(key.c_str(), 0);
          Serial.print(key);
          Serial.print(" = ");
          Serial.println(num);
        }
        else {
          Serial.println("Invalid key.");
        }
      }
      else if (command.equalsIgnoreCase("set")) {
        int secondSpace = remainder.indexOf(' ');
        if (secondSpace == -1) {
          Serial.println("Invalid set command format. Usage: set <key> <value>");
          continue;
        }
        String key = remainder.substring(0, secondSpace);
        key.trim();
        String value = remainder.substring(secondSpace + 1);
        value.trim();
        if (key.equals("wifi_ssid") || key.equals("wifi_pass") ||
            key.equals("time_zone") || key.equals("google_api_key") ||
            key.equals("google_cal_id")) {
          writeStringToMemory(key.c_str(), value.c_str());
        }
        else if (key.equals("last_up_time") || key.equals("loop_period") ) {
          uint32_t num = (uint32_t)value.toInt();
          writeNumberToMemory(key.c_str(), num);
        }
        else {
          Serial.println("Invalid key.");
        }
      }
      else {
        Serial.println("Unknown command. Use 'get' or 'set'.");
      }
    }
  }
}

void toggleMuxPort(int portNumber, int durationMs) {
  // Define pin mappings
  const int MUX_SIO = 15;
  const int MUX_S3  = 2;
  const int MUX_S2  = 4;
  const int MUX_S1  = 16;
  const int MUX_S0  = 17;
  const int MUX_EN  = 5;

  // Ensure port number is valid (0 to 31)
  if (portNumber < 0 || portNumber > 31) {
    Serial.println("Invalid port number.");
    return;
  }

  // Set pin modes
  pinMode(MUX_SIO, OUTPUT);
  pinMode(MUX_S3, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_EN, OUTPUT);

  // Set the multiplexer address
  digitalWrite(MUX_S0, portNumber & 0x01);
  digitalWrite(MUX_S1, (portNumber >> 1) & 0x01);
  digitalWrite(MUX_S2, (portNumber >> 2) & 0x01);
  digitalWrite(MUX_S3, (portNumber >> 3) & 0x01);

  // Enable the multiplexer
  digitalWrite(MUX_EN, LOW);

  // Activate the port
  digitalWrite(MUX_SIO, HIGH);
  delay(durationMs);
  digitalWrite(MUX_SIO, LOW);

  // Disable the multiplexer
  digitalWrite(MUX_EN, HIGH);
}

void triggerSliderSlot(int slot){
  Serial.printf("Triggering slot %d\n", slot);
  toggleMuxPort(slot, 800);
  delay(100);
  toggleMuxPort(slot, 400);
  delay(100);
  toggleMuxPort(slot, 400);
  delay(100);
  toggleMuxPort(slot, 400);
}