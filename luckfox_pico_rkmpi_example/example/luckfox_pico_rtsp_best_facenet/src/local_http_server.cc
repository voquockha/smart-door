#include "local_http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <jpeglib.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace {

const char kDashboard[] = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>xFace</title><style>
:root{--ink:#17202a;--muted:#6b7280;--paper:#f4f1ea;--card:#fff;--brand:#087f8c;--accent:#ff9f1c;--danger:#c2413b}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(135deg,#e5f4f2,#f8eee0);color:var(--ink);font:15px system-ui,sans-serif;min-height:100vh}
header{background:#102a32;color:white;padding:24px max(5vw,20px);display:flex;justify-content:space-between;align-items:center}h1{margin:0;font-size:24px}header small{color:#9bd8d5}
nav{background:white;border-bottom:1px solid #dfe7e7;padding:0 max(3vw,20px);display:flex;gap:8px}.tab-button{color:var(--muted);background:transparent;border-radius:0;padding:16px 20px;border-bottom:3px solid transparent}.tab-button.active{color:var(--brand);border-bottom-color:var(--brand)}main{width:min(1380px,94vw);margin:22px auto}.tab-page{display:none}.tab-page.active{display:grid}.attendance{grid-template-columns:2.2fr .9fr;gap:18px;align-items:start}.management{grid-template-columns:.8fr 1.4fr;gap:18px;align-items:start}.card{background:var(--card);border-radius:18px;padding:20px;box-shadow:0 12px 35px #24434b1c}.camera img{width:100%;aspect-ratio:3/2;object-fit:contain;background:#111;border-radius:12px}.attendance-list{max-height:620px;overflow:auto}.attendance-item{border-left:4px solid var(--brand);background:#f1f8f7;padding:12px;margin:0 0 10px;border-radius:8px}.attendance-name{font-size:17px;font-weight:800}.attendance-meta{color:var(--muted);font-size:13px;margin-top:5px}.device{grid-column:1/-1}
h2{margin:0 0 16px;font-size:18px}.status{display:flex;gap:10px;align-items:center}.dot{width:11px;height:11px;border-radius:50%;background:#22c55e;box-shadow:0 0 0 5px #22c55e22}.rtsp{background:#e9f4f3;border-radius:10px;padding:12px;font-family:monospace;word-break:break-all}
label{display:block;color:var(--muted);margin:12px 0 5px}input{width:100%;padding:11px;border:1px solid #d4d7da;border-radius:9px;background:white}button{border:0;border-radius:9px;padding:11px 16px;background:var(--brand);color:white;font-weight:700;cursor:pointer}button.danger{background:var(--danger);padding:7px 11px}button:disabled{opacity:.55}
table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:11px;border-bottom:1px solid #eee}th{color:var(--muted)}#msg{min-height:22px;margin-top:12px;font-weight:600}.ok{color:#087f5b}.err{color:var(--danger)}
@media(max-width:900px){.attendance,.management{grid-template-columns:1fr}.device{grid-column:1}header{align-items:flex-start;flex-direction:column;gap:7px}.tab-button{padding:13px 10px}}
</style></head><body><header><div><h1>xFace</h1><small>Hệ thống nhận diện và chấm công khuôn mặt</small></div><div class="status"><span class="dot"></span><span id="state">Đang kết nối…</span></div></header>
<nav><button class="tab-button active" data-tab="attendance">Điểm danh trực tiếp</button><button class="tab-button" data-tab="employees">Quản lý nhân viên</button></nav><main>
<section id="attendance" class="tab-page attendance active"><div class="card camera"><h2>Camera trực tiếp</h2><img src="/stream.mjpg" alt="Camera xFace"></div><aside class="card"><h2>Danh sách người điểm danh</h2><div id="attendanceRows" class="attendance-list"><div class="latest-empty">Chưa có lượt điểm danh</div></div></aside><section class="card device"><b id="device">Đang tải trạng thái…</b> · <span class="rtsp" id="rtsp">rtsp://172.32.0.93:554/live/0</span> <button onclick="copyRtsp()">Sao chép RTSP</button></section></section>
<section id="employees" class="tab-page management"><section class="card"><h2>Đăng ký nhân viên</h2><form id="form"><label>Mã nhân viên</label><input name="employee_id" required maxlength="63"><label>Họ và tên</label><input name="name" required maxlength="63"><label>Ảnh khuôn mặt</label><input name="face" type="file" accept="image/*" required><label>Âm thanh chào (không bắt buộc)</label><input name="audio" type="file" accept="audio/*"><p><button id="submit">Đăng ký khuôn mặt</button></p><div id="msg"></div></form></section><section class="card"><h2>Nhân viên đã đăng ký <span id="count"></span></h2><table><thead><tr><th>Mã nhân viên</th><th>Họ và tên</th><th>Âm thanh</th><th></th></tr></thead><tbody id="rows"></tbody></table></section></section></main>
<script>
const $=s=>document.querySelector(s);let statusData={};
async function refresh(){try{statusData=await fetch('/api/status').then(r=>r.json());$('#state').textContent='Đang hoạt động';$('#device').innerHTML=`HTTP :${statusData.http_port} · ${statusData.employee_count} nhân viên`;$('#rtsp').textContent=statusData.rtsp_url;const [es,history]=await Promise.all([fetch('/api/employees').then(r=>r.json()),fetch('/api/attendance').then(r=>r.json())]);$('#count').textContent=`(${es.length})`;$('#rows').innerHTML=es.map(e=>`<tr><td>${esc(e.employee_id)}</td><td>${esc(e.name)}</td><td>${e.has_audio?'Có':'Không'}</td><td><button class="danger" onclick="del('${encodeURIComponent(e.employee_id)}')">Xóa</button></td></tr>`).join('')||'<tr><td colspan="4">Chưa có dữ liệu</td></tr>';$('#attendanceRows').innerHTML=history.map(x=>`<div class="attendance-item"><div class="attendance-name">${esc(x.name)}</div><div>Mã nhân viên: <b>${esc(x.employee_id)}</b></div><div class="attendance-meta">${esc(x.time)} · Tin cậy ${Number(x.confidence).toFixed(1)}% · Distance ${Number(x.distance).toFixed(3)}</div></div>`).join('')||'<div class="latest-empty">Chưa có lượt điểm danh</div>'}catch(e){$('#state').textContent='Mất kết nối'}}
function esc(v){return String(v).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
async function del(id){if(!confirm('Xóa nhân viên này?'))return;const r=await fetch('/api/employees/'+id,{method:'DELETE'});const j=await r.json();alert(j.message);refresh()}
function copyRtsp(){navigator.clipboard.writeText($('#rtsp').textContent)}
document.querySelectorAll('.tab-button').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tab-button,.tab-page').forEach(x=>x.classList.remove('active'));b.classList.add('active');$('#'+b.dataset.tab).classList.add('active')});
$('#form').onsubmit=async e=>{e.preventDefault();$('#submit').disabled=true;$('#msg').textContent='Đang xử lý ảnh trên NPU…';$('#msg').className='';try{const r=await fetch('/api/employees',{method:'POST',body:new FormData(e.target)});const j=await r.json();$('#msg').textContent=j.message;$('#msg').className=j.ok?'ok':'err';if(j.ok){e.target.reset();refresh()}}catch(x){$('#msg').textContent='Không kết nối được thiết bị';$('#msg').className='err'}finally{$('#submit').disabled=false}};refresh();setInterval(refresh,5000);
</script></body></html>)HTML";

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if ((unsigned char)c >= 0x20) out += c;
    }
    return out;
}

void respond(int fd, int code, const char* type, const std::string& body) {
    const char* text = code == 200 ? "OK" : code == 201 ? "Created" :
                       code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                       "Internal Server Error";
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", code, text, type, body.size());
    send(fd, head, n, MSG_NOSIGNAL);
    size_t sent = 0;
    while (sent < body.size()) {
        ssize_t r = send(fd, body.data() + sent, body.size() - sent, MSG_NOSIGNAL);
        if (r <= 0) break;
        sent += (size_t)r;
    }
}

std::string headerValue(const std::string& headers, const std::string& name) {
    std::string needle = name + ":";
    size_t p = headers.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size(); while (p < headers.size() && headers[p] == ' ') ++p;
    size_t e = headers.find("\r\n", p);
    return headers.substr(p, e - p);
}

bool writeTemp(const std::string& data, std::string* path) {
    char name[] = "/tmp/http_upload_XXXXXX";
    int fd = mkstemp(name); if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) { ssize_t n = write(fd, data.data()+done, data.size()-done); if (n <= 0) { close(fd); unlink(name); return false; } done += n; }
    close(fd); *path = name; return true;
}

struct Part { std::string value; std::string filename; };
std::map<std::string,Part> parseMultipart(const std::string& body, const std::string& boundary) {
    std::map<std::string,Part> out; const std::string mark = "--" + boundary; size_t p = 0;
    while ((p = body.find(mark, p)) != std::string::npos) {
        p += mark.size(); if (body.compare(p,2,"--") == 0) break; if (body.compare(p,2,"\r\n") == 0) p += 2;
        size_t h = body.find("\r\n\r\n", p); if (h == std::string::npos) break;
        std::string head = body.substr(p,h-p); size_t end = body.find("\r\n"+mark,h+4); if (end == std::string::npos) break;
        size_t np=head.find("name=\""); if(np!=std::string::npos){np+=6;size_t ne=head.find('"',np);std::string name=head.substr(np,ne-np);Part part;part.value=body.substr(h+4,end-(h+4));size_t fp=head.find("filename=\"");if(fp!=std::string::npos){fp+=10;size_t fe=head.find('"',fp);part.filename=head.substr(fp,fe-fp);}out[name]=part;} p=end+2;
    } return out;
}
} // namespace

LocalHttpServer::LocalHttpServer(int port):port_(port),listen_fd_(-1),running_(false){}
LocalHttpServer::~LocalHttpServer(){stop();}
void LocalHttpServer::setRegisterHandler(RegisterHandler h){register_handler_=std::move(h);} void LocalHttpServer::setDeleteHandler(DeleteHandler h){delete_handler_=std::move(h);} void LocalHttpServer::setEmployeesHandler(EmployeesHandler h){employees_handler_=std::move(h);} void LocalHttpServer::setStatusHandler(StatusHandler h){status_handler_=std::move(h);}
bool LocalHttpServer::start(){if(running_)return true;listen_fd_=socket(AF_INET,SOCK_STREAM,0);if(listen_fd_<0)return false;int one=1;setsockopt(listen_fd_,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=INADDR_ANY;a.sin_port=htons(port_);if(bind(listen_fd_,(sockaddr*)&a,sizeof(a))<0||listen(listen_fd_,8)<0){close(listen_fd_);listen_fd_=-1;return false;}running_=true;worker_=std::thread(&LocalHttpServer::loop,this);printf("[http] dashboard listening on 0.0.0.0:%d\n",port_);return true;}
void LocalHttpServer::stop(){if(!running_.exchange(false))return;shutdown(listen_fd_,SHUT_RDWR);close(listen_fd_);listen_fd_=-1;if(worker_.joinable())worker_.join();}
void LocalHttpServer::updateFrame(const unsigned char* bgr,int width,int height){if(!bgr||width<=0||height<=0)return;std::lock_guard<std::mutex> lock(frame_mutex_);latest_frame_.assign(bgr,bgr+(size_t)width*height*3);frame_width_=width;frame_height_=height;}
void LocalHttpServer::updateRecognition(const std::string& id,const std::string& name,const std::string& time,float confidence,float distance){std::lock_guard<std::mutex> lock(recognition_mutex_);latest_recognition_="{\"employee_id\":\""+jsonEscape(id)+"\",\"name\":\""+jsonEscape(name)+"\",\"time\":\""+jsonEscape(time)+"\",\"confidence\":"+std::to_string(confidence*100.0f)+",\"distance\":"+std::to_string(distance)+"}";recognition_history_.insert(recognition_history_.begin(),latest_recognition_);if(recognition_history_.size()>50)recognition_history_.resize(50);}
void LocalHttpServer::loop(){while(running_){int fd=accept(listen_fd_,nullptr,nullptr);if(fd<0){if(errno==EINTR)continue;break;}std::thread([this,fd]{handleClient(fd);close(fd);}).detach();}}
void LocalHttpServer::handleClient(int fd){
    timeval tv{35,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));std::string req;char buf[4096];size_t header_end=std::string::npos,need=0;
    while(req.size()<12*1024*1024){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<=0)break;req.append(buf,n);if(header_end==std::string::npos&&(header_end=req.find("\r\n\r\n"))!=std::string::npos){std::string cl=headerValue(req.substr(0,header_end),"Content-Length");need=header_end+4+(cl.empty()?0:(size_t)strtoul(cl.c_str(),nullptr,10));}if(header_end!=std::string::npos&&req.size()>=need)break;}
    size_t line_end=req.find("\r\n");if(line_end==std::string::npos){respond(fd,400,"application/json","{\"ok\":false,\"message\":\"bad request\"}");return;}std::istringstream first(req.substr(0,line_end));std::string method,path,version;first>>method>>path>>version;
    if(method=="GET"&&path=="/"){respond(fd,200,"text/html; charset=utf-8",kDashboard);return;}
    if(method=="GET"&&path=="/stream.mjpg"){
        const char* head="HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";if(send(fd,head,strlen(head),MSG_NOSIGNAL)<=0)return;
        while(running_){std::vector<unsigned char> rgb;int w=0,h=0;{std::lock_guard<std::mutex> lock(frame_mutex_);rgb=latest_frame_;w=frame_width_;h=frame_height_;}if(rgb.empty()){usleep(200000);continue;}jpeg_compress_struct c{};jpeg_error_mgr e{};c.err=jpeg_std_error(&e);jpeg_create_compress(&c);unsigned char* out=nullptr;size_t out_size=0;jpeg_mem_dest(&c,&out,&out_size);c.image_width=w;c.image_height=h;c.input_components=3;c.in_color_space=JCS_RGB;jpeg_set_defaults(&c);jpeg_set_quality(&c,70,TRUE);jpeg_start_compress(&c,TRUE);while(c.next_scanline<c.image_height){JSAMPROW row=&rgb[(size_t)c.next_scanline*w*3];jpeg_write_scanlines(&c,&row,1);}jpeg_finish_compress(&c);char ph[160];int pn=snprintf(ph,sizeof(ph),"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",out_size);bool ok=send(fd,ph,pn,MSG_NOSIGNAL)>0&&send(fd,out,out_size,MSG_NOSIGNAL)>0&&send(fd,"\r\n",2,MSG_NOSIGNAL)>0;free(out);jpeg_destroy_compress(&c);if(!ok)break;usleep(200000);}return;}
    if(method=="GET"&&path=="/api/status"){respond(fd,200,"application/json",status_handler_?status_handler_():"{}");return;}
    if(method=="GET"&&path=="/api/latest-recognition"){std::lock_guard<std::mutex> lock(recognition_mutex_);respond(fd,200,"application/json",latest_recognition_);return;}
    if(method=="GET"&&path=="/api/attendance"){std::lock_guard<std::mutex> lock(recognition_mutex_);std::string json="[";for(size_t i=0;i<recognition_history_.size();++i){if(i)json+=',';json+=recognition_history_[i];}respond(fd,200,"application/json",json+"]");return;}
    if(method=="GET"&&path=="/api/employees"){respond(fd,200,"application/json",employees_handler_?employees_handler_():"[]");return;}
    if(method=="DELETE"&&path.find("/api/employees/")==0){std::string id=path.substr(15);std::string decoded;for(size_t i=0;i<id.size();++i){if(id[i]=='%'&&i+2<id.size()){decoded+=(char)strtol(id.substr(i+1,2).c_str(),nullptr,16);i+=2;}else decoded+=id[i];}HttpActionResult r=delete_handler_?delete_handler_(decoded):HttpActionResult{};respond(fd,r.ok?200:404,"application/json","{\"ok\":"+std::string(r.ok?"true":"false")+",\"message\":\""+jsonEscape(r.message)+"\"}");return;}
    if(method=="POST"&&path=="/api/employees"){std::string ct=headerValue(req.substr(0,header_end),"Content-Type");size_t bp=ct.find("boundary=");if(bp==std::string::npos){respond(fd,400,"application/json","{\"ok\":false,\"message\":\"multipart/form-data required\"}");return;}auto parts=parseMultipart(req.substr(header_end+4),ct.substr(bp+9));HttpRegisterRequest q;q.employee_id=parts["employee_id"].value;q.name=parts["name"].value;std::vector<std::string> temps;if(!parts["face"].value.empty()&&writeTemp(parts["face"].value,&q.image_path))temps.push_back(q.image_path);if(!parts["audio"].value.empty()&&writeTemp(parts["audio"].value,&q.audio_path))temps.push_back(q.audio_path);HttpActionResult r=register_handler_?register_handler_(q):HttpActionResult{};for(const auto& f:temps)unlink(f.c_str());respond(fd,r.ok?201:400,"application/json","{\"ok\":"+std::string(r.ok?"true":"false")+",\"message\":\""+jsonEscape(r.message)+"\"}");return;}
    respond(fd,404,"application/json","{\"ok\":false,\"message\":\"not found\"}");
}
