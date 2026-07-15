# Tài liệu MQTT cho LuckFox RetinaFace + FaceNet

Tài liệu này mô tả phần MQTT hiện tại trong example
`luckfox_pico_rtsp_retinaface_facenet`, cách cấu hình, cách test đăng ký gương mặt,
và flow đăng ký thiết bị mục tiêu để triển khai tiếp.

## 1. Phạm vi hiện tại

Code hiện tại đã hỗ trợ:

- Kết nối MQTT local anonymous, mặc định `127.0.0.1:1883`.
- Nhận command `register_face` qua MQTT.
- Tải ảnh từ `data.face_link`, chạy `retinaface.rknn` + `facenet.rknn`.
- Lưu embedding vào `face_db.bin`.
- Trả response đăng ký gương mặt đúng pattern chính:
  `uuid`, `timestamp`, `device_id`, `command`, `message`, `status`, `data`.
- Khi nhận diện thành công, gửi event qua MQTT topic khác.
- Event có thể kèm ảnh dạng base64.
- Flow provisioning thiết bị khi bật `MQTT_PROVISIONING_ENABLED=1`:
  `register_device`, `WAITING_USER_APPROVAL`, `check_register_status`,
  `DEVICE_APPROVED`, lưu credential và chuyển sang topic production.
- Gửi `device_online` sau khi vào production MQTT.
- Queue event điểm danh vào local file nếu MQTT đang offline.

Giới hạn hiện tại:

- MQTT client là TCP MQTT 3.1.1 tự viết, chưa có TLS. Khi server trả
  `mqtt_tls=true` hoặc port `8883`, cần đổi server test sang plaintext trước,
  hoặc triển khai TLS ở bước sau.
- HMAC đã có trong payload provisioning, nhưng server cần dùng cùng canonical
  string ở mục HMAC bên dưới để verify.
- ACL là phần cấu hình broker/server, không nằm trong app LuckFox.

## 2. Build

Từ thư mục gốc repo:

```bash
cmake -S . -B /tmp/luckfox_mqtt_build \
  -DEXAMPLE_DIR=example/luckfox_pico_rtsp_retinaface_facenet \
  -DEXAMPLE_NAME=luckfox_pico_rtsp_retinaface_facenet \
  -DLIBC_TYPE=uclibc

cmake --build /tmp/luckfox_mqtt_build -j2
```

Nếu dùng script `build.sh`, chọn:

- libc: `uclibc`
- example: `luckfox_pico_rtsp_retinaface_facenet`

## 3. Chạy MQTT broker local

Ví dụ chạy Mosquitto bằng Docker:

```bash
docker run --rm -it \
  -p 1883:1883 \
  eclipse-mosquitto:2 \
  mosquitto -c /mosquitto-no-auth.conf
```

## 4. Biến môi trường MQTT

Mặc định:

```bash
export MQTT_HOST=10.82.117.122
export MQTT_PORT=1883
export MQTT_REQUEST_TOPIC=x86-lite/local/device/mac/request
export MQTT_RESPONSE_TOPIC=x86-lite/local/device/mac/response
export MQTT_EVENT_TOPIC=x86-lite/local/device/mac/event
export MQTT_STATUS_TOPIC=x86-lite/local/device/mac/status
export MQTT_STATUS_INTERVAL_SECONDS=60
```

Các biến hữu ích:

```bash
# Device id trả trong response/event. Nếu không set sẽ lấy MAC eth0.
export MQTT_DEVICE_ID="ee:7b:7f:0a:95:26"

# Thông tin thiết bị được gửi trong status.
export MQTT_SERIAL_NUMBER="LF-CAM-000001"
export MQTT_DEVICE_MODEL="camera_001"
export MQTT_FIRMWARE_VERSION="1.0.0"

# Chu kỳ status tính bằng giây. Đặt 0 để tắt gửi định kỳ;
# thiết bị vẫn gửi một status ngay sau khi kết nối broker.
export MQTT_STATUS_INTERVAL_SECONDS=60

# Bật/tắt gửi ảnh base64 trong event nhận diện.
export MQTT_INCLUDE_IMAGE_BASE64=1

# Dùng tạm khi tải face_link HTTPS bị lỗi CA certificate.
export MQTT_FACE_ALLOW_INSECURE=1

# Tắt MQTT nếu chỉ muốn chạy camera/AI.
export MQTT_ENABLED=0
```

## 4.1. Biến môi trường provisioning

Mặc định provisioning tắt để không phá flow test local. Bật bằng:

```bash
export MQTT_PROVISIONING_ENABLED=1
```

Các biến thường dùng:

```bash
export MQTT_PROVISIONING_HOST=127.0.0.1
export MQTT_PROVISIONING_PORT=1883
export MQTT_CREDENTIAL_PATH=/data/device/credential.json
export MQTT_OFFLINE_QUEUE_PATH=/data/device/offline_events.jsonl

export MQTT_SERIAL_NUMBER=LF-CAM-000001
export MQTT_DEVICE_MODEL=camera_001
export MQTT_FIRMWARE_VERSION=1.0.0
export MQTT_DEVICE_SECRET=luckfox-dev-secret
```

Nếu `MQTT_PROVISIONING_ENABLED=1`:

- Nếu chưa có credential file, app connect anonymous tới provisioning broker.
- App subscribe `provisioning/v1/register/response/{uuid}`.
- App publish `register_device` vào `provisioning/v1/register/request`.
- Nếu server trả `WAITING_USER_APPROVAL`, app giữ trạng thái pending.
- App định kỳ publish `check_register_status`.
- Nếu server trả `DEVICE_APPROVED`, app lưu credential và reconnect production.

Nếu đã có credential file:

- App bỏ qua provisioning.
- App connect bằng `mqtt_user/mqtt_pass`.
- App subscribe topic `devices/{device_uid}/down/command`.
- App publish event vào `devices/{device_uid}/up/event`.

## 5. Chạy chương trình

Trên thiết bị LuckFox, trong thư mục install của example:

```bash
./luckfox_pico_rtsp_retinaface_facenet \
  run model/retinaface.rknn model/facenet.rknn face_db.bin
```

Khi chạy thành công, log sẽ có dạng:

```text
[mqtt] init enabled=true broker=127.0.0.1:1883 ...
[mqtt] connected
[mqtt] subscribed: x86-lite/local/device/mac/request
```

## 6. Test subscribe response, event và status

Mở terminal 1 để xem response đăng ký:

```bash
mosquitto_sub -h 127.0.0.1 -t x86-lite/local/device/mac/response -v
```

Mở terminal 2 để xem event nhận diện thành công:

```bash
mosquitto_sub -h 127.0.0.1 -t x86-lite/local/device/mac/event -v
```

Mở terminal 3 để xem status gửi ngay khi kết nối và gửi lại định kỳ:

```bash
mosquitto_sub -h 127.0.0.1 -t x86-lite/local/device/mac/status -v
```

Payload status local giữ nguyên pattern message hiện tại:

```json
{
  "uuid": "b1c3b3e2-9e8a-44bb-9483-60b55b392981",
  "timestamp": "2026-07-13T08:01:05Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "device_online",
  "message": "device is online",
  "status": true,
  "data": {
    "device_uid": "ee:7b:7f:0a:95:26",
    "serial_number": "LF-CAM-000001",
    "model": "camera_001",
    "mac": "ee:7b:7f:0a:95:26",
    "ip": "192.168.1.50",
    "firmware_version": "1.0.0",
    "uptime": 3600,
    "state": "running"
  }
}
```

## 7. Test đăng ký gương mặt qua MQTT

Gửi request:

```bash
mosquitto_pub -h 127.0.0.1 \
  -t x86-lite/local/device/mac/request \
  -m '{
    "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
    "timestamp": "2026-07-09T14:30:05Z",
    "device_id": "mac eth0",
    "command": "register_face",
    "data": {
      "employee_id": "vnpt_001",
      "name": "Vo Quoc Kha",
      "face_link": "https://leonard-tagged-specify-warming.trycloudflare.com/voquockha.png",
      "audio_link": "https://example.com/voquockha.mp3"
    }
  }'
```

`face_link` có thể là:

- URL HTTP/HTTPS.
- Đường dẫn file local trên thiết bị, ví dụ `/data/faces/a.jpg`.

`audio_link` cũng có thể là URL HTTP/HTTPS hoặc file local. Firmware dùng
`ffmpeg` chuyển về WAV PCM mono 16 kHz và lưu tại
`FACE_AUDIO_DIR/<employee_id>.wav`.
Đăng ký lại cùng `employee_id` sẽ cập nhật bản ghi hiện có.

Đặt file chào và cấu hình cooldown trước khi chạy:

```bash
mkdir -p /root/kha/audio
# Chép xinchao.wav vào /root/kha/audio/xinchao.wav
export FACE_AUDIO_DIR=/root/kha/audio
export ATTENDANCE_GREETING_AUDIO=/root/kha/audio/xinchao.wav
export ATTENDANCE_AUDIO_ENABLED=1
export ATTENDANCE_COOLDOWN_SECONDS=60
```

Trong 60 giây, mỗi `person_id` chỉ sinh một event điểm danh và một lần phát
`xinchao.wav` + audio nhân viên.

Response thành công:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-09T14:30:07Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_face",
  "message": "register success",
  "status": true,
  "data": {
    "employee_id": "vnpt_001",
    "name": "Nguyen Van A",
    "face_link": "https://example.com/face.jpg",
    "audio_link": "https://example.com/audio.mp3"
  }
}
```

Một số response lỗi có thể gặp:

```json
{
  "command": "register_face",
  "message": "face_link is empty",
  "status": false,
  "data": {}
}
```

```json
{
  "command": "register_face",
  "message": "audio_link is empty",
  "status": false,
  "data": {}
}
```

```json
{
  "command": "register_face",
  "message": "no face detected",
  "status": false,
  "data": {}
}
```

```json
{
  "command": "register_face",
  "message": "save database failed",
  "status": false,
  "data": {}
}
```

## 8. Event khi nhận diện thành công

Khi camera nhận diện được người trong `face_db.bin`, chương trình gửi event vào:

```text
x86-lite/local/device/mac/event
```

Payload hiện tại:

```json
{
  "uuid": "4e1c3bd3-0f4d-4d2a-b1a4-5c372e16d91d",
  "timestamp": "2026-07-09T14:40:00Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "face_recognized",
  "message": "recognition success",
  "status": true,
  "data": {
    "face_link": "/data/attendance/2026-07-09/NguyenVanA_144000.jpg",
    "name": "Nguyen Van A",
    "person_id": "user_001",
    "employee_id": "vnpt_001",
    "time": "2026-07-09 14:40:00",
    "confidence": 0.91,
    "distance": 0.42,
    "image_base64": "base64_image_here"
  }
}
```

Nếu ảnh base64 quá lớn, tắt bằng:

```bash
export MQTT_INCLUDE_IMAGE_BASE64=0
```

Khi đó MQTT chỉ gửi `face_link` là path ảnh đã lưu trên thiết bị.

## 9. Kiểm tra dữ liệu đã đăng ký

Sau khi đăng ký thành công, log sẽ in:

```text
[face_db] 1 registered face(s):
  [0] Nguyen Van A employee_id=vnpt_001 audio=/root/kha/audio/vnpt_001.wav
```

File DB nằm ở path truyền vào lệnh run, ví dụ:

```text
face_db.bin
```

DB mới lưu `name`, `employee_id`, đường dẫn audio local và embedding. Code vẫn
đọc được DB cũ; các trường metadata cũ được bỏ qua khi migrate.

## 10. Troubleshooting

Không connect MQTT:

```bash
nc -vz 127.0.0.1 1883
```

Không thấy response:

- Kiểm tra app đã log `[mqtt] subscribed`.
- Kiểm tra publish đúng topic `x86-lite/local/device/mac/request`.
- Mở `mosquitto_sub` trước khi gửi request.

Tải ảnh HTTPS bị lỗi:

```bash
export MQTT_FACE_ALLOW_INSECURE=1
```

Không detect được mặt:

- Ảnh phải rõ mặt, không quá nhỏ, không che nhiều.
- Thử ảnh local để loại trừ lỗi download:

```bash
mosquitto_pub -h 127.0.0.1 \
  -t x86-lite/local/device/mac/request \
  -m '{
    "uuid": "local-test-001",
    "timestamp": "2026-07-09T14:30:05Z",
    "device_id": "mac eth0",
    "command": "register_face",
    "data": {
      "employee_id": "test_001",
      "name": "Test User",
      "face_link": "/data/faces/test.jpg",
      "audio_link": "/data/audio/test_user.wav"
    }
  }'
```

## 11. Pattern message thống nhất

Tất cả request/response/event nên giữ pattern chính:

```text
uuid
timestamp
device_id
command
message
status
data
```

Với request, có thể chưa có `message` và `status`.

Có thể thêm:

```text
code
auth
```

Nhưng không phá vỡ các field chính.

## 12. Flow provisioning đã triển khai

### 12.1. Request `register_device`

Khi bật:

```bash
export MQTT_PROVISIONING_ENABLED=1
```

LuckFox publish vào:

```text
provisioning/v1/register/request
```

Payload có dạng:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-10T08:00:00Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_device",
  "data": {
    "serial_number": "LF-CAM-000001",
    "model": "camera_001",
    "type": "facial_scanner",
    "hardware": "luckfox",
    "mac": "ee:7b:7f:0a:95:26",
    "firmware_version": "1.0.0",
    "capabilities": [
      "camera",
      "face_register",
      "face_attendance",
      "mqtt"
    ],
    "reply_to": "provisioning/v1/register/response/{uuid}"
  },
  "auth": {
    "alg": "HMAC-SHA256",
    "key_id": "LF-CAM-000001",
    "nonce": "random-nonce",
    "data_hash": "sha256_of_data_json",
    "signature": "hmac_sha256_signature"
  }
}
```

### 12.2. HMAC canonical string

Server verify HMAC bằng cùng chuỗi:

```text
uuid|timestamp|device_id|command|nonce|data_hash
```

Trong đó:

- `data_hash = SHA256(data_json_string)`
- `signature = HMAC_SHA256(secret_key, canonical_string)`
- `secret_key` trên LuckFox lấy từ `MQTT_DEVICE_SECRET`
- `key_id` là `MQTT_SERIAL_NUMBER`

Không gửi `secret_key` qua MQTT.

### 12.3. Server trả pending

Server publish về topic `reply_to`:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-10T08:00:02Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_device",
  "message": "device is waiting for user approval",
  "status": true,
  "code": "WAITING_USER_APPROVAL",
  "data": {
    "register_status": "pending",
    "approval_required": true,
    "retry_after_seconds": 30
  }
}
```

LuckFox sẽ tiếp tục gửi:

```text
command = check_register_status
```

vào:

```text
provisioning/v1/register/request
```

### 12.4. Server trả approved

Để test local, server có thể trả:

```json
{
  "uuid": "uuid_luckfox_dang_cho",
  "timestamp": "2026-07-10T08:01:00Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "check_register_status",
  "message": "device approved successfully",
  "status": true,
  "code": "DEVICE_APPROVED",
  "data": {
    "register_status": "approved",
    "device_uid": "dev_8f3a91c2",
    "name": "LuckFox Camera 1",
    "mqtt_host": "127.0.0.1",
    "mqtt_port": 1883,
    "mqtt_tls": false,
    "mqtt_user": "dev_8f3a91c2",
    "mqtt_pass": "generated-long-random-password",
    "topics": {
      "status": "devices/dev_8f3a91c2/up/status",
      "telemetry": "devices/dev_8f3a91c2/up/telemetry",
      "event": "devices/dev_8f3a91c2/up/event",
      "ack": "devices/dev_8f3a91c2/up/ack",
      "command": "devices/dev_8f3a91c2/down/command",
      "config": "devices/dev_8f3a91c2/down/config",
      "ota": "devices/dev_8f3a91c2/down/ota"
    }
  }
}
```

Lưu ý: field `uuid` phải đúng với uuid trong request đang chờ. Xem log:

```text
[mqtt-provision] register_device published uuid=...
```

Sau khi nhận approved, app lưu:

```text
/data/device/credential.json
```

hoặc path trong `MQTT_CREDENTIAL_PATH`.

### 12.5. File credential sau khi lưu

```json
{
  "device_uid": "dev_8f3a91c2",
  "name": "LuckFox Camera 1",
  "mqtt_host": "127.0.0.1",
  "mqtt_port": 1883,
  "mqtt_tls": false,
  "mqtt_user": "dev_8f3a91c2",
  "mqtt_pass": "generated-long-random-password",
  "topics": {
    "status": "devices/dev_8f3a91c2/up/status",
    "telemetry": "devices/dev_8f3a91c2/up/telemetry",
    "event": "devices/dev_8f3a91c2/up/event",
    "ack": "devices/dev_8f3a91c2/up/ack",
    "command": "devices/dev_8f3a91c2/down/command",
    "config": "devices/dev_8f3a91c2/down/config",
    "ota": "devices/dev_8f3a91c2/down/ota"
  },
  "registered_at": "2026-07-10T08:01:00Z"
}
```

File được ghi với mode `0600` nếu hệ điều hành hỗ trợ.

### 12.6. Sau khi vào production

LuckFox publish `device_online` ngay khi kết nối và gửi lại định kỳ vào:

```text
devices/{device_uid}/up/status
```

Chu kỳ dùng chung biến `MQTT_STATUS_INTERVAL_SECONDS` (mặc định 60 giây).

LuckFox nhận command `register_face` từ:

```text
devices/{device_uid}/down/command
```

LuckFox gửi event điểm danh vào:

```text
devices/{device_uid}/up/event
```

Payload event production dùng command:

```text
attendance_success
```

## 13. Flow mục tiêu còn lại

Mục này giữ lại spec tổng thể để đối chiếu với server/broker. Phần lõi đã
implement ở mục 12. Những phần còn lại cần làm tiếp là TLS MQTT, exponential
backoff có jitter, ACK để xóa offline queue chắc chắn hơn, và command
`register_face` kiểu capture trực tiếp từ camera thay vì nhận `face_link`.

Mục tiêu:

1. LuckFox boot lần đầu.
2. Kiểm tra `/data/device/credential.json`.
3. Nếu chưa có credential thì vào state `REGISTERING`.
4. Connect MQTT anonymous.
5. Subscribe response topic trước.
6. Publish command `register_device`.
7. Payload có HMAC signature, không gửi `secret_key`.
8. Server verify HMAC.
9. Nếu thiết bị hợp lệ nhưng chưa được admin duyệt, server trả
   `WAITING_USER_APPROVAL`.
10. LuckFox vào state `REGISTER_PENDING`.
11. LuckFox định kỳ gửi `check_register_status`.
12. Khi server trả `DEVICE_APPROVED`, LuckFox lưu credential.
13. Disconnect MQTT anonymous.
14. Reconnect MQTT production bằng `mqtt_user` và `mqtt_pass`.
15. Sau khi authenticated mới cho phép `register_face` và gửi attendance event.

Topic provisioning:

```text
PUBLISH   provisioning/v1/register/request
SUBSCRIBE provisioning/v1/register/response/{uuid}
```

Request `register_device`:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-09T14:30:05Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_device",
  "data": {
    "serial_number": "LF-CAM-000001",
    "model": "camera_001",
    "type": "facial_scanner",
    "hardware": "luckfox",
    "mac": "ee:7b:7f:0a:95:26",
    "firmware_version": "1.0.0",
    "capabilities": [
      "camera",
      "face_register",
      "face_attendance",
      "mqtt"
    ],
    "reply_to": "provisioning/v1/register/response/42418bd2-b3b5-4445-8408-f51a939c4864"
  },
  "auth": {
    "alg": "HMAC-SHA256",
    "key_id": "LF-CAM-000001",
    "nonce": "random-nonce-string",
    "data_hash": "sha256_of_data",
    "signature": "hmac_signature_here"
  }
}
```

Response pending:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-09T14:30:07Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_device",
  "message": "device is waiting for user approval",
  "status": true,
  "code": "WAITING_USER_APPROVAL",
  "data": {
    "register_status": "pending",
    "approval_required": true,
    "retry_after_seconds": 30
  }
}
```

Response approved:

```json
{
  "uuid": "9b193f62-73df-4421-84e6-1d2236e3f1a2",
  "timestamp": "2026-07-09T14:31:10Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "check_register_status",
  "message": "device approved successfully",
  "status": true,
  "code": "DEVICE_APPROVED",
  "data": {
    "register_status": "approved",
    "device_uid": "dev_8f3a91c2",
    "name": "LuckFox Camera 1",
    "mqtt_host": "mqtt-broker-test",
    "mqtt_port": 8883,
    "mqtt_tls": true,
    "mqtt_user": "dev_8f3a91c2",
    "mqtt_pass": "generated-long-random-password",
    "topics": {
      "status": "devices/dev_8f3a91c2/up/status",
      "telemetry": "devices/dev_8f3a91c2/up/telemetry",
      "event": "devices/dev_8f3a91c2/up/event",
      "ack": "devices/dev_8f3a91c2/up/ack",
      "command": "devices/dev_8f3a91c2/down/command",
      "config": "devices/dev_8f3a91c2/down/config",
      "ota": "devices/dev_8f3a91c2/down/ota"
    }
  }
}
```

Credential lưu tại:

```text
/data/device/credential.json
```

Sau khi có credential, command vận hành nên đi qua:

```text
devices/{device_uid}/down/command
```

Event điểm danh nên đi qua:

```text
devices/{device_uid}/up/event
```

## 14. Flow mục tiêu: attendance_success

Payload event mục tiêu:

```json
{
  "uuid": "4e1c3bd3-0f4d-4d2a-b1a4-5c372e16d91d",
  "timestamp": "2026-07-09T14:40:00Z",
  "device_id": "dev_8f3a91c2",
  "command": "attendance_success",
  "message": "attendance verified successfully",
  "status": true,
  "data": {
    "person_id": "EMP001",
    "person_name": "Nguyen Van A",
    "employee_id": "vnpt_001",
    "confidence": 0.91,
    "camera_id": "cam_001",
    "capture_time": "2026-07-09T14:40:00Z",
    "image": {
      "type": "base64",
      "format": "jpg",
      "content": "base64_image_here"
    }
  }
}
```

Ghi chú: event hiện tại đang dùng command `face_recognized`. Khi triển khai
production flow, nên đổi sang `attendance_success` để khớp pattern mục tiêu.
