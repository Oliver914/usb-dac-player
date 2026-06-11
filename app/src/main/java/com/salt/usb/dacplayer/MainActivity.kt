// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 三水深
package com.salt.usb.dacplayer

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.updatePadding
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.lifecycle.Lifecycle
import com.salt.usb.audio.UsbAudioManager
import com.salt.usb.dacplayer.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbAudio: UsbAudioManager

    /** Capture can't exceed the system mix rate by much; cap offered rates here. */
    private val captureRateCeiling = 192000

    private var devices: List<UsbAudioManager.UsbAudioDeviceInfo> = emptyList()

    /** A selectable output quality. null rate/bits = "auto (highest)". */
    private data class FormatOption(val rate: Int?, val bits: Int?, val label: String)

    private var formatOptions: List<FormatOption> = listOf(
        FormatOption(null, null, "")  // placeholder until probed
    )
    private var probedFormats: UsbAudioManager.SupportedFormats? = null

    private val prefs by lazy { getSharedPreferences("dac_prefs", Context.MODE_PRIVATE) }

    private val projectionManager by lazy {
        getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
    }

    // --- Runtime permissions (RECORD_AUDIO is mandatory for playback capture) ---
    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result ->
        if (result[Manifest.permission.RECORD_AUDIO] == false) {
            toast("需要录音权限才能捕获系统音频")
        }
    }

    // --- MediaProjection consent ---
    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { res ->
        if (res.resultCode == RESULT_OK && res.data != null) {
            launchService(res.resultCode, res.data!!)
        } else {
            toast("未授予屏幕/音频捕获权限")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Edge-to-edge: we draw behind the system bars and pad from insets.
        WindowCompat.setDecorFitsSystemWindows(window, false)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        applyWindowInsets()

        usbAudio = UsbAudioManager(this)

        binding.refreshButton.setOnClickListener { refreshDevices() }
        binding.grantButton.setOnClickListener { grantUsbPermission() }
        binding.startButton.setOnClickListener { start() }
        binding.stopButton.setOnClickListener { stop() }
        binding.shareLogButton.setOnClickListener { shareLogs() }

        // Re-probe formats when the selected device changes (if authorized).
        binding.deviceSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                maybeProbeSelectedDevice()
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
        // Persist the chosen quality so it's restored next launch.
        binding.formatSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                saveFormatChoice(pos)
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
        setFormatPlaceholder(getString(R.string.quality_need_grant))

        requestRuntimePermissions()
        refreshDevices()
        observeServiceState()
    }

    private fun requestRuntimePermissions() {
        val perms = mutableListOf(Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms += Manifest.permission.POST_NOTIFICATIONS
        }
        permissionLauncher.launch(perms.toTypedArray())
    }

    private fun refreshDevices() {
        devices = usbAudio.detectDevices()
        val labels = if (devices.isEmpty()) {
            listOf(getString(R.string.no_device))
        } else {
            devices.map { it.name }
        }
        binding.deviceSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, labels
        )
        binding.startButton.isEnabled = devices.isNotEmpty()
    }

    private fun selectedDevice(): UsbAudioManager.UsbAudioDeviceInfo? {
        val pos = binding.deviceSpinner.selectedItemPosition
        return devices.getOrNull(pos)
    }

    private fun grantUsbPermission() {
        val dev = selectedDevice() ?: run { toast("请先选择设备"); return }
        lifecycleScope.launch {
            usbAudio.requestPermission(dev.device).collect { granted ->
                toast(if (granted) "USB 权限已授予" else "USB 权限被拒绝")
                if (granted) probeAndPopulate(dev)
            }
        }
    }

    /** Probe formats for the current device if we have permission, else show hint. */
    private fun maybeProbeSelectedDevice() {
        val dev = selectedDevice()
        if (dev == null) {
            setFormatPlaceholder(getString(R.string.no_device)); return
        }
        if (usbAudio.hasUsbPermission(dev)) probeAndPopulate(dev)
        else setFormatPlaceholder(getString(R.string.quality_need_grant))
    }

    private fun probeAndPopulate(dev: UsbAudioManager.UsbAudioDeviceInfo) {
        setFormatPlaceholder("探测中…")
        lifecycleScope.launch {
            val formats = withContext(Dispatchers.IO) { usbAudio.probeFormats(dev) }
            probedFormats = formats
            if (formats == null || formats.bitDepths.isEmpty()) {
                setFormatPlaceholder(getString(R.string.quality_need_grant))
                return@launch
            }
            buildFormatOptions(formats)
            populateFormatSpinner(dev)
        }
    }

    /** Build the high→low quality list: "auto" first, then rate×bit combos. */
    private fun buildFormatOptions(f: UsbAudioManager.SupportedFormats) {
        val rates = f.sampleRates.filter { it in 1..captureRateCeiling }
            .ifEmpty { listOf(48000) }.sortedDescending()
        val bits = f.bitDepths.ifEmpty { listOf(16) }.sortedDescending()
        val opts = mutableListOf(FormatOption(null, null, getString(R.string.quality_auto)))
        for (r in rates) for (b in bits) {
            opts.add(FormatOption(r, b, "${r / 1000}kHz / ${b}bit"))
        }
        formatOptions = opts
    }

    private fun populateFormatSpinner(dev: UsbAudioManager.UsbAudioDeviceInfo) {
        binding.formatSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, formatOptions.map { it.label }
        )
        // Restore the remembered choice for this exact DAC model.
        val saved = prefs.getString(formatPrefKey(dev), "auto")
        val idx = formatOptions.indexOfFirst { optionKey(it) == saved }.takeIf { it >= 0 } ?: 0
        binding.formatSpinner.setSelection(idx)
    }

    private fun setFormatPlaceholder(text: String) {
        formatOptions = listOf(FormatOption(null, null, text))
        binding.formatSpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, listOf(text)
        )
    }

    private fun saveFormatChoice(pos: Int) {
        val dev = selectedDevice() ?: return
        val opt = formatOptions.getOrNull(pos) ?: return
        if (probedFormats == null) return  // ignore placeholder selections
        prefs.edit().putString(formatPrefKey(dev), optionKey(opt)).apply()
    }

    private fun optionKey(o: FormatOption): String =
        if (o.rate == null || o.bits == null) "auto" else "${o.rate},${o.bits}"

    private fun formatPrefKey(dev: UsbAudioManager.UsbAudioDeviceInfo): String =
        "fmt_%04X_%04X".format(dev.vendorId, dev.productId)

    /** Resolve the current selection into a concrete (rate, bits) to stream. */
    private fun resolveFormat(): Pair<Int, Int> {
        val f = probedFormats
        val opt = formatOptions.getOrNull(binding.formatSpinner.selectedItemPosition)
        if (opt != null && opt.rate != null && opt.bits != null) return opt.rate to opt.bits
        // Auto: highest supported bit depth + highest rate within the capture ceiling.
        val bits = f?.bitDepths?.maxOrNull() ?: 32
        val rate = f?.sampleRates?.filter { it <= captureRateCeiling }?.maxOrNull() ?: 192000
        return rate to bits
    }

    private fun start() {
        val dev = selectedDevice() ?: run { toast("请先选择设备"); return }

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestRuntimePermissions()
            toast("请先授予录音权限")
            return
        }

        if (!usbAudio.hasUsbPermission(dev)) {
            toast("请先点击“授权 USB 设备”")
            grantUsbPermission()
            return
        }

        // Ask the system for media-capture consent; service starts on the result.
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    private fun launchService(resultCode: Int, data: Intent) {
        val dev = selectedDevice() ?: return
        val (rate, bits) = resolveFormat()
        val intent = Intent(this, AudioCaptureService::class.java).apply {
            action = AudioCaptureService.ACTION_START
            putExtra(AudioCaptureService.EXTRA_RESULT_CODE, resultCode)
            putExtra(AudioCaptureService.EXTRA_RESULT_DATA, data)
            putExtra(AudioCaptureService.EXTRA_DEVICE_NAME, dev.device.deviceName)
            putExtra(AudioCaptureService.EXTRA_REQ_RATE, rate)
            putExtra(AudioCaptureService.EXTRA_OUT_BITS, bits)
            putExtra(AudioCaptureService.EXTRA_MUTE_SPEAKER, binding.muteSpeakerSwitch.isChecked)
        }
        ContextCompat.startForegroundService(this, intent)
    }

    private fun stop() {
        val intent = Intent(this, AudioCaptureService::class.java).apply {
            action = AudioCaptureService.ACTION_STOP
        }
        startService(intent)
    }

    /** Export a diagnostic log (device info + probe + app log + native logcat)
     *  and open the system share sheet so users can send it for bug reports. */
    private fun shareLogs() {
        toast("正在生成日志…")
        lifecycleScope.launch {
            val file = withContext(Dispatchers.IO) { writeLogFile() }
            if (file == null) { toast("生成日志失败"); return@launch }
            val uri = FileProvider.getUriForFile(
                this@MainActivity, "$packageName.fileprovider", file
            )
            val send = Intent(Intent.ACTION_SEND).apply {
                type = "text/plain"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, "USB DAC Player 日志")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
            startActivity(Intent.createChooser(send, getString(R.string.share_log)))
        }
    }

    private fun writeLogFile(): java.io.File? = try {
        val sb = StringBuilder()
        val ver = runCatching { packageManager.getPackageInfo(packageName, 0).versionName }.getOrNull()
        sb.append("USB DAC Player v$ver\n")
        sb.append("Device: ${Build.MANUFACTURER} ${Build.MODEL}, Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n")
        sb.append("Time: ${java.text.SimpleDateFormat("yyyy-MM-dd HH:mm:ss", java.util.Locale.US).format(java.util.Date())}\n")
        probedFormats?.let { sb.append("Probed: bits=${it.bitDepths} rates=${it.sampleRates} uac2=${it.isUac2}\n") }
        sb.append("\n=== app log ===\n").append(AudioCaptureService.log.value)
        sb.append("\n=== logcat (this app) ===\n")
        runCatching {
            val p = Runtime.getRuntime().exec(
                arrayOf("logcat", "-d", "-v", "time",
                    "-s", "UsbAudioOutput:V", "UsbAudioManager:V", "AudioCaptureService:V")
            )
            p.inputStream.bufferedReader().use { sb.append(it.readText()) }
        }.onFailure { sb.append("logcat 读取失败: ${it.message}\n") }

        val f = java.io.File(cacheDir, "usb-dac-log.txt")
        f.writeText(sb.toString())
        f
    } catch (e: Exception) {
        null
    }

    /** Edge-to-edge: pad content out from system bars + display cutout (notch). */
    private fun applyWindowInsets() {
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        WindowInsetsControllerCompat(window, binding.root).apply {
            isAppearanceLightStatusBars = !night
            isAppearanceLightNavigationBars = !night
        }
        val base = (16 * resources.displayMetrics.density).toInt()
        ViewCompat.setOnApplyWindowInsetsListener(binding.content) { v, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            v.updatePadding(
                left = base + bars.left,
                top = base + bars.top,
                right = base + bars.right,
                bottom = base + bars.bottom
            )
            insets
        }
    }

    private fun observeServiceState() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    AudioCaptureService.running.collect { running ->
                        binding.startButton.isEnabled = !running && devices.isNotEmpty()
                        binding.stopButton.isEnabled = running
                        binding.refreshButton.isEnabled = !running
                        binding.statusLabel.setText(
                            if (running) R.string.status_running else R.string.status_idle
                        )
                        if (!running) binding.formatLabel.setText(R.string.status_hint)
                    }
                }
                launch {
                    AudioCaptureService.log.collect { text ->
                        binding.logView.text = text
                        // Surface the active format line (e.g. "…192kHz/32bit/立体声").
                        text.lineSequence().lastOrNull { it.contains("kHz") }
                            ?.let { binding.formatLabel.text = it.trim() }
                    }
                }
            }
        }
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
}
