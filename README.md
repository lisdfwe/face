# i.MX6 人脸识别智能安防系统

## 1. 项目简介
本项目基于 i.MX6Pro Linux 平台，实现摄像头视频采集、
人脸检测与识别，并用于智能安防场景。

## 2. 系统架构
Driver -> Media -> AI -> App -> UI

## 3. 环境要求
- Linux (i.MX6Pro)
- 摄像头 (/dev/video0)
- 屏幕 (/dev/fb0)

## 4. 编译
```bash
make
