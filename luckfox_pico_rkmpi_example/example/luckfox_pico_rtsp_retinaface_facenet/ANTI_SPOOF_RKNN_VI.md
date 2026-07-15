# Anti-spoof RKNN trên RV1106

## Luồng xử lý

Luồng camera chạy theo thứ tự:

1. RetinaFace tìm khuôn mặt.
2. MiniFASNetV2 kiểm tra khuôn mặt thật/giả trên crop mở rộng 2,7 lần.
3. Chỉ khuôn mặt thật mới được đưa qua FaceNet để nhận dạng danh tính.
4. `FaceEventManager` chỉ ghi điểm danh khi nhận được `liveness_verified=true`.

Người dùng không cần quay đầu, chớp mắt hoặc cười. Ảnh/màn hình bị chặn sẽ có
nhãn `SPOOF BLOCKED`; không phát MQTT, Telegram hoặc sự kiện điểm danh.

Nếu model không tồn tại, không tương thích hoặc không khởi tạo được, chương
trình dừng thay vì chạy nhận dạng không có chống giả mạo.

## Chạy

Model mặc định đã được đóng gói tại:

```text
model/minifasnet_v2_80x80.rknn
```

Lệnh cũ vẫn dùng được khi chạy từ thư mục demo đã cài đặt:

```bash
./luckfox_pico_rtsp_retinaface_facenet \
  run model/retinaface.rknn model/facenet.rknn face_db.bin
```

Có thể truyền đường dẫn model tường minh ở tham số cuối:

```bash
./luckfox_pico_rtsp_retinaface_facenet \
  run model/retinaface.rknn model/facenet.rknn face_db.bin \
  model/minifasnet_v2_80x80.rknn
```

Ngưỡng mặc định của lớp khuôn mặt thật là `0.80`. Chỉ điều chỉnh sau khi thu
thập dữ liệu thật/giả từ đúng camera và môi trường triển khai:

```bash
export ANTI_SPOOF_THRESHOLD=0.85
```

Khoảng hợp lệ là `0.50` đến `0.99`. Tăng ngưỡng giúp chặn giả mạo chặt hơn
nhưng có thể từ chối người thật trong ánh sáng xấu.

## Thông tin model

- Kiến trúc: MiniFASNetV2, đầu vào BGR `80x80`, ba lớp; lớp `1` là mặt thật.
- Nguồn checkpoint: `minivision-ai/Silent-Face-Anti-Spoofing`, Apache-2.0.
- Target: `rv1106`, INT8 asymmetric.
- RKNN-Toolkit2: `1.6.0+81f21f4d`, khớp `librknnmrt 1.6.0` của SDK.
- SHA-256: `b21beff5d5e8f706f0d30f4e17817f98ae948d47d4097ffc11dabc8fd34dbc87`.

Hai script trong `tools/` cho phép xuất lại checkpoint sang ONNX và chuyển
ONNX sang RKNN. Khi phát hành chính thức, nên lượng tử hóa lại bằng 100–500
crop khuôn mặt từ chính camera SC3336, gồm người thật, ảnh giấy và nhiều loại
màn hình điện thoại dưới các điều kiện sáng khác nhau.

> RGB anti-spoof phụ thuộc camera và môi trường. Cần đo FAR/FRR bằng dữ liệu
> thực tế trước khi dùng cho kiểm soát truy cập có mức rủi ro cao; IR/depth vẫn
> là lựa chọn mạnh hơn trước video replay hoặc mặt nạ tinh vi.
