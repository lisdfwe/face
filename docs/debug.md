# 调试时的指令

## 1.查看usb速率
```
lsusb -t
```
结果是否正常
- 480M   → USB High-Speed（正常）
- 12M    → USB Full-Speed（带宽不足）
## 2.查看摄像头的完整信息
```
v4l2-ctl -d /dev/video1 --all
```
## 3.使用v4l2-ctl直接抓帧测试
```
v4l2-ctl -d /dev/video1 --stream-mmap --stream-count=100
```
## 4.内核日志排查
- 查看uvc相关的日志
```
dmesg | grep -i uvc
```
- 查看usb相关的日志
```
dmesg | grep -i usb
```
## 5.内存的排查
```
cat /proc/meminfo | grep -i cma
```
## 6.cpu查询
- 查看cpu频率
```
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```
- 设置cpu模式
```
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```
## 7.查看lcd屏幕的命令
- 查看framebuff 还是DRM
```
ls /dev/fb*
```
- 查看lcd的分辨率
```
fbset -i
```

## 8.查看板子配置的信息
- 查看内存
```
cat /proc/meminfo
```

