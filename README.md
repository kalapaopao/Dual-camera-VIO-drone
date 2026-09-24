# WSL2 Docker 双目 USB 摄像头开发环境

完整的项目交接说明见：[docs/交接文档.md](docs/交接文档.md)。GitHub 版本源是本地仓库；WSL 目录只是 Docker 运行副本。

这个项目提供一个不依赖 YOLO、PyTorch、CUDA、RKNN、TensorRT 或 OpenCV 的双目 USB 摄像头开发环境。

```text
USB 双目摄像头 -> Windows usbipd -> WSL2 Ubuntu -> Docker -> V4L2/GStreamer/C++
```

## 镜像、容器和挂载

- 镜像：`uav_camera_dev:latest`
- 容器：`uav_camera_dev`
- 设备：`/dev/video0`、`/dev/video1`
- 工作区挂载：WSL 项目的 `workspace` -> 容器 `/workspace`
- HTTP 端口：容器 `8080` -> Windows/WSL `8080`，备用 `8081`
- 基础环境：Ubuntu 22.04、V4L2、GStreamer、Python 3、C/C++ 编译工具

USB 设备由 `usbipd attach --wsl` 先透传到 WSL。Docker 不直接挂载 Windows USB 总线，而是把 WSL 中生成的 V4L2 设备节点映射到容器。

## 傻瓜式启动

在 Windows PowerShell 执行：

```powershell
usbipd list
usbipd bind --busid 1-6
usbipd attach --wsl --busid 1-6
```

当前摄像头是 `0bda:5883 USB Camera`，当前 BUSID 是 `1-6`。重新插拔后 BUSID 可能变化，以 `usbipd list` 为准。不要附加 `1-3`，那是 Windows 集成摄像头。

然后在 WSL 执行：

```bash
cd /home/kaka/tl3576_camera_driver/uav_vision_docker
ls -l /dev/video*
docker exec -it uav_camera_dev bash
```

首次没有镜像时：

```bash
docker compose build
docker compose up -d --force-recreate
```

### 一条命令完成设备、目录和端口挂载

本项目推荐使用 Compose，因为设备和挂载已经写入 `docker-compose.yml`：

```bash
cd /home/kaka/tl3576_camera_driver/uav_vision_docker
docker compose up -d --force-recreate
```

这条命令等价于同时完成：

```text
/dev/video0 -> 容器 /dev/video0
/dev/video1 -> 容器 /dev/video1
WSL workspace -> 容器 /workspace
宿主机 8080 -> 容器 8080
宿主机 8081 -> 容器 8081
```

如果不使用 Compose，也可以直接执行完整的 `docker run` 命令：

```bash
docker rm -f uav_camera_dev 2>/dev/null || true
docker run -d \
  --name uav_camera_dev \
  --privileged \
  --device=/dev/video0:/dev/video0 \
  --device=/dev/video1:/dev/video1 \
  --mount type=bind,src=/home/kaka/tl3576_camera_driver/uav_vision_docker/workspace,dst=/workspace \
  --publish 8080:8080 \
  --publish 8081:8081 \
  uav_camera_dev:latest \
  /bin/bash
```

`--device` 挂载摄像头，`--mount` 挂载源码工作区，`--publish` 暴露 Windows 浏览器访问的端口。使用 `docker run` 后，不要再同时使用 Compose 创建同名容器。

容器内检查：

```bash
ls -l /dev/video*
v4l2-ctl --list-devices
v4l2-ctl --list-formats-ext
```

摄像头提供左右画面横向拼接的帧，例如 `1280x480`、`1920x540`、`2560x960`。

## Compose 中的关键映射

```yaml
privileged: true
devices:
  - /dev/video0:/dev/video0
  - /dev/video1:/dev/video1
volumes:
  - ./workspace:/workspace
ports:
  - "8080:8080"
  - "8081:8081"
```

`/dev/media0` 是 media-controller 节点，目前的 V4L2/GStreamer 采集不需要单独映射。

## C++ 双目 MJPEG HTTP 推流

源码：`stereo_mjpeg_server.cpp`。

程序使用 V4L2 MMAP 直接读取 MJPEG，不依赖 OpenCV。默认读取 `/dev/video0` 的 `1280x480@60` 双目拼接帧，并在 8080 端口提供 HTTP MJPEG 流。Windows 浏览器可以直接显示。

源码放在本地工作区后，同步到 WSL Docker 挂载目录：

```bash
cp /mnt/e/rk3576/camera/stereo_mjpeg_server.cpp \
  /home/kaka/tl3576_camera_driver/uav_vision_docker/workspace/
```

如果本地工作区不是 `/mnt/e/rk3576/camera`，将源路径替换成实际路径。也可以直接在 WSL 项目的 `workspace` 中放置同名文件。

容器内编译并运行：

```bash
cd /workspace
g++ -std=c++17 -O2 -Wall -Wextra stereo_mjpeg_server.cpp -o stereo_mjpeg_server
./stereo_mjpeg_server /dev/video0 1280 480 60 8080
```

Windows 浏览器打开：

```text
http://localhost:8080/
```

如果 `localhost` 无法访问，在 WSL 执行 `hostname -I`，然后访问 `http://<WSL_IP>:8080/`。

也可以从第二个节点启动另一条流：

```bash
./stereo_mjpeg_server /dev/video1 1280 480 60 8081
```

## GStreamer 无窗口测试

WSL 通常没有 Linux GUI，使用 `fakesink` 可以验证实际取帧：

```bash
gst-launch-1.0 \
  v4l2src device=/dev/video0 num-buffers=30 \
  ! image/jpeg,width=1280,height=480,framerate=60/1 \
  ! jpegdec \
  ! videoconvert \
  ! fakesink sync=false
```

## 停止、启动和设备重连

```bash
docker compose stop
docker compose start
docker compose down
```

摄像头重新连接或设备节点变化后，需要重新创建容器：

```bash
docker compose up -d --force-recreate
```

## 常见问题

### `/dev/video0` 不存在

```powershell
usbipd list
usbipd attach --wsl --busid 1-6
```

```bash
ls -l /dev/video*
docker compose up -d --force-recreate
```

### 容器里没有新设备

摄像头如果是在容器创建之后才附加到 WSL，旧容器不会自动获得设备，执行：

```bash
docker compose up -d --force-recreate
```

### Docker 构建下载超时

这是 Docker Hub 或 Ubuntu 软件源网络问题，可以重试：

```bash
docker pull ubuntu:22.04
docker compose build
```

网络长期很慢时，应配置 Docker Desktop 镜像源或代理。









容器的长期保存


cd /home/kaka/tl3576_camera_driver/uav_vision_docker
  docker compose start

  如果摄像头也要恢复，还需要重新执行：

  usbipd attach --wsl --busid 1-6

  然后重新创建容器：

  docker compose up -d --force-recreate

  docker compose stop 不会删除容器，也不会删除容器里的内容：

  docker compose stop
  docker compose start

  这样重新启动的是同一个容器，容器内已安装的环境和临时文件都会保留。

  但下面这个命令会删除容器：

  docker compose down

  它会删除容器和 Compose 网络。镜像通常仍然保留，但如果你在容器内部临时安装了软件、修改了没有挂载出来的文件，这些内容会
  丢失。

  当前项目的代码目录是挂载的：

  WSL:
  /home/kaka/tl3576_camera_driver/uav_vision_docker/workspace

  容器:
  /workspace

  因此 /workspace 中的代码不会因为容器删除而消失。

  容器内通过命令临时安装的内容，例如：

  apt install xxx
  pip install xxx

  如果没有写进 Dockerfile，那么执行 docker compose down 后重新创建容器时可能会消失。正式环境应当把依赖写入 Dockerfile，
  然后重新构建镜像：

  docker compose build
  docker compose up -d

  容器不会无缘无故自己消失。通常只有这些情况会导致容器被删除：

  docker compose down
  docker rm -f uav_camera_dev
  docker system prune
  docker compose up -d --force-recreate

  其中：

  docker compose up -d --force-recreate

  会重新创建容器，因此没有挂载到宿主机的容器内部修改会丢失，但镜像和 /workspace 源码不会丢失。

  如果希望 Docker 启动后自动启动容器，可以在 docker-compose.yml 中加入：

  restart: unless-stopped

  例如：

  services:
    uav_camera:
      image: uav_camera_dev:latest
      container_name: uav_camera_dev
      restart: unless-stopped
      privileged: true
      devices:
        - /dev/video0:/dev/video0
        - /dev/video1:/dev/video1
      volumes:
        - ./workspace:/workspace
      ports:
        - "8080:8080"
        - "8081:8081"

  这样电脑重启、Docker Desktop 重启后，容器会自动启动。但 USB 摄像头仍然需要重新执行：

  usbipd attach --wsl --busid 1-6

  我建议平时使用：

  docker compose stop
  docker compose start

  不要把重要代码只放在容器内部，代码都放在 /workspace；不要只在运行中的容器里安装依赖，依赖写入 Dockerfile。
