package com.namnh.awesomecam

import java.io.File
import kotlin.concurrent.thread

class Mp4FeedController(
    private val postStatus: (String) -> Unit,
) {
    @Volatile
    private var worker: Thread? = null

    @Volatile
    private var active: Boolean = false

    val isRunning: Boolean
        get() = active

    fun start(path: String = DEFAULT_VIDEO_PATH, displayName: String? = null) {
        if (isRunning) return
        active = true
        worker = thread(start = true, name = "ffmpeg-player-control") {
            try {
                if (refreshRunningState()) {
                    postStatus("Video feed: native FFmpeg player is already running")
                    return@thread
                }

                val source = prepareReadableSource(path)
                active = true
                val sourceLabel = displayName?.takeIf { it.isNotBlank() } ?: "staged video"
                postStatus("Video feed: starting native FFmpeg player for $sourceLabel")
                val output = runRoot(buildStartCommands(source))
                active = parseStartedPid(output) != null
                postStatus("Video feed: native player start requested\n$output")
                if (active) {
                    postStatus("Video feed: open camera/webcam preview and watch FFmpegPlayer + source=FFmpegPlayback logs")
                } else {
                    postStatus("Video feed: player PID was not observed; check /data/camera/awesomecam_player.log")
                }
            } catch (t: Throwable) {
                active = false
                postStatus("Video feed error: ${t.message}")
            } finally {
                worker = null
            }
        }
    }

    fun stop() {
        thread(start = true, name = "ffmpeg-player-stop") {
            val output = stopBlocking()
            postStatus("Video feed: native player stop requested; source cache cleared\n$output")
        }
    }

    fun stopBlocking(): String {
        worker?.interrupt()
        worker = null
        active = false
        val output = runRoot(buildStopCommands())
        runCatching { VideoBridge.clear() }
        active = false
        return output
    }

    fun refreshRunningStateAsync(onComplete: (Boolean) -> Unit = {}) {
        thread(start = true, name = "ffmpeg-player-probe") {
            val running = refreshRunningState()
            onComplete(running)
        }
    }

    fun refreshRunningState(): Boolean {
        return try {
            val output = runRoot(buildProbeCommands())
            val running = parseProbeResult(output) != null
            active = running
            running
        } catch (_: Throwable) {
            active = false
            false
        }
    }

    private fun prepareReadableSource(path: String): String {
        if (path == DEFAULT_VIDEO_PATH) return path
        val source = File(path)
        require(source.exists()) { "Missing source $path" }
        return source.absolutePath
    }

    private fun buildStartCommands(source: String): List<String> = listOf(
        "mkdir -p ${shellQuote(RUNTIME_DIR)}",
        "if [ ! -r ${shellQuote(source)} ]; then echo 'ERROR: no staged video found; choose a video first'; exit 1; fi",
        "rm -f ${shellQuote(PLAYER_PID)}",
        "export LD_LIBRARY_PATH=${shellQuote(RUNTIME_DIR)}:\${LD_LIBRARY_PATH}",
        "cd ${shellQuote(RUNTIME_DIR)}",
        "${shellQuote(PLAYER_PATH)} --input ${shellQuote(source)} --loop --pidfile ${shellQuote(PLAYER_PID)} >> ${shellQuote(PLAYER_LOG)} 2>&1 &",
        "sleep 0.2",
        "cat ${shellQuote(PLAYER_PID)} 2>/dev/null || true",
    )

    private fun buildStopCommands(): List<String> = listOf(
        "if [ -f ${shellQuote(PLAYER_PID)} ]; then kill -TERM $(cat ${shellQuote(PLAYER_PID)}) 2>/dev/null || true; fi",
        "pkill -TERM -f ${shellQuote(PLAYER_PATH)} 2>/dev/null || true",
        "rm -f ${shellQuote(PLAYER_PID)}",
    )

    private fun buildProbeCommands(): List<String> = listOf(
        """
        pid=''
        running=''
        if [ -f ${shellQuote(PLAYER_PID)} ]; then
          pid=$(cat ${shellQuote(PLAYER_PID)} 2>/dev/null | head -n 1)
        fi
        if [ -n "${'$'}pid" ] && kill -0 "${'$'}pid" 2>/dev/null; then
          cmd=$(tr '\0' ' ' < "/proc/${'$'}pid/cmdline" 2>/dev/null || true)
          case "${'$'}cmd" in *awesomecam_player*) running="${'$'}pid";; esac
        fi
        if [ -z "${'$'}running" ]; then
          rm -f ${shellQuote(PLAYER_PID)}
          set -- $(pidof awesomecam_player 2>/dev/null)
          pid="${'$'}1"
          if [ -n "${'$'}pid" ] && kill -0 "${'$'}pid" 2>/dev/null; then
            running="${'$'}pid"
          fi
        fi
        if [ -n "${'$'}running" ]; then
          echo "RUNNING ${'$'}running"
        else
          echo "STOPPED"
        fi
        """.trimIndent(),
    )

    private fun parseStartedPid(output: String): String? {
        return output.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.toIntOrNull() != null }
    }

    private fun parseProbeResult(output: String): String? {
        return output.lineSequence()
            .map { it.trim() }
            .firstOrNull { it.startsWith("RUNNING ") }
            ?.substringAfter(' ')
            ?.takeIf { it.isNotBlank() }
    }

    private fun runRoot(commands: List<String>): String {
        val process = ProcessBuilder("su")
            .redirectErrorStream(true)
            .start()
        process.outputStream.bufferedWriter().use { writer ->
            for (command in commands) {
                writer.write(command)
                writer.newLine()
            }
            writer.write("exit")
            writer.newLine()
            writer.flush()
        }
        val output = process.inputStream.bufferedReader().readText()
        val exitCode = process.waitFor()
        return "exit=$exitCode\n" + output.ifBlank { "(no output)" }
    }

    private fun shellQuote(value: String): String {
        return "'" + value.replace("'", "'\"'\"'") + "'"
    }

    companion object {
        const val DEFAULT_VIDEO_PATH: String = "/data/camera/input.mp4"
        private const val RUNTIME_DIR = "/data/camera"
        private const val PLAYER_PATH = "$RUNTIME_DIR/awesomecam_player"
        private const val PLAYER_PID = "$RUNTIME_DIR/awesomecam_player.pid"
        private const val PLAYER_LOG = "$RUNTIME_DIR/awesomecam_player.log"
    }
}
