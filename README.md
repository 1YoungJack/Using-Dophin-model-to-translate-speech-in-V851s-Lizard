# Using-Dophin-model-to-translate-speech-in-V851s-Lizard
## 仓库结构
.  
├── alsa  
├── app6_modified.py  
├── cJSON.c  
├── cJSON.h  
├── init_wifi.sh  
├── libwebsockets  
├── Makefile  
├── openssl  
├── README.md  
├── set_rec_play.sh  
├── sherpa-onnx  
├── silero_vad.onnx  
├── vad_capture  
└── vad_capture.c  
* ./alsa：armhf架构的`alsa`库
* ./libwebsockets：armhf架构的`libwebsockets`库
* ./openssl：armhf架构的`openssl`库
* ./sherpa-onnx：armhf架构的`sherpa-onnx`库
* cJSON.h、cJSON.c：`cJSON`库源码
* init_wifi.sh：V851s Lizard连接wifi脚本
* set_rec_wifi.sh：V851s Lizard初始化音频设备脚本
* silero_vad.onnx：silero_vad模型文件
* vad_capture.c：客户端源代码
* app_modified.py：服务端源代码
* Makefile：编译客户端源代码make脚本

---

## 使用指南
1. 执行`make`编译客户端源代码，生成可执行文件`vad_capture`
2. 利用adb传输`vad_capture` `silero_vad.onnx` `init_wifi.sh` `set_rec_play.sh`至`/root`目录，传输`alsa/lib/` `libwebsockets/lib/` `sherpa-onnx/lib/`内的库文件至`/usr/lib`
3. 修改或新建`/etc/wpa_supplicant.conf`,`ssid`为wifi名，`psk`为wifi密码
4. 进入`/root`目录
5. 执行`./init_wifi.sh`以设置时间和连接wifi
6. 执行`./set_rec_play.sh`以初始化音频设备
7. 执行`./vad_capture -th 0.2`以运行客户端程序，-th用于设置vad阈值

---

## 注意事项
1. 本项目使用的V851s Lizard开发板修改了PCB，添加了连接音频设备的引脚
2. 笔者使用的交叉编译工具链为`arm-openwrt-linux-muslgnueabi`，gcc版本为8.3.0，若使用不同版本工具链，请重新交叉编译第三方库
3. 交叉编译`libwebsockets`时，请先编译`openssl`库；交叉编译`sherpa-onnx`库时，请先编译`onnxruntime`库并替换初始化`sherpa-onnx`编译环境后的`libonnxruntime.so`

---

## 参考链接
1. `libwebsockets`交叉编译指南：[https://blog.csdn.net/weixin_45783574/article/details/145809844](https://blog.csdn.net/weixin_45783574/article/details/145809844)
2. `onnxruntime`交叉编译指南：[https://www.cnblogs.com/sheephuan/p/17744411.html](https://www.cnblogs.com/sheephuan/p/17744411.html)
3. V851s Lizard连接wifi：[https://dongshanpi.com/YuzukiHD-Lizard/12-1_TinaSDK_SupportWifiBluetooth/](https://dongshanpi.com/YuzukiHD-Lizard/12-1_TinaSDK_SupportWifiBluetooth/)