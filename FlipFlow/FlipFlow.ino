#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>

// Configurable defines

// TODO: make this configurable via serial with 'preferences'
#define WIFI_SSID             "CHANGE ME"
#define WIFI_PASSWORD         "CHANGE ME"
#define TIME_ZONE             "GMT0BST,M3.5.0/1,M10.5.0" // UK change for yours
#define LOOP_PERIOD           60*30 // seconds
#define GOOGLE_API_KEY        "CHANGE ME"
#define GOOGLE_CALENDAR_ID    "CHANGE ME"  // Use the calendar ID

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


// Run once at start
void setup() {
  // Initialize serial console
  Serial.begin(115200);
  connectToWiFi();
  setupNTP();
  last_succesfull_update = readNumberFromMemory("lastSucUp", 0);
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
        writeNumberToMemory("lastSucUp", last_succesfull_update);
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
  
  // Sleep for LOOP_PERIOD before repeating
  delay(LOOP_PERIOD * 1000);
}

// Function to convert time_t to ISO8601 string for api call
String timeToISOString(time_t t) {
  struct tm * timeinfo = gmtime(&t);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z", timeinfo);
  return String(buffer);
}

void connectToWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
  setenv("TZ", TIME_ZONE, 1);
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
                String(GOOGLE_CALENDAR_ID) +
               "/events?key=" + String(GOOGLE_API_KEY) +
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

void triggerSliderSlot(int slot){
  Serial.print("Triggering slot ");
  Serial.println(slot);
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
    Serial.printf("Failed to write to key \"%s\"\n", key);
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
