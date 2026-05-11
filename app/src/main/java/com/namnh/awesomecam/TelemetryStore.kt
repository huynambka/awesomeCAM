package com.namnh.awesomecam

import android.os.Handler
import android.os.Looper
import java.util.concurrent.CopyOnWriteArraySet
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

object TelemetryStore {
    private const val LOGCAT_PLACEHOLDER = "Waiting for logcat..."
    private const val LOGCAT_COMMAND = "logcat -v time -s awesomeCAM:I *:S"
    private const val MAX_OUTPUT_CHARS = 16000

    data class Snapshot(
        val status: String,
        val log: String,
    )

    interface Listener {
        fun onTelemetryChanged(snapshot: Snapshot)
    }

    private val mainHandler = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArraySet<Listener>()
    private val lock = Any()
    private val visibleCount = AtomicInteger(0)

    @Volatile
    private var logcatProcess: Process? = null

    @Volatile
    private var logcatThread: Thread? = null

    private var statusText: String = ""
    private var logText: String = LOGCAT_PLACEHOLDER
    private var lastPromotedExecutionLine: String = ""

    fun setInitialStatusIfEmpty(value: String) {
        var changed = false
        synchronized(lock) {
            if (statusText.isBlank()) {
                statusText = trimOutput(value)
                changed = true
            }
        }
        if (changed) dispatch()
    }

    fun appendStatus(message: String) {
        synchronized(lock) {
            statusText = trimOutput(
                buildString {
                    if (statusText.isNotBlank()) {
                        append(statusText)
                        append("\n\n")
                    }
                    append(message)
                },
            )
        }
        dispatch()
    }

    fun appendLogLine(line: String) {
        val promoted = promoteImportantLogToExecutionStatus(line)
        synchronized(lock) {
            logText = trimOutput(
                if (logText.isBlank() || logText == LOGCAT_PLACEHOLDER) {
                    line
                } else {
                    "$logText\n$line"
                },
            )
            if (promoted != null && promoted != lastPromotedExecutionLine) {
                statusText = appendStatusLocked(promoted)
                lastPromotedExecutionLine = promoted
            }
        }
        dispatch()
    }

    fun snapshot(): Snapshot = synchronized(lock) {
        Snapshot(status = statusText, log = logText)
    }

    fun addListener(listener: Listener) {
        listeners.add(listener)
        val snapshot = snapshot()
        mainHandler.post { listener.onTelemetryChanged(snapshot) }
    }

    fun removeListener(listener: Listener) {
        listeners.remove(listener)
    }

    fun acquireLogcat() {
        if (visibleCount.incrementAndGet() == 1) {
            startLogcatIfNeeded()
        }
    }

    fun releaseLogcat() {
        val remaining = visibleCount.decrementAndGet()
        if (remaining <= 0) {
            visibleCount.set(0)
            stopLogcat()
        }
    }

    private fun dispatch() {
        val snapshot = snapshot()
        mainHandler.post {
            listeners.forEach { it.onTelemetryChanged(snapshot) }
        }
    }

    private fun startLogcatIfNeeded() {
        if (logcatThread?.isAlive == true) return

        val readerThread = thread(start = false, name = "telemetry-logcat") {
            var process: Process? = null
            try {
                process = ProcessBuilder("su", "-c", LOGCAT_COMMAND)
                    .redirectErrorStream(true)
                    .start()
                logcatProcess = process

                process.inputStream.bufferedReader().use { reader ->
                    while (true) {
                        val line = reader.readLine() ?: break
                        appendLogLine(line)
                    }
                }

                val exitCode = process.waitFor()
                if (logcatProcess === process && exitCode != 0) {
                    appendLogLine("logcat exited with code $exitCode")
                }
            } catch (t: Throwable) {
                if (logcatProcess === process) {
                    appendLogLine("Logcat error: ${t.message}")
                }
            } finally {
                if (logcatProcess === process) {
                    logcatProcess = null
                }
                if (logcatThread === Thread.currentThread()) {
                    logcatThread = null
                }
            }
        }

        logcatThread = readerThread
        readerThread.start()
    }

    private fun stopLogcat() {
        val process = logcatProcess
        logcatProcess = null
        logcatThread?.interrupt()
        logcatThread = null
        process?.destroy()
    }

    private fun appendStatusLocked(message: String): String {
        return trimOutput(
            buildString {
                if (statusText.isNotBlank()) {
                    append(statusText)
                    append("\n\n")
                }
                append(message)
            },
        )
    }

    private fun promoteImportantLogToExecutionStatus(line: String): String? {
        val cameraTargetActive = Regex("""Video2CameraService target active gen=\d+ (\d+x\d+) fmt=(0x[0-9a-fA-F]+)""")
            .find(line)
        if (cameraTargetActive != null) {
            val (resolution, format) = cameraTargetActive.destructured
            return "Camera target detected: $resolution fmt=$format"
        }

        if (line.contains("RTMPDemuxer: opened stream")) {
            Regex("""codec=(\S+) mime=(\S+) src=(\d+x\d+) fps=(\d+) source=(\S+)""")
                .find(line)
                ?.let {
                    val (codec, mime, src, fps, source) = it.destructured
                    return "RTMP source active: codec=$codec mime=$mime src=$src fps=$fps source=$source"
                }
        }

        val media = line.substringAfter("MediaCodecPlayer: ", missingDelimiterValue = "")
            .takeIf { it.isNotBlank() }
            ?: return null

        Regex("""initial source=rtmp urlFile=(\S+) fpsCap=(\d+) live=1""")
            .find(media)
            ?.let {
                val (urlFile, fpsCap) = it.destructured
                return "Playback initial source: RTMP live urlFile=${urlFile.substringAfterLast('/')} fpsCap=$fpsCap"
            }

        Regex("""RTMP stream mime=(\S+) src=(\d+x\d+) out=(\d+x\d+) fps=(\d+) fpsCap=(\d+) source=(\S+)""")
            .find(media)
            ?.let {
                val (mime, src, out, fps, fpsCap, source) = it.destructured
                return "RTMP decoder active: mime=$mime src=$src out=$out fps=$fps fpsCap=$fpsCap source=$source"
            }

        Regex("""initial input=(\S+) autoVariant=(\d+) fpsCap=(\d+) default=(\S+)""")
            .find(media)
            ?.let {
                val (input, autoVariant, fpsCap, defaultInput) = it.destructured
                return "Playback initial input: ${input.substringAfterLast('/')} autoVariant=$autoVariant fpsCap=$fpsCap default=${defaultInput.substringAfterLast('/')}"
            }

        Regex("""stream=\d+ mime=\S+ src=(\d+x\d+) out=(\d+x\d+).*fpsCap=(\d+).*input=(\S+)""")
            .find(media)
            ?.let {
                val (src, out, fpsCap, input) = it.destructured
                return "Playback decoder active: src=$src out=$out fpsCap=$fpsCap input=${input.substringAfterLast('/')}"
            }

        Regex("""selected override variant (\S+) path=(\S+) reason=(\S+)""")
            .find(media)
            ?.let {
                val (variant, path, reason) = it.destructured
                return "Playback resolution selected: $variant override reason=$reason input=${path.substringAfterLast('/')}"
            }

        Regex("""selected target variant (\S+) path=(\S+) hits=(\d+) reason=(\S+)""")
            .find(media)
            ?.let {
                val (variant, path, hits, reason) = it.destructured
                return "Playback resolution selected: $variant exact target hits=$hits reason=$reason input=${path.substringAfterLast('/')}"
            }

        Regex("""selected nearest target variant (\S+) path=(\S+) for target=(\d+x\d+) hits=(\d+) score=([0-9.]+) reason=(\S+)""")
            .find(media)
            ?.let {
                val (variant, path, target, hits, score, reason) = it.destructured
                return "Playback resolution selected: $variant nearest for target=$target hits=$hits score=$score reason=$reason input=${path.substringAfterLast('/')}"
            }

        Regex("""target (\d+x\d+) has no exact mp4 variant""")
            .find(media)
            ?.let {
                val (target) = it.destructured
                return "Playback target $target has no exact MP4 variant; selecting nearest pre-scaled input and ReadyFrameCache will scale final output"
            }

        Regex("""switching input current=(\S+) next=(\S+)""")
            .find(media)
            ?.let {
                val (current, next) = it.destructured
                return "Playback input switch: ${current.substringAfterLast('/')} -> ${next.substringAfterLast('/')}"
            }

        Regex("""restarting for target variant input=(\S+)""")
            .find(media)
            ?.let {
                val (input) = it.destructured
                return "Playback restarted with selected input: ${input.substringAfterLast('/')}"
            }

        return null
    }

    private fun trimOutput(text: String): String {
        return if (text.length <= MAX_OUTPUT_CHARS) {
            text
        } else {
            text.takeLast(MAX_OUTPUT_CHARS).trimStart()
        }
    }

}
