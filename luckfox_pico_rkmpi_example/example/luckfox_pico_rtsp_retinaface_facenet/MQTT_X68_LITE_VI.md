# Cấu hình MQTT X68 Lite

Firmware dùng MQTT 3.1.1 qua TLS, QoS 1 và `retain=false` theo giao thức
`x68-lite/v1`. Lần chạy đầu, thiết bị tự tạo và lưu một `installation_id`
UUID v4, kết nối bằng tài khoản bootstrap, gửi `activate_device`, lưu credential
Cloud cấp rồi tự kết nối lại bằng tài khoản riêng.

## Biến môi trường

```sh
export MQTT_ENABLED=1
export MQTT_HOST=mqtt.x68.space
export MQTT_PORT=8883
export MQTT_TLS_ENABLED=1
export MQTT_TLS_VERIFY_SERVER=0

export MQTT_SERIAL_NUMBER=X680001
export MQTT_DEVICE_MODEL=x68-lite
export MQTT_SOFTWARE_VERSION=1.0.0
# Tùy chọn nếu MAC không đọc được từ /sys/class/net/eth0/address:
# export MQTT_MAC=ee:7b:7f:0a:95:26

export MQTT_CREDENTIAL_PATH=/data/device/credential.json
export MQTT_CREDENTIAL_FALLBACK_PATH=/root/x68-device/credential.json
export MQTT_INSTALLATION_ID_PATH=/data/device/installation_id
export MQTT_OFFLINE_QUEUE_PATH=/root/x68-device/pending_events.jsonl
export MQTT_COMMAND_CACHE_PATH=/root/x68-device/processed_commands.jsonl
export MQTT_COMMAND_CACHE_FALLBACK_PATH=/root/x68-device/processed_commands.jsonl
export MQTT_STATUS_INTERVAL_SECONDS=60
export MQTT_RETRY_INTERVAL_SECONDS=30
export MQTT_DEBUG_PAYLOAD=0
export BENCH_LOG_ENABLED=0

# LuckFox thường không có CA store đầy đủ; mặc định firmware không verify TLS
# khi tải face_link/audio_link HTTPS. Đặt =1 nếu image đã cài CA hợp lệ.
export MQTT_FILE_TLS_VERIFY=0
export MQTT_FACE_ALLOW_INSECURE=1

export FACE_AUDIO_DIR=/root/kha/audio
export ATTENDANCE_HTTP_ENABLED=0
export ATTENDANCE_AUDIO_ENABLED=1
export ATTENDANCE_AUDIO_PLAYER=/usr/bin/aplay
export ATTENDANCE_SAVE_IMAGE=1
export ATTENDANCE_BASE_DIR=/root/attendance
export ATTENDANCE_COOLDOWN_SECONDS=60

# Nhận diện: distance càng nhỏ càng giống khuôn mặt đã đăng ký.
export FACE_DIST_THRESHOLD=0.78
export FACE_MATCH_MARGIN=0.10
export FACE_MIN_SIZE_PIXELS=100
export FACE_CONFIRM_FRAMES=3
export FACE_ENROLL_MIN_SIZE_PIXELS=120
export ANTI_SPOOF_THRESHOLD=0.85
```

Các lớp bảo vệ nhận diện mặc định:

- `FACE_DIST_THRESHOLD=0.78`: từ chối nếu khoảng cách embedding không đủ gần.
  Giá trị cũ `0.95` quá rộng và có thể nhận nhầm người.
- `FACE_MATCH_MARGIN=0.10`: nếu kết quả gần nhất và gần nhì trong DB chênh
  nhau dưới `0.10`, thiết bị coi là mơ hồ và không điểm danh.
- `FACE_MIN_SIZE_PIXELS=100`: bỏ qua khuôn mặt có chiều rộng hoặc chiều cao
  dưới 100 pixel trong ảnh camera 720x480; người dùng cần đi lại gần hơn.
- `FACE_CONFIRM_FRAMES=3`: chỉ phát sinh sự kiện sau khi cùng một danh tính
  khớp liên tiếp 3 frame.
- `FACE_ENROLL_MIN_SIZE_PIXELS=120`: ảnh đăng ký phải có đúng một người, khuôn
  mặt chính diện, không sát mép và rộng/cao ít nhất 120 pixel sau khi resize về
  640x640. Ảnh không đạt sẽ bị từ chối thay vì ghi embedding kém vào DB.

Khi tinh chỉnh, xem log `[face-match]`. Muốn chặt hơn thì giảm
`FACE_DIST_THRESHOLD` (ví dụ `0.72`) hoặc tăng `FACE_MATCH_MARGIN`. Không nên
tăng threshold lại lên `0.95`. Nếu người đúng thường có distance trên `0.78`,
nên đăng ký lại bằng ảnh chính diện, đủ sáng, chỉ có một người và khuôn mặt lớn
trước khi cân nhắc tăng nhẹ threshold.

Các giá trị bootstrap mặc định trong firmware là:

```text
Client ID: installation_id
Host: mqtt.x68.space
Port: 8883
Username: x68-bootstrap
Password: theo tài khoản bootstrap trong đặc tả
```

`MQTT_BOOTSTRAP_USERNAME` và `MQTT_BOOTSTRAP_PASSWORD` chỉ cần đặt khi Cloud
thay tài khoản bootstrap. Không đưa các biến chứa password vào log. Thiết bị chỉ
chuyển sang username/password riêng sau khi server chấp nhận kích hoạt và cấp
credential.

Response kích hoạt có thể chỉ cấp `credential_version` và `password`. Khi không
có các field còn lại, firmware dùng MAC viết thường bỏ dấu `:` làm `client_id`
và `username`, đồng thời tạo `topic_root` là
`x68-lite/v1/device/{mac}`.

`MQTT_INSTALLATION_ID` có thể dùng để cấp sẵn UUID. Nếu không đặt, firmware tự
tạo UUID và lưu tại `MQTT_INSTALLATION_ID_PATH`; file này phải được giữ nguyên
qua các lần khởi động.

Nếu không ghi được `MQTT_CREDENTIAL_PATH`, firmware sẽ thử
`MQTT_CREDENTIAL_FALLBACK_PATH`. Thiết bị chỉ gửi ACK cấu hình sau khi credential
đã được ghi thành công ở một trong hai path này.

Tương tự, nếu không ghi được `MQTT_COMMAND_CACHE_PATH`, firmware sẽ thử
`MQTT_COMMAND_CACHE_FALLBACK_PATH` để cache response của lệnh `register_face` và
`delete_face` theo `uuid`.

Đặt `MQTT_DEBUG_PAYLOAD=1` khi cần debug Cloud. Firmware sẽ in JSON publish và
receive, nhưng vẫn mask `password` và rút gọn `image_base64`. Đặt
`BENCH_LOG_ENABLED=1` nếu muốn bật lại log hiệu năng từng frame.

Không cần cấu hình `MQTT_CA_FILE`. Mặc định `MQTT_TLS_VERIFY_SERVER=0` để phù hợp
với firmware LuckFox không có kho CA: kết nối vẫn được mã hóa TLS nhưng không
xác minh danh tính broker. Nếu image có kho CA, nên đặt
`MQTT_TLS_VERIFY_SERVER=1`; có thể dùng thêm `MQTT_CA_FILE` cho CA riêng.
`MQTT_TLS_ENABLED=0` chỉ dành cho broker cục bộ khi phát triển.

Tải file `face_link`/`audio_link` qua HTTPS cũng mặc định không verify certificate
vì cùng lý do thiếu CA store (`MQTT_FILE_TLS_VERIFY=0`). Biến cũ
`MQTT_FACE_ALLOW_INSECURE=1` vẫn dùng được; nếu muốn bắt buộc verify thì đặt
`MQTT_FILE_TLS_VERIFY=1` hoặc `MQTT_FACE_ALLOW_INSECURE=0`.

`ATTENDANCE_HTTP_ENABLED=0` tắt hẳn HTTP local attendance: firmware không post
tới `ATTENDANCE_SERVER_HOST`, không tạo/ghi `attendance_queue.jsonl`, và không
retry queue cũ. MQTT recognition event vẫn được gửi qua Cloud khi nhận diện
thành công.

## Topic

Bootstrap dùng `installation_id`:

```text
x68-lite/v1/bootstrap/{installation_id}/request
x68-lite/v1/bootstrap/{installation_id}/response
x68-lite/v1/bootstrap/{installation_id}/ack
```

Sau khi kích hoạt, firmware lấy `topic_root` từ response và dùng:

```text
{topic_root}/request   Cloud -> thiết bị
{topic_root}/ack       Cloud -> thiết bị
{topic_root}/response  thiết bị -> Cloud
{topic_root}/event     thiết bị -> Cloud
{topic_root}/status    thiết bị -> Cloud
```

Firmware subscribe `response` trước khi gửi yêu cầu bootstrap. Ở production,
firmware đặt `clean_session=false` và subscribe cả `request` lẫn `ack`.

## Retry và chống trùng

- Yêu cầu kích hoạt được gửi lại sau 30 giây với cùng `uuid`.
- Sự kiện nhận diện được ghi xuống `MQTT_OFFLINE_QUEUE_PATH` trước khi publish,
  chỉ xóa khi Cloud gửi ACK `status=true` có cùng `uuid`.
- Lệnh `register_face` và `delete_face` lặp lại cùng `uuid` nhận lại response đã
  cache bền vững, không chạy handler lần hai kể cả sau khi firmware khởi động lại.
- Status `device_status` được gửi ngay sau khi kết nối production và mỗi 60 giây
  theo topic `{topic_root}/status`.

Credential được lưu với quyền `0600`. Firmware không ghi password, URL tải ảnh,
URL tải audio hoặc nội dung Base64 vào log.
