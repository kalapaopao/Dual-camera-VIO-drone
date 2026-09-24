#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/videodev2.h>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }
int xioctl(int fd, unsigned long request, void* arg) {
    int result;
    do { result = ioctl(fd, request, arg); } while (result == -1 && errno == EINTR);
    return result;
}
struct MappedBuffer { void* start = nullptr; size_t length = 0; };

class Camera {
public:
    Camera(const std::string& device, unsigned width, unsigned height, unsigned fps)
        : device_(device), width_(width), height_(height), fps_(fps) {}
    ~Camera() { close_camera(); }

    bool open_camera() {
        fd_ = open(device_.c_str(), O_RDWR | O_NONBLOCK);
        if (fd_ < 0) { std::cerr << "open " << device_ << ": " << std::strerror(errno) << '\n'; return false; }
        v4l2_capability cap{};
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0 || !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
            std::cerr << device_ << " is not a streaming V4L2 capture device\n"; return false;
        }
        v4l2_format format{};
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width_; format.fmt.pix.height = height_;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG; format.fmt.pix.field = V4L2_FIELD_ANY;
        if (xioctl(fd_, VIDIOC_S_FMT, &format) < 0) { std::cerr << "VIDIOC_S_FMT: " << std::strerror(errno) << '\n'; return false; }
        width_ = format.fmt.pix.width; height_ = format.fmt.pix.height;
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) { std::cerr << "Camera did not accept MJPEG format\n"; return false; }
        v4l2_streamparm stream{};
        stream.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        stream.parm.capture.timeperframe.numerator = 1; stream.parm.capture.timeperframe.denominator = fps_;
        xioctl(fd_, VIDIOC_S_PARM, &stream);
        v4l2_requestbuffers request{};
        request.count = 4; request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; request.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) { std::cerr << "VIDIOC_REQBUFS: " << std::strerror(errno) << '\n'; return false; }
        buffers_.resize(request.count);
        for (unsigned i = 0; i < request.count; ++i) {
            v4l2_buffer buffer{}; buffer.type = request.type; buffer.memory = request.memory; buffer.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) return false;
            buffers_[i].length = buffer.length;
            buffers_[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buffer.m.offset);
            if (buffers_[i].start == MAP_FAILED) { buffers_[i].start = nullptr; return false; }
            if (xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) return false;
        }
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return false;
        streaming_ = true;
        std::cerr << "Capturing " << device_ << " at " << width_ << "x" << height_ << " MJPEG\n";
        return true;
    }

    bool read_frame(std::vector<unsigned char>& frame) {
        pollfd descriptor{fd_, POLLIN, 0};
        int ready = poll(&descriptor, 1, 1000);
        if (ready <= 0) return ready == 0;
        v4l2_buffer buffer{}; buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) return true;
            std::cerr << "VIDIOC_DQBUF: " << std::strerror(errno) << '\n'; return false;
        }
        frame.assign(static_cast<unsigned char*>(buffers_[buffer.index].start), static_cast<unsigned char*>(buffers_[buffer.index].start) + buffer.bytesused);
        return xioctl(fd_, VIDIOC_QBUF, &buffer) == 0;
    }

private:
    void close_camera() {
        if (streaming_) { v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; xioctl(fd_, VIDIOC_STREAMOFF, &type); }
        for (auto& buffer : buffers_) if (buffer.start) munmap(buffer.start, buffer.length);
        if (fd_ >= 0) close(fd_);
    }
    std::string device_; unsigned width_, height_, fps_; int fd_ = -1; bool streaming_ = false; std::vector<MappedBuffer> buffers_;
};

bool send_all(int fd, const void* data, size_t size) {
    const char* bytes = static_cast<const char*>(data);
    while (size) { ssize_t sent = send(fd, bytes, size, MSG_NOSIGNAL); if (sent <= 0) return false; bytes += sent; size -= static_cast<size_t>(sent); }
    return true;
}

void stream_client(int client, Camera& camera) {
    const char* header = "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (!send_all(client, header, std::strlen(header))) { close(client); return; }
    std::vector<unsigned char> frame;
    while (g_running && camera.read_frame(frame)) {
        if (frame.empty()) continue;
        std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(frame.size()) + "\r\n\r\n";
        if (!send_all(client, part.data(), part.size()) || !send_all(client, frame.data(), frame.size()) || !send_all(client, "\r\n", 2)) break;
    }
    close(client);
}

int make_server(unsigned short port) {
    int server = socket(AF_INET, SOCK_STREAM, 0); if (server < 0) return -1;
    int reuse = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(port);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server, 4) < 0) { std::cerr << "HTTP server: " << std::strerror(errno) << '\n'; close(server); return -1; }
    return server;
}
}  // namespace

int main(int argc, char** argv) {
    std::string device = argc > 1 ? argv[1] : "/dev/video0";
    unsigned width = argc > 2 ? std::stoul(argv[2]) : 1280;
    unsigned height = argc > 3 ? std::stoul(argv[3]) : 480;
    unsigned fps = argc > 4 ? std::stoul(argv[4]) : 60;
    unsigned short port = argc > 5 ? static_cast<unsigned short>(std::stoul(argv[5])) : 8080;
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
    Camera camera(device, width, height, fps); if (!camera.open_camera()) return 1;
    int server = make_server(port); if (server < 0) return 1;
    std::cerr << "Open http://localhost:" << port << "/ in Windows\n";
    while (g_running) {
        sockaddr_in client_address{}; socklen_t client_length = sizeof(client_address);
        int client = accept(server, reinterpret_cast<sockaddr*>(&client_address), &client_length);
        if (client < 0) { if (errno == EINTR) continue; break; }
        stream_client(client, camera);
    }
    close(server); return 0;
}
