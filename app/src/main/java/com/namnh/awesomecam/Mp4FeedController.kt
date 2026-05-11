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
        start(SourceSpec.File(path, displayName))
    }

    fun startRtmp(url: String, displayName: String? = null) {
        require(isValidRtmpUrl(url)) { "RTMP URL must start with rtmp:// or rtmpt://" }
        require(!url.contains('\n') && !url.contains('\r')) { "RTMP URL must be single-line" }
        start(SourceSpec.Rtmp(url, displayName))
    }

    private fun start(sourceSpec: SourceSpec) {
        if (isRunning) return
        active = true
        worker = thread(start = true, name = "mediacodec-player-control") {
            try {
                if (refreshRunningState()) {
                    postStatus("Video feed: stopping existing player before switching source")
                    stopBlocking()
                }

                val source = prepareSource(sourceSpec)
                active = true
                postStatus("Video feed: starting native MediaCodec player for ${source.label}")
                val output = runRoot(buildStartCommands(source))
                active = parseStartedPid(output) != null
                postStatus("Video feed: native player start requested\n$output")
                if (active) {
                    postStatus("Video feed: open camera/webcam preview and watch MediaCodecPlayer + source=MediaCodecPlayback logs")
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
        thread(start = true, name = "mediacodec-player-stop") {
            val output = stopBlocking()
            postStatus("Video feed: native player stop requested; source cache cleared\n$output")
        }
    }

    fun stopBlocking(): String {
        if (worker !== Thread.currentThread()) {
            worker?.interrupt()
        }
        worker = null
        active = false
        val output = runRoot(buildStopCommands())
        runCatching { VideoBridge.clear() }
        active = false
        return output
    }

    fun refreshRunningStateAsync(onComplete: (Boolean) -> Unit = {}) {
        thread(start = true, name = "mediacodec-player-probe") {
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

    private fun prepareSource(sourceSpec: SourceSpec): PreparedSource {
        return when (sourceSpec) {
            is SourceSpec.File -> {
                val path = prepareReadableSource(sourceSpec.path)
                val label = sourceSpec.displayName?.takeIf { it.isNotBlank() } ?: "staged video"
                PreparedSource.File(path, label)
            }
            is SourceSpec.Rtmp -> {
                val label = sourceSpec.displayName?.takeIf { it.isNotBlank() } ?: "RTMP livestream"
                PreparedSource.Rtmp(sourceSpec.url.trim(), label)
            }
        }
    }

    private fun prepareReadableSource(path: String): String {
        if (path == DEFAULT_VIDEO_PATH) return path
        val source = File(path)
        require(source.exists()) { "Missing source $path" }
        return source.absolutePath
    }

    private fun buildStartCommands(source: PreparedSource): List<String> = buildList {
        add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
        add("rm -f ${shellQuote(PLAYER_PID)}")
        add("export LD_LIBRARY_PATH=${shellQuote(RUNTIME_DIR)}:${'$'}{LD_LIBRARY_PATH}")
        add("cd ${shellQuote(RUNTIME_DIR)}")
        when (source) {
            is PreparedSource.File -> {
                add("printf 'file\\n' > ${shellQuote(SOURCE_MODE_PATH)}")
                add("chmod 0644 ${shellQuote(SOURCE_MODE_PATH)} 2>/dev/null || true")
                if (source.path == DEFAULT_VIDEO_PATH) {
                    add(buildVariantSelectionCommand())
                } else {
                    add("SOURCE=${shellQuote(source.path)}")
                }
                add("if [ ! -r \"${'$'}SOURCE\" ]; then echo \"ERROR: selected prescaled video missing/unreadable: ${'$'}SOURCE\"; exit 1; fi")
                add("${shellQuote(PLAYER_PATH)} --input \"${'$'}SOURCE\" --auto-variant --loop --fps-cap 30 --pidfile ${shellQuote(PLAYER_PID)} >> ${shellQuote(PLAYER_LOG)} 2>&1 &")
            }
            is PreparedSource.Rtmp -> {
                add("cat > ${shellQuote(RTMP_URL_PATH)} <<'AWESOMECAM_RTMP_URL'\n${source.url}\nAWESOMECAM_RTMP_URL")
                add("chmod 0600 ${shellQuote(RTMP_URL_PATH)} 2>/dev/null || true")
                add("chcon u:object_r:system_data_file:s0 ${shellQuote(RTMP_URL_PATH)} 2>/dev/null || true")
                add("printf 'rtmp\\n' > ${shellQuote(SOURCE_MODE_PATH)}")
                add("chmod 0644 ${shellQuote(SOURCE_MODE_PATH)} 2>/dev/null || true")
                add("chcon u:object_r:system_data_file:s0 ${shellQuote(SOURCE_MODE_PATH)} 2>/dev/null || true")
                add("if [ ! -s ${shellQuote(RTMP_URL_PATH)} ]; then echo \"ERROR: RTMP URL file missing/empty\"; exit 1; fi")
                add("${shellQuote(PLAYER_PATH)} --rtmp-url-file ${shellQuote(RTMP_URL_PATH)} --live --fps-cap 30 --pidfile ${shellQuote(PLAYER_PID)} >> ${shellQuote(PLAYER_LOG)} 2>&1 &")
            }
        }
        add("sleep 0.5")
        add(
            """
            pid="$(cat ${shellQuote(PLAYER_PID)} 2>/dev/null | head -n 1)"
            if [ -n "${'$'}pid" ] && kill -0 "${'$'}pid" 2>/dev/null; then
              echo "${'$'}pid"
            else
              echo "ERROR: MediaCodec player did not stay running"
              tail -n 80 ${shellQuote(PLAYER_LOG)} 2>/dev/null || true
              exit 1
            fi
            """.trimIndent(),
        )
    }

    private fun buildVariantSelectionCommand(): String {
        return """
            VARIANT='1440x1080'
            if [ -r ${shellQuote(VARIANT_OVERRIDE_PATH)} ]; then
              CANDIDATE="$(head -n 1 ${shellQuote(VARIANT_OVERRIDE_PATH)} 2>/dev/null | tr -d '[:space:]')"
              case "${'$'}CANDIDATE" in
                1440x1080|1280x720|1920x1080|640x480)
                  VARIANT="${'$'}CANDIDATE"
                  ;;
                "")
                  ;;
                *)
                  echo "WARN: invalid awesomecam_variant '${'$'}CANDIDATE'; defaulting to 1440x1080"
                  ;;
              esac
            fi
            SOURCE="$RUNTIME_DIR/input_${'$'}{VARIANT}.mp4"
            echo "SELECTED_VARIANT ${'$'}VARIANT ${'$'}SOURCE"
        """.trimIndent()
    }

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

    private fun isValidRtmpUrl(url: String): Boolean {
        return url.startsWith("rtmp://", ignoreCase = true) ||
            url.startsWith("rtmpt://", ignoreCase = true)
    }

    private sealed class SourceSpec {
        data class File(val path: String, val displayName: String?) : SourceSpec()
        data class Rtmp(val url: String, val displayName: String?) : SourceSpec()
    }

    private sealed class PreparedSource(open val label: String) {
        data class File(val path: String, override val label: String) : PreparedSource(label)
        data class Rtmp(val url: String, override val label: String) : PreparedSource(label)
    }

    companion object {
        const val STAGED_INPUT_PATH: String = "/data/camera/input.mp4"
        const val DEFAULT_VIDEO_PATH: String = "/data/camera/input_1440x1080.mp4"
        private const val RUNTIME_DIR = "/data/camera"
        private const val PLAYER_PATH = "$RUNTIME_DIR/awesomecam_player"
        private const val PLAYER_PID = "$RUNTIME_DIR/awesomecam_player.pid"
        private const val PLAYER_LOG = "$RUNTIME_DIR/awesomecam_player.log"
        private const val VARIANT_OVERRIDE_PATH = "$RUNTIME_DIR/awesomecam_variant"
        private const val SOURCE_MODE_PATH = "$RUNTIME_DIR/awesomecam_source_mode"
        private const val RTMP_URL_PATH = "$RUNTIME_DIR/awesomecam_rtmp_url"
    }
}
