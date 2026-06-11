// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 三水深
package com.salt.usb.dacplayer

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import com.salt.usb.audio.UsbAudioManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlin.concurrent.thread

/**
 * Foreground service that "hijacks" the system media audio via
 * [AudioPlaybackCaptureConfiguration] (MediaProjection) and streams the captured
 * PCM straight to a USB DAC through [UsbAudioManager] (libusb isochronous OUT),
 * bypassing Android's normal output path for that copy of the audio.
 *
 * Pipeline:  other apps' media  →  AudioRecord (playback capture)  →  ring buffer
 *            →  native libusb isochronous OUT  →  USB DAC ("小尾巴").
 */
class AudioCaptureService : Service() {

    companion object {
        private const val TAG = "AudioCaptureService"

        const val ACTION_START = "com.salt.usb.dacplayer.START"
        const val ACTION_STOP = "com.salt.usb.dacplayer.STOP"

        const val EXTRA_RESULT_CODE = "result_code"
        const val EXTRA_RESULT_DATA = "result_data"
        const val EXTRA_DEVICE_NAME = "device_name"
        const val EXTRA_REQ_RATE = "req_rate"     // requested capture/output sample rate
        const val EXTRA_OUT_BITS = "out_bits"     // 16 / 24 / 32
        const val EXTRA_MUTE_SPEAKER = "mute_speaker"

        private const val NOTIF_CHANNEL_ID = "usb_dac_output"
        private const val NOTIF_ID = 1001

        const val CHANNELS = 2
        const val DEFAULT_RATE = 192000
        const val DEFAULT_BITS = 32

        /** Observable state for the UI. */
        val running = MutableStateFlow(false)
        val log: StateFlow<String> get() = _log
        private val _log = MutableStateFlow("")

        fun emit(line: String) {
            Log.i(TAG, line)
            _log.value = (_log.value + line + "\n").takeLast(4000)
        }
    }

    private var projection: MediaProjection? = null
    private var audioRecord: AudioRecord? = null
    private var usbAudio: UsbAudioManager? = null
    private var captureThread: Thread? = null
    @Volatile private var keepRunning = false

    private var restoreSpeakerVolume = -1
    private val mainHandler = Handler(Looper.getMainLooper())

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopEverything()
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_START -> startCapture(intent)
            else -> stopSelf()
        }
        return START_NOT_STICKY
    }

    private fun startCapture(intent: Intent) {
        if (keepRunning) {
            emit("已经在运行中")
            return
        }

        val resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0)
        val resultData = intent.getParcelableExtra<Intent>(EXTRA_RESULT_DATA)
        val deviceName = intent.getStringExtra(EXTRA_DEVICE_NAME)
        val reqRate = intent.getIntExtra(EXTRA_REQ_RATE, DEFAULT_RATE)
        val outBits = intent.getIntExtra(EXTRA_OUT_BITS, DEFAULT_BITS)
        val muteSpeaker = intent.getBooleanExtra(EXTRA_MUTE_SPEAKER, false)

        if (resultData == null || deviceName == null) {
            emit("启动参数缺失，已停止")
            stopSelf()
            return
        }

        // 1) Become a foreground service of type mediaProjection BEFORE obtaining
        //    the projection (required on Android 14+).
        startForeground(NOTIF_ID, buildNotification())

        // 2) Obtain the MediaProjection from the user's consent result.
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        val mp = mpm.getMediaProjection(resultCode, resultData)
        if (mp == null) {
            emit("获取 MediaProjection 失败")
            stopEverything(); stopSelf(); return
        }
        projection = mp
        // Android 14 requires a registered callback before the projection is used.
        mp.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                emit("MediaProjection 已被系统/用户停止")
                stopEverything(); stopSelf()
            }
        }, mainHandler)

        // 3) Build the playback-capture AudioRecord FIRST so we learn the format
        //    the system actually grants (rate may be capped below the request).
        val record = try {
            buildCaptureRecord(mp, reqRate)
        } catch (e: Exception) {
            emit("创建 AudioRecord 失败: ${e.message}")
            stopEverything(); stopSelf(); return
        }
        audioRecord = record
        val actualRate = record.sampleRate
        val isFloat = record.audioFormat == AudioFormat.ENCODING_PCM_FLOAT
        emit("捕获格式: ${actualRate}Hz ${if (isFloat) "float" else "16bit"}/立体声")

        // 4) Open the USB DAC at the captured rate and the chosen bit depth.
        val usb = UsbAudioManager(this)
        val info = usb.detectDevices().find { it.device.deviceName == deviceName }
        if (info == null) {
            emit("未找到 USB 设备: $deviceName")
            stopEverything(); stopSelf(); return
        }
        if (!usb.open(info, actualRate, outBits, CHANNELS)) {
            emit("打开 USB DAC 失败（权限/格式/端点不支持）")
            stopEverything(); stopSelf(); return
        }
        if (!usb.start()) {
            emit("启动 USB 输出失败")
            usb.close(); stopEverything(); stopSelf(); return
        }
        usbAudio = usb
        emit("USB DAC 已打开: ${usb.getDeviceInfo()}")

        if (muteSpeaker) muteLocalSpeaker()

        // 5) Pump capture -> 32-bit LE -> USB ring buffer on a dedicated thread.
        keepRunning = true
        running.value = true
        record.startRecording()
        val bytesPerSample = outBits / 8
        emit("开始独占输出（${actualRate / 1000}kHz/${outBits}bit/立体声）")

        captureThread = thread(name = "usb-dac-pump", priority = Thread.MAX_PRIORITY) {
            val usbRef = usbAudio
            val frames = 2048
            val out = ByteArray(frames * CHANNELS * bytesPerSample)
            if (isFloat) {
                val fbuf = FloatArray(frames * CHANNELS)
                while (keepRunning) {
                    val n = record.read(fbuf, 0, fbuf.size, AudioRecord.READ_BLOCKING)
                    if (n > 0) {
                        val bytes = floatToPcmLE(fbuf, n, out, outBits)
                        if (usbRef?.writeFully(out, 0, bytes) != true) break
                    } else if (n < 0) { emit("AudioRecord.read 错误: $n"); break }
                }
            } else {
                val sbuf = ShortArray(frames * CHANNELS)
                while (keepRunning) {
                    val n = record.read(sbuf, 0, sbuf.size)
                    if (n > 0) {
                        val bytes = s16ToPcmLE(sbuf, n, out, outBits)
                        if (usbRef?.writeFully(out, 0, bytes) != true) break
                    } else if (n < 0) { emit("AudioRecord.read 错误: $n"); break }
                }
            }
            emit("捕获线程结束")
        }
    }

    /** Convert n float samples [-1,1] to interleaved LE PCM of [bits] (16/24/32). */
    private fun floatToPcmLE(src: FloatArray, n: Int, dst: ByteArray, bits: Int): Int {
        var j = 0
        for (i in 0 until n) {
            var f = src[i]
            if (f > 1f) f = 1f else if (f < -1f) f = -1f
            when (bits) {
                16 -> {
                    val v = (f * 32767.0f).toInt()
                    dst[j] = v.toByte(); dst[j + 1] = (v shr 8).toByte(); j += 2
                }
                24 -> {
                    val v = (f * 8388607.0f).toInt()
                    dst[j] = v.toByte(); dst[j + 1] = (v shr 8).toByte()
                    dst[j + 2] = (v shr 16).toByte(); j += 3
                }
                else -> {
                    val v = (f * 2147483647.0f).toInt()
                    dst[j] = v.toByte(); dst[j + 1] = (v shr 8).toByte()
                    dst[j + 2] = (v shr 16).toByte(); dst[j + 3] = (v shr 24).toByte(); j += 4
                }
            }
        }
        return j
    }

    /** Convert n 16-bit samples to interleaved LE PCM of [bits] (16/24/32). */
    private fun s16ToPcmLE(src: ShortArray, n: Int, dst: ByteArray, bits: Int): Int {
        var j = 0
        for (i in 0 until n) {
            val s = src[i].toInt()
            when (bits) {
                16 -> { dst[j] = s.toByte(); dst[j + 1] = (s shr 8).toByte(); j += 2 }
                24 -> {
                    dst[j] = 0; dst[j + 1] = s.toByte(); dst[j + 2] = (s shr 8).toByte(); j += 3
                }
                else -> {
                    val v = s shl 16
                    dst[j] = v.toByte(); dst[j + 1] = (v shr 8).toByte()
                    dst[j + 2] = (v shr 16).toByte(); dst[j + 3] = (v shr 24).toByte(); j += 4
                }
            }
        }
        return j
    }

    private fun buildCaptureRecord(mp: MediaProjection, requestedRate: Int): AudioRecord {
        // Capture only media-like usages so we don't pull in notifications/voice.
        val config = AudioPlaybackCaptureConfiguration.Builder(mp)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .build()

        // Try float at the requested high rate first, then progressively fall back.
        val candidates = listOf(
            requestedRate to AudioFormat.ENCODING_PCM_FLOAT,
            96000 to AudioFormat.ENCODING_PCM_FLOAT,
            48000 to AudioFormat.ENCODING_PCM_FLOAT,
            48000 to AudioFormat.ENCODING_PCM_16BIT
        )
        var lastError: Exception? = null
        for ((rate, encoding) in candidates) {
            try {
                val format = AudioFormat.Builder()
                    .setEncoding(encoding)
                    .setSampleRate(rate)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                    .build()
                val minBuf = AudioRecord.getMinBufferSize(rate, AudioFormat.CHANNEL_IN_STEREO, encoding)
                    .coerceAtLeast(8192)
                val rec = AudioRecord.Builder()
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(minBuf * 4)
                    .setAudioPlaybackCaptureConfig(config)
                    .build()
                if (rec.state == AudioRecord.STATE_INITIALIZED) return rec
                rec.release()
            } catch (e: Exception) {
                lastError = e
            }
        }
        throw lastError ?: IllegalStateException("AudioRecord init failed")
    }

    /**
     * Best-effort: drop the on-device media stream to silence so the user doesn't
     * hear the phone speaker duplicating what the DAC is playing. This is a
     * playback-side change; the captured copy feeding the DAC is unaffected.
     * Behaviour can vary by device/Android version.
     */
    private fun muteLocalSpeaker() {
        try {
            val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            restoreSpeakerVolume = am.getStreamVolume(AudioManager.STREAM_MUSIC)
            am.setStreamVolume(AudioManager.STREAM_MUSIC, 0, 0)
            emit("已尝试静音手机扬声器（音乐音量 -> 0）")
        } catch (e: Exception) {
            emit("静音扬声器失败: ${e.message}")
        }
    }

    private fun restoreLocalSpeaker() {
        if (restoreSpeakerVolume >= 0) {
            try {
                val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
                am.setStreamVolume(AudioManager.STREAM_MUSIC, restoreSpeakerVolume, 0)
            } catch (_: Exception) {
            }
            restoreSpeakerVolume = -1
        }
    }

    private fun stopEverything() {
        keepRunning = false
        running.value = false

        captureThread?.let {
            try {
                it.join(500)
            } catch (_: InterruptedException) {
            }
        }
        captureThread = null

        audioRecord?.let {
            try {
                it.stop()
            } catch (_: Exception) {
            }
            it.release()
        }
        audioRecord = null

        usbAudio?.let {
            it.stop()
            it.close()
        }
        usbAudio = null

        projection?.stop()
        projection = null

        restoreLocalSpeaker()

        try {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } catch (_: Exception) {
        }
        emit("已停止")
    }

    override fun onDestroy() {
        stopEverything()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIF_CHANNEL_ID,
                getString(R.string.notif_channel_name),
                NotificationManager.IMPORTANCE_LOW
            )
            nm.createNotificationChannel(channel)
        }
        return Notification.Builder(this, NOTIF_CHANNEL_ID)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(getString(R.string.notif_text))
            .setSmallIcon(R.drawable.ic_launcher)
            .setOngoing(true)
            .build()
    }
}
