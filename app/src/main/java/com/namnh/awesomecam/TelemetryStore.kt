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
        synchronized(lock) {
            logText = trimOutput(
                if (logText.isBlank() || logText == LOGCAT_PLACEHOLDER) {
                    line
                } else {
                    "$logText\n$line"
                },
            )
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

    private fun trimOutput(text: String): String {
        return if (text.length <= MAX_OUTPUT_CHARS) {
            text
        } else {
            text.takeLast(MAX_OUTPUT_CHARS).trimStart()
        }
    }

}
