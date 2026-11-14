# 1.2.1 整合交接(2026-11-14)

## 结果
- 分支 `integrate/1.2.1`,HEAD `45e990c`(merge 89f4aea 上游 1.2.1 + minimal/lying-vbv)。
- 构建 0 error 0 warning;core-tests T=0(5 行 queue guard 全出);bench 摆动峰值 141.4 Mbps(150 设定内,无突发);二进制指纹 4 项全在(guard dip / guard at the floor / datagrams refused / poseWait)。

## 冲突怎么判的(3 文件 7 块)
1. `video_sender.h` / `video_sender.cpp`:整体取上游 1.2.1 架构(直接 Send、无 worker、无槽),
   丢我们的 Submit/槽/worker;OnFeedback 整段保留我方(守卫接线 + 动态 FEC + floor 日志)。
   pacer 行由上游 `m_targetKbps*1.5` 改为 `TargetBitrateKbps()*1.08`(守卫后的目标,8% 余量)。
   `queue_guard.h` 与分支逐字节一致(blob 76ebbd0)。
2. `vr_stream.cpp` B1 Stop:取上游(job/send mutex 唤醒),删 m_sender.Stop()。
3. `vr_stream.cpp` B2:取上游作业结构(EncodeJob 填充 + RunEncoder 头),但 **×2 按计划书 §3 判据判为"不加"**:
   - 作业交接是**有界槽**(RunEncoder 等上一帧 SendJob 被取走)→ 满足"保留删除"判据;
   - 更根本:nvenc 的慢 EMA 已把 frameRateNum 重配成实际帧率,半帧率本身就把每帧预算翻倍,×2 是双倍补偿
     (实测 17.5→35 Mb/s 对 26 Mb/s pacer,帧率锁 29-33)。结论已写进 RunEncoder 注释。
4. `vr_stream.cpp` B3:取上游(job.motion 在采集线程读、frameMeta.insert + SendJob → RunSender)。
5. 自动合并埋雷两处已排:`desktop_stream.cpp` 的 `m_sender.Stop()` 与 `Submit` 调用改回上游
   `Send`(void);桌面流保留它自己的 ×2(桌面编码器不吃双 EMA,×2 在那是对的)。
   `tools/encode-bench/main.cpp` 的 Submit/Stop 同步改 Send(语义:铺发在调用线程内完成)。
6. 附带修复:nvenc_encoder.cpp:362 三元式 C4244 ×2(`EncoderConfig::fps` 本是 float,两个分支都显式
   uint32_t)——非合并引入,顺手清零。

## 未完成 / 给真机的注意
- **APK 必须同版本**(协议加 vr.poseWait):见 dist 包与 apk 产物记录;装前 `unzip -l | grep libopenxr` 验加载器。
- 凹点编码:上游 1.2.1 新实现(镜心大方块+外围半分辨率)默认值沿用上游;**未恢复 UI 开关**
  (计划书 §10.2 建议恢复,涉及 UI 改动,留给下一步或用户裁决)。
- 真机验收项:视角/手柄跳变、显示刷新率匹配(头显每 5s 日志)、重地图低帧率表现。
- 硬性遵守:未推任何远端、未开 PR;所有产物在本地。
