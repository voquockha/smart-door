# X68 Lite LuckFox runtime environment.
# Source this file before starting the demo:
#   . ./x68_luckfox_env.sh

# MQTT bootstrap + production.
export MQTT_ENABLED=1
export MQTT_HOST=mqtt.x68.space
export MQTT_PORT=8883
export MQTT_TLS_ENABLED=1
export MQTT_TLS_VERIFY_SERVER=0

# Device metadata sent in activate_device/status.
export MQTT_SERIAL_NUMBER=X680001
export MQTT_DEVICE_MODEL=x68-lite
export MQTT_SOFTWARE_VERSION=1.0.0

# Persistent device state.
export MQTT_CREDENTIAL_PATH=/data/device/credential.json
export MQTT_CREDENTIAL_FALLBACK_PATH=/root/x68-device/credential.json
export MQTT_INSTALLATION_ID_PATH=/data/device/installation_id
export MQTT_OFFLINE_QUEUE_PATH=/root/x68-device/pending_events.jsonl
export MQTT_COMMAND_CACHE_PATH=/root/x68-device/processed_commands.jsonl
export MQTT_COMMAND_CACHE_FALLBACK_PATH=/root/x68-device/processed_commands.jsonl

# Intervals and logs.
export MQTT_STATUS_INTERVAL_SECONDS=60
export MQTT_RETRY_INTERVAL_SECONDS=30
export MQTT_DEBUG_PAYLOAD=1
export BENCH_LOG_ENABLED=0

# HTTPS downloads from console.x68.space. LuckFox images often lack CA store.
export MQTT_FILE_TLS_VERIFY=0
export MQTT_FACE_ALLOW_INSECURE=1

# Face/audio/attendance local behavior.
export FACE_AUDIO_DIR=/root/kha/audio
export ATTENDANCE_HTTP_ENABLED=0
export ATTENDANCE_AUDIO_ENABLED=1
export ATTENDANCE_AUDIO_PLAYER=/usr/bin/aplay
export ATTENDANCE_SAVE_IMAGE=1
export ATTENDANCE_BASE_DIR=/root/attendance
export ATTENDANCE_COOLDOWN_SECONDS=60
export ANTI_SPOOF_THRESHOLD=0.85

# Optional Telegram. Leave unset for MQTT-only runtime.
# export TELEGRAM_BOT_TOKEN=
# export TELEGRAM_CHAT_ID=
# export TELEGRAM_TOPIC_ID=
# export TELEGRAM_ALLOW_INSECURE=1
