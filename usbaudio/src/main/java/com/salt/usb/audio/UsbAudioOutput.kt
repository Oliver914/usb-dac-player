// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 三水深
package com.salt.usb.audio

import android.content.Context
import android.hardware.usb.*
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

/**
 * USB Audio Exclusive Output Manager.
 *
 * Provides bit-perfect USB audio output by bypassing Android's audio pipeline
 * and sending raw PCM data directly to USB DACs via isochronous transfers.
 *
 * Usage:
 * ```
 * val manager = UsbAudioManager(context)
 * manager.detectDevices() // find USB DACs
 * manager.open(device, 44100, 16, 2) // open with format
 * manager.start() // begin output
 * manager.write(pcmData) // feed PCM data
 * manager.stop() // stop output
 * manager.close() // release device
 * ```
 */
class UsbAudioManager(private val context: Context) {

    companion object {
        private const val TAG = "UsbAudioManager"

        /** USB Audio Class code */
        private const val USB_CLASS_AUDIO = UsbConstants.USB_CLASS_AUDIO

        /** Audio Streaming subclass */
        private const val USB_SUBCLASS_AUDIO_STREAMING = 0x02
    }

    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager

    /** Currently connected USB audio device */
    private var currentDevice: UsbDevice? = null
    private var currentConnection: UsbDeviceConnection? = null
    private var currentInterface: UsbInterface? = null
    private var currentEndpoint: UsbEndpoint? = null

    /** Native output engine */
    private val nativeOutput = UsbAudioOutput()

    /** Device info for discovered USB DACs */
    data class UsbAudioDeviceInfo(
        val device: UsbDevice,
        val name: String,
        val vendorId: Int,
        val productId: Int,
        val interfaceCount: Int,
        val hasAudioStreaming: Boolean
    )

    /**
     * Detect all connected USB audio devices (DACs).
     */
    fun detectDevices(): List<UsbAudioDeviceInfo> {
        val devices = mutableListOf<UsbAudioDeviceInfo>()
        val deviceList = usbManager.deviceList

        for ((_, device) in deviceList) {
            var hasAudioStreaming = false

            // Check device class
            if (device.deviceClass == USB_CLASS_AUDIO) {
                hasAudioStreaming = true
            }

            // Check each interface for audio class
            for (i in 0 until device.interfaceCount) {
                val iface = device.getInterface(i)
                if (iface.interfaceClass == USB_CLASS_AUDIO) {
                    if (iface.interfaceSubclass == USB_SUBCLASS_AUDIO_STREAMING) {
                        hasAudioStreaming = true
                    }
                }
            }

            if (hasAudioStreaming) {
                val name = buildString {
                    append(device.deviceName)
                    append(" - ")
                    append("VID:${String.format("%04X", device.vendorId)}")
                    append(" PID:${String.format("%04X", device.productId)}")
                }

                devices.add(UsbAudioDeviceInfo(
                    device = device,
                    name = name,
                    vendorId = device.vendorId,
                    productId = device.productId,
                    interfaceCount = device.interfaceCount,
                    hasAudioStreaming = hasAudioStreaming
                ))
            }
        }

        Log.i(TAG, "Found ${devices.size} USB audio device(s)")
        return devices
    }

    /** Whether the app already holds permission to access [deviceInfo]'s device. */
    fun hasUsbPermission(deviceInfo: UsbAudioDeviceInfo): Boolean =
        usbManager.hasPermission(deviceInfo.device)

    /** PCM formats a DAC advertises (for auto/manual quality selection). */
    data class SupportedFormats(
        val bitDepths: List<Int>,    // e.g. [16, 24, 32]
        val sampleRates: List<Int>,  // e.g. [44100, 48000, 96000, 192000]
        val isUac2: Boolean
    )

    /**
     * Probe a DAC's supported bit depths and sample rates without streaming.
     * Opens a temporary connection and claims the audio interfaces (required for
     * the UAC2 clock range query), then releases them.
     */
    fun probeFormats(deviceInfo: UsbAudioDeviceInfo): SupportedFormats? {
        val device = deviceInfo.device
        if (!usbManager.hasPermission(device)) return null
        val connection = usbManager.openDevice(device) ?: return null
        try {
            for (i in 0 until device.interfaceCount) {
                val iface = device.getInterface(i)
                if (iface.interfaceClass == USB_CLASS_AUDIO) connection.claimInterface(iface, true)
            }
            val result = nativeOutput.nativeProbe(connection.fileDescriptor)
            Log.i(TAG, "Probe result: $result")
            return parseFormats(result)
        } catch (e: Exception) {
            Log.e(TAG, "probeFormats failed", e)
            return null
        } finally {
            for (i in 0 until device.interfaceCount) {
                val iface = device.getInterface(i)
                if (iface.interfaceClass == USB_CLASS_AUDIO) runCatching { connection.releaseInterface(iface) }
            }
            connection.close()
        }
    }

    private fun parseFormats(s: String): SupportedFormats {
        var bits = emptyList<Int>()
        var rates = emptyList<Int>()
        var uac2 = false
        for (part in s.split(";")) {
            val kv = part.split("=", limit = 2)
            if (kv.size != 2) continue
            when (kv[0]) {
                "bits" -> bits = kv[1].split(",").mapNotNull { it.trim().toIntOrNull() }
                "rates" -> rates = kv[1].split(",").mapNotNull { it.trim().toIntOrNull() }
                "uac2" -> uac2 = kv[1].trim() == "1"
            }
        }
        return SupportedFormats(
            bitDepths = bits.distinct().sorted(),
            sampleRates = rates.distinct().sorted(),
            isUac2 = uac2
        )
    }

    /**
     * Request USB permission from the user.
     * @return Flow that emits true if permission granted, false otherwise.
     */
    fun requestPermission(device: UsbDevice): Flow<Boolean> = callbackFlow {
        if (usbManager.hasPermission(device)) {
            trySend(true)
            close()
            return@callbackFlow
        }

        val action = "com.salt.usb.audio.USB_PERMISSION"
        val receiver = object : android.content.BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: android.content.Intent) {
                if (intent.action == action) {
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    trySend(granted)
                    close()
                }
            }
        }

        // Android 13+ requires the export flag on dynamically registered receivers.
        ContextCompat.registerReceiver(
            context, receiver, android.content.IntentFilter(action),
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        // Android 14 requires the broadcast Intent backing a mutable PendingIntent
        // to be explicit (package-scoped), otherwise the request is rejected.
        val permissionIntent = android.content.Intent(action).setPackage(context.packageName)
        val pendingIntent = android.app.PendingIntent.getBroadcast(
            context, 0, permissionIntent,
            android.app.PendingIntent.FLAG_MUTABLE
        )
        usbManager.requestPermission(device, pendingIntent)

        awaitClose { context.unregisterReceiver(receiver) }
    }

    /**
     * Open a USB audio device for exclusive output.
     *
     * @param deviceInfo The USB device info from detectDevices()
     * @param sampleRate Desired sample rate (44100, 48000, 88200, 96000, 176400, 192000, etc.)
     * @param bitDepth   Desired bit depth (16, 24, or 32)
     * @param channels   Number of channels (1 = mono, 2 = stereo)
     * @return true if opened successfully
     */
    fun open(
        deviceInfo: UsbAudioDeviceInfo,
        sampleRate: Int = 44100,
        bitDepth: Int = 16,
        channels: Int = 2
    ): Boolean {
        val device = deviceInfo.device

        if (!usbManager.hasPermission(device)) {
            Log.e(TAG, "No permission for USB device")
            return false
        }

        // Find the audio streaming interface with an OUT endpoint
        var audioInterface: UsbInterface? = null
        var outEndpoint: UsbEndpoint? = null

        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.interfaceClass != USB_CLASS_AUDIO) continue
            if (iface.interfaceSubclass != USB_SUBCLASS_AUDIO_STREAMING) continue

            for (j in 0 until iface.endpointCount) {
                val ep = iface.getEndpoint(j)
                // OUT direction (host to device) and isochronous type
                if (ep.direction == UsbConstants.USB_DIR_OUT &&
                    ep.type == UsbConstants.USB_ENDPOINT_XFER_ISOC) {
                    audioInterface = iface
                    outEndpoint = ep
                    break
                }
            }
            if (audioInterface != null) break
        }

        if (audioInterface == null || outEndpoint == null) {
            Log.e(TAG, "No suitable audio streaming OUT endpoint found")
            return false
        }

        // Open connection
        val connection = usbManager.openDevice(device) ?: run {
            Log.e(TAG, "Failed to open USB device connection")
            return false
        }

        // Claim EVERY audio interface (both AudioControl and AudioStreaming) with
        // force=true. This detaches Android's kernel snd-usb-audio driver from the
        // whole audio function. Claiming only the streaming interface leaves the
        // kernel driver on the AudioControl interface, which (a) makes class
        // control transfers to the clock/feature entities fail with EIO and
        // (b) lets the kernel driver fight us, causing the device to drop
        // (NO_DEVICE) once we start streaming.
        var claimedAny = false
        for (i in 0 until device.interfaceCount) {
            val iface = device.getInterface(i)
            if (iface.interfaceClass != USB_CLASS_AUDIO) continue
            if (connection.claimInterface(iface, true)) {
                claimedAny = true
            } else {
                Log.w(TAG, "Failed to claim audio interface ${iface.id} (alt ${iface.alternateSetting})")
            }
        }
        if (!claimedAny && !connection.claimInterface(audioInterface, true)) {
            Log.e(TAG, "Failed to claim any audio interface")
            connection.close()
            return false
        }

        currentDevice = device
        currentConnection = connection
        currentInterface = audioInterface
        currentEndpoint = outEndpoint

        // Get file descriptor for native code
        val fd = connection.fileDescriptor

        // Call native open
        val result = nativeOutput.nativeOpen(fd, sampleRate, bitDepth, channels)
        if (result < 0) {
            Log.e(TAG, "Native open failed: $result")
            connection.releaseInterface(audioInterface)
            connection.close()
            currentDevice = null
            currentConnection = null
            currentInterface = null
            currentEndpoint = null
            return false
        }

        Log.i(TAG, "USB audio device opened: $deviceInfo, rate=$sampleRate, bits=$bitDepth, ch=$channels")
        return true
    }

    /**
     * Start audio output.
     */
    fun start(): Boolean {
        val result = nativeOutput.nativeStart()
        if (result < 0) {
            Log.e(TAG, "Failed to start USB audio output")
            return false
        }
        Log.i(TAG, "USB audio output started")
        return true
    }

    /**
     * Write PCM data to the USB output.
     * Data should be in the format specified when opening (interleaved, little-endian).
     *
     * @param data   PCM audio bytes
     * @param offset Starting offset
     * @param length Number of bytes to write
     * @return Number of bytes actually written, or -1 on error
     */
    fun write(data: ByteArray, offset: Int = 0, length: Int = data.size): Int {
        return nativeOutput.nativeWrite(data, offset, length)
    }

    /**
     * Write the entire buffer, blocking (busy-wait with short sleeps) while the
     * native ring buffer is full. The native [write] is non-blocking and only
     * accepts as much as currently fits, so a real-time producer (e.g. the audio
     * capture loop) must loop on the remainder to avoid silently dropping audio.
     *
     * @return true if all bytes were written, false if output stopped midway.
     */
    fun writeFully(data: ByteArray, offset: Int = 0, length: Int = data.size): Boolean {
        var pos = offset
        var remaining = length
        while (remaining > 0) {
            val n = nativeOutput.nativeWrite(data, pos, remaining)
            if (n < 0) return false   // output stopped/closed
            if (n == 0) {
                // Ring buffer full — wait roughly one USB frame (~1ms) and retry.
                try {
                    Thread.sleep(1)
                } catch (e: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return false
                }
                continue
            }
            pos += n
            remaining -= n
        }
        return true
    }

    /**
     * Get current buffer fill level (0.0 to 1.0).
     */
    fun getBufferLevel(): Float {
        return nativeOutput.nativeGetBufferLevel()
    }

    /**
     * Get device info string.
     */
    fun getDeviceInfo(): String {
        return nativeOutput.nativeGetDeviceInfo()
    }

    /**
     * Set volume in dB.
     */
    fun setVolume(volumeDb: Float): Int {
        return nativeOutput.nativeSetVolume(volumeDb)
    }

    /**
     * Stop audio output.
     */
    fun stop() {
        nativeOutput.nativeStop()
        Log.i(TAG, "USB audio output stopped")
    }

    /**
     * Close and release USB device.
     */
    fun close() {
        nativeOutput.nativeClose()

        currentConnection?.let { conn ->
            // Release every audio interface we claimed in open().
            currentDevice?.let { dev ->
                for (i in 0 until dev.interfaceCount) {
                    val iface = dev.getInterface(i)
                    if (iface.interfaceClass == USB_CLASS_AUDIO) {
                        runCatching { conn.releaseInterface(iface) }
                    }
                }
            }
            conn.close()
        }

        currentDevice = null
        currentConnection = null
        currentInterface = null
        currentEndpoint = null

        Log.i(TAG, "USB audio device closed")
    }

    /**
     * Check if a USB device is currently open.
     */
    fun isOpen(): Boolean = currentDevice != null && nativeOutput.nativeGetBufferLevel() >= 0f

    /**
     * Register for USB device attach/detach events.
     */
    fun registerDeviceListener(
        onAttached: (UsbAudioDeviceInfo) -> Unit,
        onDetached: (UsbAudioDeviceInfo) -> Unit
    ): android.content.BroadcastReceiver {
        val receiver = object : android.content.BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: android.content.Intent) {
                val device: UsbDevice = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE) ?: return
                when (intent.action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                        // Match the freshly-attached device against the audio-capable list.
                        detectDevices().find { it.device.deviceName == device.deviceName }
                            ?.let(onAttached)
                    }
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                        onDetached(
                            UsbAudioDeviceInfo(
                                device = device,
                                name = device.deviceName,
                                vendorId = device.vendorId,
                                productId = device.productId,
                                interfaceCount = device.interfaceCount,
                                hasAudioStreaming = true
                            )
                        )
                    }
                }
            }
        }

        val filter = android.content.IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        }

        // System USB broadcasts are not app-specific; register as exported.
        ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_EXPORTED)
        return receiver
    }
}

/**
 * JNI bridge to native USB audio output engine.
 */
class UsbAudioOutput {

    companion object {
        init {
            System.loadLibrary("usb_audio_output")
        }
    }

    // Native methods matching usb_audio_output.c
    external fun nativeProbe(fd: Int): String
    external fun nativeOpen(fd: Int, sampleRate: Int, bitDepth: Int, channels: Int): Int
    external fun nativeStart(): Int
    external fun nativeWrite(data: ByteArray, offset: Int, length: Int): Int
    external fun nativeStop()
    external fun nativeClose()
    external fun nativeSetVolume(volumeDb: Float): Int
    external fun nativeGetDeviceInfo(): String
    external fun nativeGetBufferLevel(): Float
}
