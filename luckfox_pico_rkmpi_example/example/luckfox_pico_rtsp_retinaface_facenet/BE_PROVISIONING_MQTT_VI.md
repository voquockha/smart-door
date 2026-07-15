# Tài liệu BE: Provisioning MQTT cho thiết bị LuckFox

Tài liệu này mô tả logic server/backend cần implement để thiết bị LuckFox:

1. Đăng ký thiết bị lần đầu qua MQTT anonymous.
2. Server verify HMAC.
3. Server chờ admin/user duyệt thiết bị.
4. Server cấp credential MQTT production.
5. Thiết bị reconnect bằng `mqtt_user/mqtt_pass`.
6. Server giao tiếp command/event với thiết bị qua topic production.

## 1. Pattern message chung

Mọi message nên giữ các field chính:

```json
{
  "uuid": "message-uuid",
  "timestamp": "2026-07-10T08:00:00Z",
  "device_id": "device-id-or-mac",
  "command": "command_name",
  "message": "human readable message",
  "status": true,
  "code": "OPTIONAL_CODE",
  "data": {}
}
```

Với request từ device, `message`, `status`, `code` có thể chưa có.

## 2. Hai mode giao tiếp MQTT

Hiện firmware có 2 mode:

```text
LOCAL TEST
  Dùng để test nhanh broker local, chưa cần provisioning/credential.

PROVISIONING + PRODUCTION
  Dùng cho flow thật: đăng ký thiết bị, duyệt thiết bị, cấp credential,
  rồi giao tiếp qua topic riêng của device.
```

### 2.1. Topic local test hiện tại

Khi chưa bật provisioning, app dùng topic mặc định:

```text
Server -> Device command:  x86-lite/local/device/mac/request
Device -> Server response: x86-lite/local/device/mac/response
Device -> Server event:    x86-lite/local/device/mac/event
```

Format `register_face` local:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-10T08:05:00Z",
  "device_id": "mac eth0",
  "command": "register_face",
  "data": {
    "face_link": "https://example.com/person.jpg",
    "name": "Nguyen Van A",
    "sex": "Nam",
    "cccd": "1234567890",
    "employee_id": "vnpt_001",
    "company_id": "vnpt"
  }
}
```

Response local:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-10T08:05:03Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_face",
  "message": "register success",
  "status": true,
  "data": {
    "face_link": "https://example.com/person.jpg",
    "name": "Nguyen Van A",
    "sex": "Nam",
    "cccd": "1234567890",
    "employee_id": "vnpt_001",
    "company_id": "vnpt"
  }
}
```

Event nhận diện local:

```json
{
  "uuid": "4e1c3bd3-0f4d-4d2a-b1a4-5c372e16d91d",
  "timestamp": "2026-07-10T08:10:00Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "face_recognized",
  "message": "recognition success",
  "status": true,
  "data": {
    "face_link": "/data/attendance/2026-07-10/NguyenVanA_081000.bmp",
    "name": "Nguyen Van A",
    "person_id": "Nguyen Van A",
    "time": "2026-07-10 08:10:00",
    "confidence": 0.91,
    "distance": 0.42,
    "employee_id": "vnpt_001",
    "company_id": "vnpt"
  }
}
```

Local test không bắt buộc HMAC. Flow HMAC nằm ở provisioning bên dưới.

## 3. Topic provisioning

Khi chưa có credential, LuckFox dùng MQTT anonymous.

Device publish:

```text
provisioning/v1/register/request
```

Device subscribe:

```text
provisioning/v1/register/response/{uuid}
```

Ví dụ:

```text
provisioning/v1/register/response/42418bd2-b3b5-4445-8408-f51a939c4864
```

BE phải publish response đúng vào topic `data.reply_to` mà device gửi.

ACL anonymous đề xuất:

```text
WRITE provisioning/v1/register/request
READ  provisioning/v1/register/response/+
```

Anonymous không được đọc/ghi topic production.

## 4. Secret và HMAC

Mỗi thiết bị cần có secret ban đầu.

Ví dụ bảng server:

```text
serial_number: LF-CAM-000001
secret_key:    luckfox-dev-secret
status:        new | pending | approved | rejected
```

Trên LuckFox, secret tương ứng là env:

```bash
MQTT_SERIAL_NUMBER=LF-CAM-000001
MQTT_DEVICE_SECRET=luckfox-dev-secret
```

Không gửi `secret_key` qua MQTT.

## 5. Cách verify HMAC

Device gửi `auth`:

```json
{
  "alg": "HMAC-SHA256",
  "key_id": "LF-CAM-000001",
  "nonce": "random-nonce",
  "data_hash": "sha256_of_data_json",
  "signature": "hmac_sha256_signature"
}
```

BE verify theo thứ tự:

1. Parse JSON request.
2. Lấy `auth.key_id`.
3. Tìm `secret_key` theo `key_id`.
4. Tính lại `data_hash`.
5. Tạo canonical string.
6. Tính HMAC-SHA256.
7. So sánh constant-time với `auth.signature`.
8. Check timestamp/nonce để chống replay.

Canonical string hiện tại:

```text
uuid|timestamp|device_id|command|nonce|data_hash
```

Ví dụ:

```text
42418bd2-b3b5-4445-8408-f51a939c4864|2026-07-10T08:00:00Z|ee:7b:7f:0a:95:26|register_device|random-nonce|abc123...
```

HMAC:

```text
signature = HEX(HMAC_SHA256(secret_key, canonical_string))
```

### Lưu ý quan trọng về `data_hash`

Firmware hiện tính:

```text
data_hash = HEX(SHA256(data_json_string))
```

Trong đó `data_json_string` là JSON object `data` dạng compact, không có space.

Ví dụ string được hash:

```json
{"serial_number":"LF-CAM-000001","model":"camera_001","type":"facial_scanner","hardware":"luckfox","mac":"ee:7b:7f:0a:95:26","firmware_version":"1.0.0","capabilities":["camera","face_register","face_attendance","mqtt"],"reply_to":"provisioning/v1/register/response/42418bd2-b3b5-4445-8408-f51a939c4864"}
```

BE nên làm một trong hai cách:

- Tốt nhất: extract raw substring của object `data` từ request body rồi SHA256 chính substring đó.
- Hoặc canonicalize JSON về đúng format compact và đúng thứ tự field như firmware.

Không nên parse rồi stringify tùy ý vì thứ tự field/space khác sẽ làm hash khác.

Pseudo-code:

```text
request = parse_json(body)
auth = request.auth
secret = find_secret_by_key_id(auth.key_id)

raw_data_json = extract_raw_json_object(body, "data")
computed_data_hash = sha256_hex(raw_data_json)
assert computed_data_hash == auth.data_hash

canonical = request.uuid + "|" +
            request.timestamp + "|" +
            request.device_id + "|" +
            request.command + "|" +
            auth.nonce + "|" +
            auth.data_hash

computed_signature = hmac_sha256_hex(secret, canonical)
assert constant_time_equal(computed_signature, auth.signature)
```

Replay protection:

- Reject nếu `timestamp` lệch quá 5 phút so với server.
- Lưu nonce theo `key_id` trong TTL 5-10 phút.
- Reject nếu nonce đã dùng.

Ví dụ Node.js:

```js
const crypto = require("crypto");

function sha256Hex(input) {
  return crypto.createHash("sha256").update(input, "utf8").digest("hex");
}

function hmacSha256Hex(secret, input) {
  return crypto.createHmac("sha256", secret).update(input, "utf8").digest("hex");
}

function timingSafeEqualHex(a, b) {
  const ab = Buffer.from(a, "hex");
  const bb = Buffer.from(b, "hex");
  return ab.length === bb.length && crypto.timingSafeEqual(ab, bb);
}

function verifyDeviceHmac({ body, rawDataJson, secret }) {
  const req = JSON.parse(body);
  const auth = req.auth || {};

  const dataHash = sha256Hex(rawDataJson);
  if (!timingSafeEqualHex(dataHash, auth.data_hash)) {
    return { ok: false, code: "INVALID_DATA_HASH" };
  }

  const canonical = [
    req.uuid,
    req.timestamp,
    req.device_id,
    req.command,
    auth.nonce,
    auth.data_hash,
  ].join("|");

  const signature = hmacSha256Hex(secret, canonical);
  if (!timingSafeEqualHex(signature, auth.signature)) {
    return { ok: false, code: "INVALID_SIGNATURE" };
  }

  return { ok: true, keyId: auth.key_id };
}
```

`rawDataJson` là phần raw JSON của object `data` trong body MQTT. Nếu BE dùng parser làm mất raw body, cần canonicalize JSON đúng thứ tự field như firmware.

## 6. Flow tổng thể

```text
INIT DEVICE
  |
  | Chưa có /data/device/credential.json
  v
CONNECT MQTT ANONYMOUS
  |
  | subscribe provisioning/v1/register/response/{uuid}
  | publish register_device
  v
SERVER VERIFY HMAC
  |
  | HMAC sai
  v
INVALID_SIGNATURE

SERVER VERIFY HMAC
  |
  | HMAC đúng, chưa được duyệt
  v
WAITING_USER_APPROVAL
  |
  | device định kỳ publish check_register_status
  v
ADMIN APPROVES DEVICE
  |
  | server trả DEVICE_APPROVED + mqtt credential
  v
DEVICE SAVES credential.json
  |
  | disconnect anonymous, reconnect production
  v
RUNNING
```

## 7. Request `register_device`

Topic:

```text
provisioning/v1/register/request
```

Payload:

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
    "reply_to": "provisioning/v1/register/response/42418bd2-b3b5-4445-8408-f51a939c4864"
  },
  "auth": {
    "alg": "HMAC-SHA256",
    "key_id": "LF-CAM-000001",
    "nonce": "random-nonce-string",
    "data_hash": "sha256_of_data_json",
    "signature": "hmac_signature_here"
  }
}
```

BE xử lý:

1. Verify HMAC.
2. Upsert device theo `serial_number` hoặc `mac`.
3. Nếu chưa có user/admin duyệt, lưu trạng thái `pending`.
4. Publish response pending vào `data.reply_to`.

## 8. Response lỗi HMAC sai

Topic:

```text
provisioning/v1/register/response/{uuid}
```

Payload:

```json
{
  "uuid": "42418bd2-b3b5-4445-8408-f51a939c4864",
  "timestamp": "2026-07-10T08:00:02Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "register_device",
  "message": "invalid device signature",
  "status": false,
  "code": "INVALID_SIGNATURE",
  "data": {}
}
```

## 9. Response pending chờ duyệt

Topic:

```text
provisioning/v1/register/response/{uuid}
```

Payload:

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
    "display_name": "LuckFox Camera LF-CAM-000001",
    "serial_number": "LF-CAM-000001",
    "retry_after_seconds": 30
  }
}
```

Device sẽ giữ MQTT anonymous và không chạy command production.

## 10. Request `check_register_status`

Khi đang pending, device định kỳ hỏi lại.

Topic:

```text
provisioning/v1/register/request
```

Payload:

```json
{
  "uuid": "9b193f62-73df-4421-84e6-1d2236e3f1a2",
  "timestamp": "2026-07-10T08:00:30Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "check_register_status",
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
    "reply_to": "provisioning/v1/register/response/9b193f62-73df-4421-84e6-1d2236e3f1a2"
  },
  "auth": {
    "alg": "HMAC-SHA256",
    "key_id": "LF-CAM-000001",
    "nonce": "random-nonce-string",
    "data_hash": "sha256_of_data_json",
    "signature": "hmac_signature_here"
  }
}
```

Nếu vẫn chưa duyệt, BE trả:

```json
{
  "uuid": "9b193f62-73df-4421-84e6-1d2236e3f1a2",
  "timestamp": "2026-07-10T08:00:31Z",
  "device_id": "ee:7b:7f:0a:95:26",
  "command": "check_register_status",
  "message": "device is still waiting for user approval",
  "status": true,
  "code": "WAITING_USER_APPROVAL",
  "data": {
    "register_status": "pending",
    "approval_required": true,
    "retry_after_seconds": 30
  }
}
```

## 11. Response approved và cấp credential

Khi admin/user đã duyệt thiết bị, BE tạo credential MQTT production.

Topic:

```text
provisioning/v1/register/response/{uuid}
```

Payload:

```json
{
  "uuid": "9b193f62-73df-4421-84e6-1d2236e3f1a2",
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
    "mqtt_host": "mqtt-broker.local",
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

Lưu ý hiện tại firmware MQTT raw TCP chưa hỗ trợ TLS. Trong giai đoạn test, BE nên trả:

```json
{
  "mqtt_port": 1883,
  "mqtt_tls": false
}
```

## 12. Credential được lưu trên device

Device lưu credential vào:

```text
/data/device/credential.json
```

Nội dung:

```json
{
  "device_uid": "dev_8f3a91c2",
  "name": "LuckFox Camera 1",
  "mqtt_host": "mqtt-broker.local",
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

Từ lần boot sau, nếu file này hợp lệ, device bỏ qua provisioning và connect production trực tiếp.

## 13. Topic production

Sau khi authenticated, broker ACL nên giới hạn theo user.

Nếu:

```text
mqtt_user = dev_8f3a91c2
```

ACL:

```text
WRITE devices/dev_8f3a91c2/up/#
READ  devices/dev_8f3a91c2/down/#
```

Hoặc với Mosquitto pattern:

```text
pattern write devices/%u/up/#
pattern read  devices/%u/down/#
```

Topic:

```text
Device -> Server status: devices/{device_uid}/up/status
Device -> Server telemetry: devices/{device_uid}/up/telemetry
Device -> Server event: devices/{device_uid}/up/event
Device -> Server ack: devices/{device_uid}/up/ack

Server -> Device command: devices/{device_uid}/down/command
Server -> Device config: devices/{device_uid}/down/config
Server -> Device ota: devices/{device_uid}/down/ota
```

## 14. Device online event

Sau khi connect production thành công, device publish:

Topic:

```text
devices/{device_uid}/up/status
```

Payload:

```json
{
  "uuid": "b1c3b3e2-9e8a-44bb-9483-60b55b392981",
  "timestamp": "2026-07-10T08:01:05Z",
  "device_id": "dev_8f3a91c2",
  "command": "device_online",
  "message": "device is online",
  "status": true,
  "data": {
    "device_uid": "dev_8f3a91c2",
    "mac": "ee:7b:7f:0a:95:26",
    "ip": "192.168.1.50",
    "firmware_version": "1.0.0",
    "uptime": 0,
    "state": "running"
  }
}
```

## 15. Server gửi command register_face

Topic:

```text
devices/{device_uid}/down/command
```

Payload hiện firmware đang hỗ trợ:

```json
{
  "uuid": "af9b5a53-7ef6-4e61-b7f3-6d6f39ed1b92",
  "timestamp": "2026-07-10T08:05:00Z",
  "device_id": "dev_8f3a91c2",
  "command": "register_face",
  "data": {
    "face_link": "https://example.com/person.jpg",
    "name": "Nguyen Van A",
    "sex": "Nam",
    "cccd": "1234567890"
  }
}
```

Response/ACK hiện tại device publish vào topic `ack` trong credential:

```text
devices/{device_uid}/up/ack
```

Payload:

```json
{
  "uuid": "af9b5a53-7ef6-4e61-b7f3-6d6f39ed1b92",
  "timestamp": "2026-07-10T08:05:03Z",
  "device_id": "dev_8f3a91c2",
  "command": "register_face",
  "message": "register success",
  "status": true,
  "data": {
    "face_link": "https://example.com/person.jpg",
    "name": "Nguyen Van A",
    "sex": "Nam",
    "cccd": "1234567890"
  }
}
```

Lỗi:

```json
{
  "uuid": "af9b5a53-7ef6-4e61-b7f3-6d6f39ed1b92",
  "timestamp": "2026-07-10T08:05:03Z",
  "device_id": "dev_8f3a91c2",
  "command": "register_face",
  "message": "no face detected",
  "status": false,
  "data": {
    "face_link": "https://example.com/person.jpg",
    "name": "Nguyen Van A",
    "sex": "Nam",
    "cccd": "1234567890"
  }
}
```

## 16. Device gửi event attendance_success

Topic:

```text
devices/{device_uid}/up/event
```

Payload khi production:

```json
{
  "uuid": "4e1c3bd3-0f4d-4d2a-b1a4-5c372e16d91d",
  "timestamp": "2026-07-10T08:10:00Z",
  "device_id": "dev_8f3a91c2",
  "command": "attendance_success",
  "message": "attendance verified successfully",
  "status": true,
  "data": {
    "person_id": "user_001",
    "person_name": "Nguyen Van A",
    "confidence": 0.91,
    "distance": 0.42,
    "camera_id": "cam_001",
    "capture_time": "2026-07-10T08:10:00Z",
    "image": {
      "type": "path",
      "format": "bmp",
      "content": "/data/attendance/2026-07-10/NguyenVanA_081000.bmp"
    }
  }
}
```

Mặc định firmware hiện không gửi base64 để tránh payload MQTT quá lớn.

Nếu bật:

```bash
MQTT_INCLUDE_IMAGE_BASE64=1
```

thì `image.type = "base64"` và `image.content` là base64.

Khuyến nghị BE production:

- MQTT chỉ nhận metadata.
- Ảnh nên upload HTTP/object storage riêng, MQTT chỉ gửi `image_url` hoặc `image_id`.

## 17. Code lỗi chuẩn đề xuất

```text
INVALID_SIGNATURE
REQUEST_EXPIRED
REPLAY_NONCE
DEVICE_NOT_FOUND
WAITING_USER_APPROVAL
DEVICE_APPROVED
DEVICE_REJECTED
INVALID_CREDENTIAL
UNSUPPORTED_COMMAND
NO_FACE_DETECTED
DOWNLOAD_FACE_FAILED
DATABASE_FULL
```

## 18. Checklist BE cần làm

- Tạo bảng pre-shared device secret theo `serial_number`.
- Implement MQTT subscriber cho `provisioning/v1/register/request`.
- Verify HMAC cho `register_device` và `check_register_status`.
- Lưu trạng thái device pending/approved/rejected.
- API/admin UI để duyệt thiết bị.
- Generate `device_uid`, `mqtt_user`, `mqtt_pass`.
- Tạo ACL broker cho user/device.
- Publish response vào `data.reply_to`.
- Subscribe `devices/+/up/status`, `devices/+/up/event`, `devices/+/up/ack`.
- Publish command vào `devices/{device_uid}/down/command`.
