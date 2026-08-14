# luckfox_pico_rtsp_best_facenet

Ứng dụng giữ nguyên pipeline của `luckfox_pico_rtsp_retinaface_facenet`
(RTSP, FaceNet, anti-spoof, face DB, chấm công, MQTT và Telegram), nhưng dùng
`model/best_rknpu_rv1106_int8.rknn` để phát hiện khuôn mặt.

## Build

```sh
export LUCKFOX_SDK_PATH=/duong-dan/toi/luckfox-pico
./build.sh
```

Chọn `uclibc`, sau đó chọn `luckfox_pico_rtsp_best_facenet`. Gói cài đặt được
tạo tại:

```text
install/uclibc/luckfox_pico_rtsp_best_facenet_demo/
```

## Chạy

```sh
./luckfox_pico_rtsp_best_facenet run \
  model/best_rknpu_rv1106_int8.rknn \
  model/facenet.rknn \
  face_db.bin \
  model/minifasnet_v2_80x80.rknn
```

`facenet.rknn` và database vẫn dùng đúng phiên bản của ứng dụng cũ.

## Web điều khiển local

Khi chạy ở chế độ `run`, mở trình duyệt tại:

```text
http://172.32.0.93:8080
```

Dashboard cho phép xem trạng thái, danh sách nhân viên, đăng ký bằng ảnh và
audio tùy chọn, xóa nhân viên, xem camera MJPEG trực tiếp có bounding box/tên,
và sao chép địa chỉ RTSP. API local gồm:

- `GET /api/status`
- `GET /api/employees`
- `POST /api/employees` (`multipart/form-data`: `employee_id`, `name`,
  `face`, và `audio` tùy chọn)
- `DELETE /api/employees/{employee_id}`
- `GET /stream.mjpg`

Phiên bản này không khởi động MQTT; mọi thao tác quản lý được thực hiện qua
HTTP trong mạng local.

## Lưu ý về landmark

Model `best_rknpu_rv1106_int8.rknn` có ba output YOLOv5 18 kênh và chỉ xuất
bounding box/confidence, không xuất 5 facial landmark. Để giữ nguyên API và
luồng align FaceNet, detector mới sinh landmark chuẩn theo tỉ lệ bounding box.
Cách này tốt nhất với khuôn mặt nhìn thẳng; khuôn mặt nghiêng có thể nhận diện
kém chính xác hơn RetinaFace.
