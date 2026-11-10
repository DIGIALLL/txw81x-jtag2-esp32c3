# Камера с OpenTXW81X в Home Assistant

После прошивки [OpenTXW81X](https://github.com/openshwprojects/OpenTXW81X) камера отдаёт:

- веб-интерфейс OpenBK: `http://<camera-ip>/`;
- RTSP: `rtsp://<camera-ip>:7070/webcam` — **MJPEG 640×480, только UDP**
  (TCP-интерливинг не работает);
- лог: TCP-порт 9000.

Видео поднимается после перезагрузки, если в LFS лежит `autoexec.bat` со строкой:

```
startDriver TXWCAM
```

## Почему просто Generic Camera не показывает видео в браузере

Если указать камеру в Generic Camera напрямую, HA принимает поток (снимки через API работают),
но в карточке видео не идёт. HA пересобирает поток в HLS/WebRTC **без перекодирования**,
а MJPEG внутри MP4 (`CODECS="mp4v"`) браузеры не воспроизводят.

## Рабочая схема: камера → go2rtc (H.264) → Generic Camera

1. В `go2rtc.yaml` (аддон go2rtc или встроенный) добавить поток:

   ```yaml
   streams:
     txw817:
       - ffmpeg:rtsp://<camera-ip>:7070/webcam#input=rtsp/udp#video=h264
   ```

   `#input=rtsp/udp` нужен потому, что камера не умеет RTSP поверх TCP.
   Перекодирование запускается, только пока кто-то смотрит поток.

2. Добавить интеграцию **Generic Camera**:
   - Stream source: `rtsp://<go2rtc-host>:8554/txw817`
   - Still image URL: `http://<go2rtc-host>:1984/api/frame.jpeg?src=txw817`
   - RTSP transport: TCP

   Если при сохранении HA пишет `timeout`, поток ещё не успел запуститься (холодный старт
   ffmpeg занимает несколько секунд). Откройте его в go2rtc или VLC и сохраните ещё раз.

3. Закрепите IP камеры за её MAC в DHCP роутера.

Результат: живое видео через WebRTC в карточке HA, около 7–8 кадров/с при 640×480.
