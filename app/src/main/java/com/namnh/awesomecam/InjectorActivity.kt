package com.namnh.awesomecam

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.Button
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.NestedScrollView
import java.io.File
import java.util.zip.ZipFile
import kotlin.concurrent.thread

class InjectorActivity : AppCompatActivity() {
    private lateinit var summaryText: TextView
    private lateinit var summaryScroll: NestedScrollView
    private lateinit var buildTitleText: TextView
    private lateinit var feedController: Mp4FeedController
    private lateinit var feedButton: Button
    private lateinit var selectedVideoText: TextView

    private val chooseVideoLauncher = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        handlePickedVideo(uri)
    }

    private val listener = object : TelemetryStore.Listener {
        override fun onTelemetryChanged(snapshot: TelemetryStore.Snapshot) {
            runOnUiThread {
                summaryText.text = buildSummary(snapshot.status)
                summaryScroll.post {
                    scrollToBottom(summaryScroll)
                }
                updateFeedButton()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_injector)

        summaryText = findViewById(R.id.summaryText)
        summaryScroll = findViewById(R.id.summaryScroll)
        buildTitleText = findViewById(R.id.buildTitleText)
        feedController = Mp4FeedController(::appendStatus)
        feedButton = findViewById(R.id.feedButton)
        selectedVideoText = findViewById(R.id.selectedVideoText)
        updateFeedButton()
        updateSelectedVideoText()

        val buildIdentity = buildIdentityLabel()
        buildTitleText.text = buildIdentity
        title = buildIdentity

        TelemetryStore.setInitialStatusIfEmpty(
            "Build: $buildIdentity\n\nRequired assets: $HELPER_ASSET, $PLAYER_ASSET, $SHADOWHOOK_LIB_NAME, $HOOK_ASSET\n" +
                "Stage 1: $RUNTIME_DIR/$SHADOWHOOK_LIB_NAME\n" +
                "Stage 2: $RUNTIME_DIR/$HOOK_ASSET (call main_hook)\n" +
                "Player: $RUNTIME_DIR/$PLAYER_ASSET + FFmpeg libs\n\n" +
                "Video source: ${selectedVideoDisplayName()}",
        )

        findViewById<Button>(R.id.injectButton).setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                val localPlayer = extractAsset(PLAYER_ASSET)
                val localFfmpegLibs = FFMPEG_LIB_NAMES.mapNotNull { extractOptionalBundledNativeLib(it) }
                require(localFfmpegLibs.any { it.name == "libffmpeg.so" }) {
                    "Missing bundled FFmpeg libffmpeg.so in APK"
                }
                // libhook.so must come from APK native lib/.
                // AGP packages assets before CMake refreshes app/src/main/assets/libhook.so,
                // so assets/libhook.so can be one build stale and crash cameraserver.
                val localHook = extractBundledNativeLib(HOOK_ASSET)
                val localShadowHook = extractBundledNativeLib(SHADOWHOOK_LIB_NAME)

                appendStatus(
                    "App-private staging ready:\n" +
                        (listOf(localHelper.absolutePath, localPlayer.absolutePath, localHook.absolutePath) +
                            listOf(localShadowHook.absolutePath) + localFfmpegLibs.map { it.absolutePath })
                            .joinToString("\n"),
                )
                val commands = buildRuntimeCommands(localHelper, localPlayer, localShadowHook, localHook, localFfmpegLibs)

                appendStatus("Running injector as root")
                appendStatus(runRoot(commands))
            }
        }

        feedButton.setOnClickListener {
            if (feedController.isRunning) {
                appendStatus("Stopping MP4 feed")
                feedController.stop()
                updateFeedButton()
            } else {
                val sourceName = selectedVideoDisplayName()
                appendStatus("Starting MP4 feed from $sourceName")
                feedController.start(Mp4FeedController.DEFAULT_VIDEO_PATH, sourceName)
                updateFeedButton()
            }
        }

        findViewById<Button>(R.id.chooseVideoButton).setOnClickListener {
            chooseVideoLauncher.launch(arrayOf("video/*"))
        }

        findViewById<Button>(R.id.resetButton).setOnClickListener {
            runAsync {
                appendStatus("Restarting cameraserver")
                feedController.stop()
                appendStatus(runRoot(listOf("pkill cameraserver || killall cameraserver || true")))
                runOnUiThread { updateFeedButton() }
            }
        }

        findViewById<Button>(R.id.openLogsButton).setOnClickListener {
            startActivity(Intent(this, LogsActivity::class.java))
        }
    }

    override fun onStart() {
        super.onStart()
        TelemetryStore.addListener(listener)
        TelemetryStore.acquireLogcat()
        feedController.refreshRunningStateAsync {
            runOnUiThread { updateFeedButton() }
        }
    }

    override fun onStop() {
        TelemetryStore.removeListener(listener)
        TelemetryStore.releaseLogcat()
        super.onStop()
    }

    private fun buildIdentityLabel(): String {
        return "awesomeCAM ${BuildConfig.VERSION_NAME} · ${BuildConfig.ARCH_LABEL} · ${BuildConfig.GIT_SHA} · ${BuildConfig.BUILD_STAMP}"
    }

    private fun updateFeedButton() {
        feedButton.text = getString(
            if (feedController.isRunning) R.string.feed_button_stop else R.string.feed_button_start,
        )
    }

    private fun updateSelectedVideoText(displayName: String = selectedVideoDisplayName()) {
        selectedVideoText.text = getString(R.string.video_source_selected_format, displayName)
    }

    private fun setSelectedVideoStatus(displayName: String, statusRes: Int) {
        selectedVideoText.text = getString(statusRes, displayName)
    }

    private fun selectedVideoDisplayName(): String {
        return getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_SELECTED_VIDEO_NAME, null)
            ?.takeIf { it.isNotBlank() }
            ?: getString(R.string.video_source_default_name)
    }

    private fun persistSelectedVideoDisplayName(displayName: String) {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_SELECTED_VIDEO_NAME, displayName)
            .apply()
    }

    private fun handlePickedVideo(uri: Uri) {
        val displayName = cleanDisplayName(
            resolveDisplayName(uri) ?: getString(R.string.video_source_generic_name),
        )
        val previousDisplayName = selectedVideoDisplayName()
        setSelectedVideoStatus(displayName, R.string.video_source_staging_format)
        appendStatus("Video source: selected \"$displayName\"")

        runAsync {
            var localCopy: File? = null
            try {
                val stagedCopy = copyPickedVideoToTemp(uri, displayName)
                localCopy = stagedCopy
                val wasRunning = feedController.refreshRunningState()
                if (wasRunning) {
                    appendStatus("Video source: stopping running feed before switching video")
                    val stopOutput = feedController.stopBlocking()
                    appendStatus("Video feed: native player stop requested; source cache cleared\n$stopOutput")
                    runOnUiThread { updateFeedButton() }
                }

                appendStatus("Video source: staging \"$displayName\" for native playback")
                val output = runRoot(buildStageVideoCommands(stagedCopy))
                persistSelectedVideoDisplayName(displayName)
                appendStatus("Video source: \"$displayName\" is ready\n$output")
                runOnUiThread {
                    updateSelectedVideoText(displayName)
                    updateFeedButton()
                }

                if (wasRunning) {
                    appendStatus("Video source: restarting feed with \"$displayName\"")
                    feedController.start(Mp4FeedController.DEFAULT_VIDEO_PATH, displayName)
                    runOnUiThread { updateFeedButton() }
                }
            } catch (t: Throwable) {
                runOnUiThread { updateSelectedVideoText(previousDisplayName) }
                throw t
            } finally {
                localCopy?.delete()
            }
        }
    }

    private fun copyPickedVideoToTemp(uri: Uri, displayName: String): File {
        val extension = displayName
            .substringAfterLast('.', "mp4")
            .filter { it.isLetterOrDigit() }
            .take(8)
            .ifBlank { "mp4" }
        val outFile = File(cacheDir, "chosen-video-${System.currentTimeMillis()}.$extension")
        val input = contentResolver.openInputStream(uri)
            ?: error("Unable to open selected video")
        input.use { source ->
            outFile.outputStream().use { sink ->
                source.copyTo(sink)
            }
        }
        return outFile
    }

    private fun resolveDisplayName(uri: Uri): String? {
        if (uri.scheme == "content") {
            contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (column >= 0 && cursor.moveToFirst()) {
                    cursor.getString(column)?.takeIf { it.isNotBlank() }?.let { return it }
                }
            }
        }
        return uri.lastPathSegment
            ?.substringAfterLast('/')
            ?.takeIf { it.isNotBlank() }
    }

    private fun cleanDisplayName(name: String): String {
        return name
            .replace(Regex("[\\r\\n\\t]+"), " ")
            .trim()
            .take(96)
            .ifBlank { getString(R.string.video_source_generic_name) }
    }

    private fun runAsync(block: () -> Unit) {
        thread(start = true) {
            try {
                block()
            } catch (t: Throwable) {
                appendStatus("Error: ${t.message}")
            }
        }
    }

    private fun appendStatus(message: String) {
        TelemetryStore.appendStatus(message)
    }

    private fun buildSummary(status: String): String {
        if (status.isBlank()) return getString(R.string.summary_placeholder)
        return status.trim()
    }

    private fun scrollToBottom(scrollView: NestedScrollView) {
        val child = scrollView.getChildAt(0) ?: return
        val y = (child.height - scrollView.height).coerceAtLeast(0)
        scrollView.scrollTo(0, y)
    }

    private fun extractAsset(name: String): File {
        val outFile = File(filesDir, name)
        assets.open(name).use { input ->
            outFile.outputStream().use { output ->
                input.copyTo(output)
            }
        }
        return outFile
    }

    private fun extractBundledNativeLib(libName: String): File {
        return extractOptionalBundledNativeLib(libName)
            ?: error("Missing bundled native lib in APK: $libName")
    }

    private fun extractOptionalBundledNativeLib(libName: String): File? {
        val outFile = File(filesDir, libName)

        val abiCandidates = buildList {
            addAll(Build.SUPPORTED_ABIS.map { abi -> "lib/$abi/$libName" })
            add("lib/arm64-v8a/$libName")
            add("lib/armeabi-v7a/$libName")
            add("lib/x86_64/$libName")
            add("lib/x86/$libName")
        }.distinct()

        ZipFile(applicationInfo.sourceDir).use { zip ->
            val entry = abiCandidates
                .asSequence()
                .mapNotNull { path -> zip.getEntry(path) }
                .firstOrNull()
                ?: return null

            zip.getInputStream(entry).use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }

        return outFile
    }

    private fun buildRuntimeCommands(
        helper: File,
        player: File,
        shadowHook: File,
        hook: File,
        ffmpegLibs: List<File>,
    ): List<String> {
        val helperDst = "$RUNTIME_DIR/$HELPER_ASSET"
        val playerDst = "$RUNTIME_DIR/$PLAYER_ASSET"
        val shadowHookDst = "$RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"
        val hookDst = "$RUNTIME_DIR/$HOOK_ASSET"

        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            add("cp ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)}")
            add("cp ${shellQuote(player.absolutePath)} ${shellQuote(playerDst)}")
            for (lib in ffmpegLibs) {
                add("cp ${shellQuote(lib.absolutePath)} ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
            add("cp ${shellQuote(shadowHook.absolutePath)} ${shellQuote(shadowHookDst)}")
            add("cp ${shellQuote(hook.absolutePath)} ${shellQuote(hookDst)}")
            add("chmod 0755 ${shellQuote(helperDst)} ${shellQuote(playerDst)}")
            add("chmod 0644 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            for (lib in ffmpegLibs) {
                add("chmod 0644 ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(playerDst)} ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            for (lib in ffmpegLibs) {
                add("chcon u:object_r:system_lib_file:s0 ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
            // Do not wrap with `sh -c`.
            //
            // KernelSU keeps the top-level `su` command in u:r:su:s0, but a nested
            // `sh -c ...` can run in u:r:shell:s0. On enforcing builds, shell is
            // not allowed to execute /data/camera/injector_helper
            // (system_data_file), so injection silently fails and cameraserver has
            // no libhook.so mapped. Execute the helper directly from the su command
            // stream instead.
            add("${shellQuote(helperDst)} cameraserver ${shellQuote(shadowHookDst)}")
            add("${shellQuote(helperDst)} cameraserver ${shellQuote(hookDst)} main_hook")
        }
    }

    private fun buildStageVideoCommands(source: File): List<String> {
        val tmpDst = "$RUNTIME_DIR/.input.mp4.tmp"
        val finalDst = Mp4FeedController.DEFAULT_VIDEO_PATH
        return listOf(
            "mkdir -p ${shellQuote(RUNTIME_DIR)}",
            "rm -f ${shellQuote(tmpDst)}",
            "cp ${shellQuote(source.absolutePath)} ${shellQuote(tmpDst)}",
            "chmod 0644 ${shellQuote(tmpDst)}",
            "chcon u:object_r:awesomecam_source_file:s0 ${shellQuote(tmpDst)} 2>/dev/null || true",
            "mv -f ${shellQuote(tmpDst)} ${shellQuote(finalDst)}",
            "chmod 0644 ${shellQuote(finalDst)}",
            "chcon u:object_r:awesomecam_source_file:s0 ${shellQuote(finalDst)} 2>/dev/null || true",
            "echo 'STAGED selected video for native playback'",
        )
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
        return buildString {
            append("exit=")
            append(exitCode)
            append('\n')
            append(output.ifBlank { "(no output)" })
        }
    }

    private fun shellQuote(value: String): String {
        return "'" + value.replace("'", "'\"'\"'") + "'"
    }

    companion object {
        private const val RUNTIME_DIR = "/data/camera"
        private const val HELPER_ASSET = "injector_helper"
        private const val PLAYER_ASSET = "awesomecam_player"
        private const val HOOK_ASSET = "libhook.so"
        private const val SHADOWHOOK_LIB_NAME = "libshadowhook.so"
        private const val PREFS_NAME = "video_source"
        private const val PREF_SELECTED_VIDEO_NAME = "selected_video_display_name"
        private val FFMPEG_LIB_NAMES = listOf(
            "libffmpeg.so",
            "libffmpegexe.so",
            "libavformat.so",
            "libavcodec.so",
            "libavutil.so",
            "libswscale.so",
            "libswresample.so",
        )
    }
}
