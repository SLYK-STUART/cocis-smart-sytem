#pragma once
#define GROUP_ID        "g5"
#define TOPIC_ROOT      "cocis/" GROUP_ID

// Node liveness, retained: "online" / "offline" (offline comes from the MQTT last will)
#define T_STATUS_BASE   TOPIC_ROOT "/status/"
#define T_STATUS_ALL    TOPIC_ROOT "/status/#"

// Entrance publishes every access decision
#define T_ACCESS_EVENT  TOPIC_ROOT "/entrance/access"

// Rooms publish, retained: {"room":1,"occupied":0,"light":0}
#define T_ROOM1_STATE   TOPIC_ROOT "/room1/state"
#define T_ROOM2_STATE   TOPIC_ROOT "/room2/state"

// Entrance -> rooms (used in a later batch): emergency, force-off, etc.
#define T_ROOM1_CMD     TOPIC_ROOT "/room1/cmd"
#define T_ROOM2_CMD     TOPIC_ROOT "/room2/cmd"

// Entrance publishes (retained): {"open":"07:00","close":"22:00","emergency":0}
#define T_SYSTEM_STATE  TOPIC_ROOT "/entrance/system"

// Entrance publishes safety alerts: {"code":"...","detail":"..."}
#define T_ALERT         TOPIC_ROOT "/entrance/alert"