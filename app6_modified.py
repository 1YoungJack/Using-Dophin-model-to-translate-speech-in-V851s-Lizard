import os
import uvicorn
import numpy as np
import torch
import re
import asyncio
from fastapi import FastAPI, UploadFile, File, Form, HTTPException, WebSocket
from fastapi.responses import HTMLResponse, FileResponse
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
import dolphin
from dolphin.constants import SPEECH_LENGTH, SAMPLE_RATE
import logging
import json
from html_content import html_content, languages

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler("app5.log"),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# 加载语音识别模型
model = dolphin.load_model("small", "/mnt/hdsd1/syf/dolphin/dolphin_model", "cuda")

app5 = FastAPI()

# 支持多并发和CORS配置
app5.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# 创建静态目录和HTML文件
if not os.path.exists("static"):
    os.makedirs("static")

# 将HTML内容写入文件
with open("static/index.html", "w", encoding="utf-8") as f:
    f.write(html_content)

# 挂载静态文件目录
app5.mount("/static", StaticFiles(directory="static"), name="static")

# 长音频转录函数
def long_transcribe(waveform, lang_sym, region_sym):
    speech_length = int(SAMPLE_RATE * SPEECH_LENGTH)
    result_long = ""
    speech = waveform
    # Prepare speech
    if isinstance(speech, np.ndarray):
        speech = torch.tensor(speech)

    # Only support single-channel speech
    if speech.dim() > 1:
        assert (speech.dim() == 2 and speech.size(1) == 1), f"speech of size {speech.size()} is not supported"
        speech = speech.squeeze(1)  # (nsamples, 1) --> (nsamples,)
    while speech.size(-1) >= speech_length:
        if lang_sym == "":
            result_segment = model(speech).text
        else:
            result_segment = model(speech, lang_sym=lang_sym, region_sym=region_sym).text
        matches = re.findall(r'<([^<>]*)>', result_segment)
        if len(matches) >= 2:
            value_str = matches[-2].strip()
        length = float(value_str)
        if length <= 1:
            length = 30
            result_long += result_segment
        else:
            end_index = result_segment.rfind(matches[-2]) + len(matches[-2]) + 2
            result_long += result_segment[:end_index]
        
        speech = speech[int(SAMPLE_RATE * length):]
    result_long += model(speech, lang_sym=lang_sym, region_sym=region_sym).text
    return result_long

# 单个音频转录函数
def transcribe_audio(audio_file, language_option):
    lang_sym, region_sym = languages[language_option]
    if audio_file is not None:
        # 保存临时文件
        temp_path = f"temp_{audio_file.filename}"
        with open(temp_path, "wb") as f:
            f.write(audio_file.file.read())
            
        waveform = dolphin.load_audio(temp_path)
        
        # 删除临时文件
        os.remove(temp_path)
        
        if len(waveform) < 160:
            return "音频太短！"
        else:
            return long_transcribe(waveform, lang_sym, region_sym)
    else:
        return "请上传音频文件！"

# 批量音频转录函数
def batch_transcribe_audio(audio_files, language_option):
    results = {}
    for audio_file in audio_files:
        result = transcribe_audio(audio_file, language_option)
        results[audio_file.filename] = result
    return results

# 保存结果为文本文件
def save_results_as_text(results):
    output_path = "recognition_results.txt"
    with open(output_path, "w", encoding="utf-8") as f:
        for filename, result in results.items():
            f.write(f"{filename}:\n{result}\n\n")
    return output_path


# =============== 改进实时识别功能 ===============
class AudioProcessor:
    def __init__(self, sample_rate=SAMPLE_RATE):
        self.sample_rate = sample_rate
        self.buffer = np.array([], dtype=np.float32)
        self.last_process_time = 0
        self.min_audio_length = 0.3  # 最小识别音频长度(秒)
    
    def reset(self):
        """重置处理器状态"""
        self.buffer = np.array([], dtype=np.float32)
        self.last_process_time = 0
    
    def add_audio(self, audio_data):
        """添加音频数据到缓冲区"""
        # 将字节数据转换为16位整数，然后转换为浮点数并归一化
        audio_array = np.frombuffer(audio_data, dtype=np.int16)
        audio_float = audio_array.astype(np.float32) / 32768.0
        self.buffer = np.concatenate([self.buffer, audio_float])
    
    def get_audio_for_recognition(self, min_duration=1.0):
        """
        获取足够时长的音频进行识别
        返回: (audio_chunk, has_more)
        """
        # 计算缓冲区中的音频时长
        buffer_duration = len(self.buffer) / self.sample_rate
        
        # 如果缓冲区时长不足最小识别长度，返回None
        if buffer_duration < self.min_audio_length:
            return None, False
        
        # 计算需要提取的样本数
        samples_needed = min(
            int(min_duration * self.sample_rate), 
            len(self.buffer)
        )
        
        # 提取音频片段
        audio_chunk = self.buffer[:samples_needed]
        
        # 更新缓冲区
        self.buffer = self.buffer[samples_needed:]
        
        # 检查是否还有剩余数据
        has_more = len(self.buffer) > 0
        
        return audio_chunk, has_more
    
    def get_all_audio(self):
        """获取缓冲区中的所有音频"""
        if len(self.buffer) == 0:
            return None
        audio = self.buffer.copy()
        self.reset()
        return audio

@app5.websocket("/ws/realtime")
async def websocket_realtime(websocket: WebSocket):
    await websocket.accept()
    logger.info("WebSocket连接已建立")
    
    # 创建音频处理器
    audio_processor = AudioProcessor()
    
    try:
        while True:
            # 接收数据
            data = await websocket.receive()
            
            if data["type"] == "websocket.receive":
                # 处理二进制数据（音频）
                if "bytes" in data:
                    audio_processor.add_audio(data["bytes"])
                    # 立即推理并返回结果
                    audio_chunk = audio_processor.get_all_audio()  # 0表示全部处理
                    if audio_chunk is not None and len(audio_chunk) > 0:
                        transcript = model(torch.tensor(audio_chunk)).text
                        await websocket.send_json({"transcript": transcript})
                
                # 处理文本消息（控制命令）
                elif "text" in data:
                    message = json.loads(data["text"])
                    
                    if message.get("action") == "reset":
                        # 重置处理器
                        audio_processor.reset()
                        await websocket.send_json({"status": "reset"})
                    
                    elif message.get("action") == "stop":
                        # 停止录音，处理剩余音频
                        remaining_audio = audio_processor.get_all_audio()
                        if remaining_audio is not None and len(remaining_audio) > 0:
                            transcript = model(torch.tensor(remaining_audio)).text
                            await websocket.send_json({"transcript": transcript})
                        await websocket.send_json({"action": "stopped"})
                        break
    
    except Exception as e:
        logger.error(f"WebSocket错误: {str(e)}")
    finally:
        logger.info("WebSocket连接已关闭")
        try:
            await websocket.close()
        except:
            pass

# 主页面路由
@app5.get("/", response_class=HTMLResponse)
async def read_index():
    with open("static/index.html", "r", encoding="utf-8") as f:
        return f.read()

# 批量识别API
@app5.post("/batch_transcribe")
async def batch_transcribe_endpoint(
    language: str = Form(...),
    files: list[UploadFile] = File(...)
):
    try:
        # 检查语言选项是否有效
        if language not in languages:
            raise HTTPException(status_code=400, detail="无效的语言选项")
        
        # 处理上传的文件
        results = batch_transcribe_audio(files, language)
        
        # 保存结果到文件
        output_path = save_results_as_text(results)
        
        # 返回结果和下载链接
        return {
            "status": "success",
            "results": results,
            "download_link": f"/download/{os.path.basename(output_path)}"
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

# 文件下载路由
@app5.get("/download/{filename}")
async def download_file(filename: str):
    if not os.path.exists(filename):
        raise HTTPException(status_code=404, detail="文件未找到")
    return FileResponse(filename, filename=filename)

if __name__ == "__main__":
    uvicorn.run(
        app5,
        host="192.168.0.202",
        port=9877,
        ssl_certfile='/mnt/hdsd1/syf/dolphin/ssl/nginx.crt',
        ssl_keyfile="/mnt/hdsd1/syf/dolphin/ssl/nginx.key"
    )