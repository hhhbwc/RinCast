# HANDOFF — 发送链路核实与 150 Mbps(执行完毕)

> 执行:主 AI(本会话,2026-11-13)。分支 `minimal/lying-vbv`,工作目录 `C:/tmp/rincast-stock`。
> 上一执行者(另一 AI)额度耗尽,其未提交实验状态已按逻辑分组提交(e61c059/001d995/a2ee83d/f84be59)。

## 已核实的事实(引用代码,全部亲验)

1. **fps 反馈链路**(86b2971/68d5378 + 本轮补丁):`TrackFrameInterval()`(`nvenc_encoder.cpp:375`起)
   每 Encode 采样帧间隔,EMA 0.4/0.6 快速平滑;重配置门控 100ms;相对死区 ±1/12(锚 m_effectiveFps);
   启动宽限期内 VBV 钳到标称大小(修了实测的 0→15→96 震荡 → 101.7Mbps 顶爆链路)。
2. **启动宽限之外的残余风险(本轮发现并修复)**:一个 800ms 的停顿样本(加载/卡顿)经 0.4/0.6
   快速平滑把跟踪率砸到 15fps 下限 → VBV 涨到 1.5×(150/15)=15Mbit → 游戏恢复 90fps 后每帧预算
   放大六倍。修复:单个样本钳到平滑值 ×3(`nvenc_encoder.cpp` TrackFrameInterval)——持续变慢仍
   收敛(上限随平滑自适应),一次性间隙动不了它。
3. **重配置失败也会记账**(本轮发现并修复):m_effectiveFps/m_init 在 nvEncReconfigureEncoder 之前
   赋值,失败后判重跳过重试、VBV 口径脱节。改为成功分支内记账。
4. **pacer 重写**(另一 AI 未提交工作,本轮验证正确):可取消高精度等待定时器 + 停止事件、
   欠账超过 600us 丢弃(停顿后不补发成突发)、发送时钟跨帧累加、空闲 50ms 重置。
   实测唤醒粒度 ~1ms → paced 批 ≈ 1ms × pacing 速率。
5. **shared/ 源码编码**:中文注释让 MSVC(无 /utf-8 时按 GBK)读碎预处理结构(C1020 意外 #endif、
   字符串常量断行)。已全部转 ASCII;pc/CMakeLists 给 MSVC 目标统一 /utf-8;bench 输出改无缓冲
   (崩溃保留临终输出——调试时被缓冲吞过一次)。

## 离线测量(encode-bench,4352×2176@90,150 Mbps,合成内容,640 以下不需要 GPU 之外的任何东西)

| 场景 | 结果 |
|---|---|
| 稳态 600 帧 | window 152.7-153.6 Mbps,90 fps,峰值 153.6/150(+2.4%),encode avg 7.06ms |
| 负载摆动 900 帧(--swing) | fps 33-42,帧 417KB 均值/1002KB 峰值,**慢帧仍承载 120-149 Mbps**(旧行为=设定的 1/3),峰值 159.3/150(+6.2%) |
| 旧代码同场景 | 174 Mbps against 100(+74%,另一 AI 实测) |

## 测试状态

- `rincast-core-tests`:连续 6 次 exit 0(修了两个对调度敏感的断言:批中位数阈值 32→64KB 容忍
  两批合并;删除了错误的"unpaced 间隙比 paced 小 3 倍"断言——paced 批内有大量小间隙,该比不成立)。
- **教训:本 worktree 的测试 exe 在 `build/bin/Release/`(没有 pc/),别跑 `build/pc/bin/` 下的旧货;
  cmd 单行里 %ERRORLEVEL% 是解析期展开,必须 //v:on //c 用 !ERRORLEVEL!。**

## 未做 / 如何继续

1. 真机验证(只有用户能做):装 minimal/lying-vbv 的包,2600 档 + 150 Mbps 实测;
   看日志三行对齐(`NVENC ... encoder rate N fps` / `NVENC: X fps measured` / `Stream N throughput`)。
2. Paper-gaze 半成品在主工作树 `wip/paper-gaze` 分支(与本项目无关,别混)。
3. 审计遗留 10 项见主交接文档 §1.9.2(与本项目正交)。

## 提交清单(minimal/lying-vbv,本轮)

- e61c059 LAN pacing + 最新姿态(drawing 自上一执行者未提交工作)
- 001d995 可取消高精度 pacer + 编码器码率跟踪(同上)
- a2ee83d pacing 测试 + sender bench + /utf-8
- f84be59 desktop 流关停顺序
- cff350a 停顿钳制 + 成功后才记账 + shared 全 ASCII + bench 无缓冲
- (最新)tests: pacing batch 阈值容忍调度抖动
