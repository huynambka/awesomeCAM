package com.namnh.awesomecam

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.widget.Button
import android.widget.EditText
import android.widget.RadioButton
import android.widget.RadioGroup
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
    private lateinit var sourceModeGroup: RadioGroup
    private lateinit var fileSourceRadio: RadioButton
    private lateinit var rtmpSourceRadio: RadioButton
    private lateinit var rtmpUrlEditText: EditText

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
        sourceModeGroup = findViewById(R.id.sourceModeGroup)
        fileSourceRadio = findViewById(R.id.fileSourceRadio)
        rtmpSourceRadio = findViewById(R.id.rtmpSourceRadio)
        rtmpUrlEditText = findViewById(R.id.rtmpUrlEditText)
        rtmpUrlEditText.setText(persistedRtmpUrl())
        updateSourceModeControls(selectedSourceMode())
        updateFeedButton()
        updateSelectedVideoText()

        val buildIdentity = buildIdentityLabel()
        buildTitleText.text = buildIdentity
        title = buildIdentity

        TelemetryStore.setInitialStatusIfEmpty(
            "Build: $buildIdentity\n\nRequired assets: $HELPER_ASSET, $PLAYER_ASSET, $SHADOWHOOK_LIB_NAME, $HOOK_ASSET\n" +
                "SELinux: runtime ksud policy apply; no sepolicy patcher module required\n" +
                "Stage 1: $RUNTIME_DIR/$SHADOWHOOK_LIB_NAME\n" +
                "Stage 2: $RUNTIME_DIR/$HOOK_ASSET (call main_hook)\n" +
                "Player: $RUNTIME_DIR/$PLAYER_ASSET (MediaCodec) + FFmpeg prescale/RTMP demux tool\n\n" +
                "Default playback variant: ${Mp4FeedController.DEFAULT_VIDEO_PATH}\n" +
                "Video source: ${selectedSourceDisplayName()}",
        )

        findViewById<Button>(R.id.injectButton).setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                val localPlayer = extractAsset(PLAYER_ASSET)
                val localFfmpegLibs = extractBundledFfmpegRuntime()
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
                appendStatus("Stopping video feed")
                feedController.stop()
                updateFeedButton()
            } else {
                startSelectedFeed()
                updateFeedButton()
            }
        }

        findViewById<Button>(R.id.chooseVideoButton).setOnClickListener {
            persistSourceMode(SourceMode.FILE)
            updateSourceModeControls(SourceMode.FILE)
            chooseVideoLauncher.launch(arrayOf("video/*"))
        }

        findViewById<Button>(R.id.saveRtmpButton).setOnClickListener {
            val url = rtmpUrlEditText.text?.toString()?.trim().orEmpty()
            if (!isValidRtmpUrl(url)) {
                appendStatus("RTMP source error: URL must start with rtmp:// or rtmpt://")
                return@setOnClickListener
            }
            persistRtmpUrl(url)
            persistSourceMode(SourceMode.RTMP)
            updateSourceModeControls(SourceMode.RTMP)
            appendStatus("RTMP source saved: ${redactedRtmpUrl(url)}")
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
        selectedVideoText.text = getString(R.string.video_source_selected_format, selectedSourceDisplayName(displayName))
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

    private fun selectedSourceDisplayName(fileDisplayName: String = selectedVideoDisplayName()): String {
        return when (selectedSourceMode()) {
            SourceMode.FILE -> fileDisplayName
            SourceMode.RTMP -> persistedRtmpUrl()
                .takeIf { it.isNotBlank() }
                ?.let { redactedRtmpUrl(it) }
                ?: getString(R.string.video_source_rtmp_name)
        }
    }

    private fun selectedSourceMode(): SourceMode {
        val raw = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_SOURCE_MODE, SourceMode.FILE.value)
        return SourceMode.fromValue(raw)
    }

    private fun persistSourceMode(mode: SourceMode) {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_SOURCE_MODE, mode.value)
            .apply()
    }

    private fun persistedRtmpUrl(): String {
        return getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_RTMP_URL, null)
            ?.takeIf { it.isNotBlank() }
            ?: ""
    }

    private fun persistRtmpUrl(url: String) {
        getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_RTMP_URL, url)
            .apply()
    }

    private fun updateSourceModeControls(mode: SourceMode) {
        sourceModeGroup.setOnCheckedChangeListener(null)
        fileSourceRadio.isChecked = mode == SourceMode.FILE
        rtmpSourceRadio.isChecked = mode == SourceMode.RTMP
        sourceModeGroup.setOnCheckedChangeListener { _, checkedId ->
            val selected = if (checkedId == R.id.rtmpSourceRadio) SourceMode.RTMP else SourceMode.FILE
            persistSourceMode(selected)
            updateSelectedVideoText()
        }
        updateSelectedVideoText()
    }

    private fun startSelectedFeed() {
        when (selectedSourceMode()) {
            SourceMode.FILE -> {
                val sourceName = selectedVideoDisplayName()
                appendStatus("Starting file feed from $sourceName")
                feedController.start(Mp4FeedController.DEFAULT_VIDEO_PATH, sourceName)
            }
            SourceMode.RTMP -> {
                val url = rtmpUrlEditText.text?.toString()?.trim().orEmpty()
                if (!isValidRtmpUrl(url)) {
                    appendStatus("RTMP source error: URL must start with rtmp:// or rtmpt://")
                    return
                }
                persistRtmpUrl(url)
                appendStatus("Starting RTMP feed from ${redactedRtmpUrl(url)}")
                feedController.startRtmp(url, getString(R.string.video_source_rtmp_name))
            }
        }
    }

    private fun isValidRtmpUrl(url: String): Boolean {
        return (url.startsWith("rtmp://", ignoreCase = true) ||
            url.startsWith("rtmpt://", ignoreCase = true)) &&
            !url.contains('\n') &&
            !url.contains('\r')
    }

    private fun redactedRtmpUrl(url: String): String {
        val schemeEnd = url.indexOf("://").takeIf { it > 0 } ?: return "rtmp://<redacted>"
        val hostStart = schemeEnd + 3
        val pathStart = url.indexOf('/', hostStart)
        val scheme = url.substring(0, schemeEnd)
        if (pathStart < 0) return "$scheme://${url.substring(hostStart).substringBefore('@').ifBlank { "<host>" }}/<redacted>"
        val authority = url.substring(hostStart, pathStart).substringAfter('@')
        val path = url.substring(pathStart + 1)
        val app = path.substringBefore('/').takeIf { it.isNotBlank() } ?: "<app>"
        return "$scheme://$authority/$app/<redacted>"
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

                appendStatus("Video source: staging \"$displayName\" and normalizing 30fps variants")
                val localFfmpegLibs = extractBundledFfmpegRuntime()
                val output = runRootChecked(
                    buildRuntimeSePolicyCommands() +
                        buildFfmpegRuntimeCommands(localFfmpegLibs) +
                        buildStageVideoCommands(stagedCopy),
                )
                persistSelectedVideoDisplayName(displayName)
                appendStatus("Video source: \"$displayName\" is ready\n$output")
                runOnUiThread {
                    persistSourceMode(SourceMode.FILE)
                    updateSourceModeControls(SourceMode.FILE)
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

    private fun extractBundledFfmpegRuntime(): List<File> {
        val localFfmpegLibs = FFMPEG_LIB_NAMES.mapNotNull { extractOptionalBundledNativeLib(it) }
        require(localFfmpegLibs.any { it.name == "libffmpeg.so" }) {
            "Missing bundled FFmpeg libffmpeg.so in APK"
        }
        require(localFfmpegLibs.any { it.name == FFMPEG_EXE_LIB_NAME }) {
            "Missing bundled FFmpeg executable $FFMPEG_EXE_LIB_NAME in APK"
        }
        return localFfmpegLibs
    }

    private fun buildRuntimeSePolicyCommands(): List<String> {
        return listOf(
            "mkdir -p ${shellQuote(RUNTIME_DIR)}",
            """
            |cat > ${shellQuote(RUNTIME_SEPOLICY_FILE)} <<'AWESOMECAM_SEPOLICY'
            |$RUNTIME_SEPOLICY_RULES
            |AWESOMECAM_SEPOLICY
            |chmod 0644 ${shellQuote(RUNTIME_SEPOLICY_FILE)} 2>/dev/null || true
            |chcon u:object_r:system_data_file:s0 ${shellQuote(RUNTIME_SEPOLICY_FILE)} 2>/dev/null || true
            """.trimMargin(),
            """
            ensure_awesomecam_sepolicy() {
              POLICY=${shellQuote(RUNTIME_SEPOLICY_FILE)}
              LOG=${shellQuote("$RUNTIME_DIR/ksud_sepolicy_apply.log")}
              rm -f "${'$'}LOG" 2>/dev/null || true

              run_ksud_policy() {
                KSUD_BIN="${'$'}1"
                if [ -z "${'$'}KSUD_BIN" ]; then
                  return 1
                fi
                echo "SELINUX_RUNTIME: checking policy with ${'$'}KSUD_BIN"
                if ! "${'$'}KSUD_BIN" sepolicy check "${'$'}POLICY" > "${'$'}LOG" 2>&1; then
                  echo "SELINUX_RUNTIME: ksud check failed for ${'$'}KSUD_BIN"
                  tail -n 8 "${'$'}LOG" 2>/dev/null || true
                  return 1
                fi
                echo "SELINUX_RUNTIME: applying policy with ${'$'}KSUD_BIN"
                if ! "${'$'}KSUD_BIN" sepolicy apply "${'$'}POLICY" > "${'$'}LOG" 2>&1; then
                  echo "SELINUX_RUNTIME: ksud apply failed for ${'$'}KSUD_BIN"
                  tail -n 8 "${'$'}LOG" 2>/dev/null || true
                  return 1
                fi
                echo "SELINUX_RUNTIME: applied via ${'$'}KSUD_BIN"
                return 0
              }

              KSUD_IN_PATH="$(command -v ksud 2>/dev/null || true)"
              if run_ksud_policy "${'$'}KSUD_IN_PATH"; then
                return 0
              fi

              KSUD_TMP=/data/local/tmp/awesomecam_ksud
              rm -f "${'$'}KSUD_TMP" 2>/dev/null || true
              if cp /data/adb/ksud "${'$'}KSUD_TMP" 2>> "${'$'}LOG"; then
                chmod 0755 "${'$'}KSUD_TMP" 2>/dev/null || true
                if run_ksud_policy "${'$'}KSUD_TMP"; then
                  return 0
                fi
              else
                echo "SELINUX_RUNTIME: unable to copy /data/adb/ksud to ${'$'}KSUD_TMP"
              fi

              if run_ksud_policy /data/adb/ksud; then
                return 0
              fi

              MAGISKPOLICY="$(command -v magiskpolicy 2>/dev/null || true)"
              if [ -n "${'$'}MAGISKPOLICY" ]; then
                echo "SELINUX_RUNTIME: applying policy with magiskpolicy fallback"
                if "${'$'}MAGISKPOLICY" --live --apply "${'$'}POLICY" > "${'$'}LOG" 2>&1; then
                  echo "SELINUX_RUNTIME: applied via ${'$'}MAGISKPOLICY"
                  return 0
                fi
                echo "SELINUX_RUNTIME: magiskpolicy fallback failed"
                tail -n 8 "${'$'}LOG" 2>/dev/null || true
              fi

              echo "ERROR: SELinux runtime policy apply failed; expected KernelSU ksud at /data/adb/ksud"
              echo "ERROR: see ${'$'}LOG"
              return 1
            }
            ensure_awesomecam_sepolicy || exit 1
            """.trimIndent(),
        )
    }

    private fun buildFfmpegRuntimeCommands(ffmpegLibs: List<File>): List<String> {
        val ffmpegExeSrc = ffmpegLibs.firstOrNull { it.name == FFMPEG_EXE_LIB_NAME }
            ?: error("Missing local FFmpeg executable $FFMPEG_EXE_LIB_NAME")
        val ffmpegExeDst = "$RUNTIME_DIR/$FFMPEG_BIN_NAME"
        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            for (lib in ffmpegLibs) {
                add("cp ${shellQuote(lib.absolutePath)} ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
            add("cp ${shellQuote(ffmpegExeSrc.absolutePath)} ${shellQuote(ffmpegExeDst)}")
            add("chmod 0755 ${shellQuote(ffmpegExeDst)}")
            for (lib in ffmpegLibs) {
                add("chmod 0644 ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(ffmpegExeDst)}")
            for (lib in ffmpegLibs) {
                add("chcon u:object_r:system_lib_file:s0 ${shellQuote("$RUNTIME_DIR/${lib.name}")}")
            }
        }
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
        val ffmpegExeDst = "$RUNTIME_DIR/$FFMPEG_BIN_NAME"
        val shadowHookDst = "$RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"
        val hookDst = "$RUNTIME_DIR/$HOOK_ASSET"

        return buildList {
            addAll(buildRuntimeSePolicyCommands())
            add("chcon u:object_r:system_data_file:s0 ${shellQuote(RUNTIME_DIR)} 2>/dev/null || true")
            add(
                """
                stage_file() {
                  SRC="${'$'}1"
                  DST="${'$'}2"
                  MODE="${'$'}3"
                  LABEL="${'$'}4"
                  TMP="${'$'}{DST}.tmp.${'$'}${'$'}"
                  rm -f "${'$'}TMP"
                  cp "${'$'}SRC" "${'$'}TMP" || return 1
                  chmod "${'$'}MODE" "${'$'}TMP" || return 1
                  chcon "${'$'}LABEL" "${'$'}TMP" 2>/dev/null || true
                  mv -f "${'$'}TMP" "${'$'}DST" || return 1
                  chmod "${'$'}MODE" "${'$'}DST" || return 1
                  chcon "${'$'}LABEL" "${'$'}DST" 2>/dev/null || true
                }
                """.trimIndent(),
            )
            add("stage_file ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)} 0755 u:object_r:system_lib_file:s0")
            add("stage_file ${shellQuote(player.absolutePath)} ${shellQuote(playerDst)} 0755 u:object_r:system_lib_file:s0")
            addAll(buildFfmpegRuntimeCommands(ffmpegLibs))
            add("stage_file ${shellQuote(shadowHook.absolutePath)} ${shellQuote(shadowHookDst)} 0644 u:object_r:system_lib_file:s0")
            add("stage_file ${shellQuote(hook.absolutePath)} ${shellQuote(hookDst)} 0644 u:object_r:system_lib_file:s0")
            add("chmod 0755 ${shellQuote(helperDst)} ${shellQuote(playerDst)} ${shellQuote(ffmpegExeDst)}")
            add("chmod 0644 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(playerDst)} ${shellQuote(ffmpegExeDst)} ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            add(
                """
                pkill -TERM -f ${shellQuote(playerDst)} 2>/dev/null || true
                OLDPID="$(pidof cameraserver 2>/dev/null | awk '{print ${'$'}1}')"
                if [ -n "${'$'}OLDPID" ]; then
                  echo "Restarting cameraserver to unload stale staged payload pid=${'$'}OLDPID"
                  kill -9 "${'$'}OLDPID" 2>/dev/null || true
                fi
                NEWPID=""
                for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
                  NEWPID="$(pidof cameraserver 2>/dev/null | awk '{print ${'$'}1}')"
                  if [ -n "${'$'}NEWPID" ] && [ "${'$'}NEWPID" != "${'$'}OLDPID" ]; then
                    break
                  fi
                  sleep 0.25
                done
                if [ -z "${'$'}NEWPID" ]; then
                  echo "ERROR: cameraserver PID not observed after refresh"
                  exit 1
                fi
                echo "cameraserver refreshed pid=${'$'}NEWPID"
                """.trimIndent(),
            )
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
        val finalDst = Mp4FeedController.STAGED_INPUT_PATH
        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            add("rm -f ${shellQuote(tmpDst)}")
            add("cp ${shellQuote(source.absolutePath)} ${shellQuote(tmpDst)}")
            add("chmod 0644 ${shellQuote(tmpDst)}")
            add("chcon u:object_r:awesomecam_source_file:s0 ${shellQuote(tmpDst)} 2>/dev/null || true")
            add("mv -f ${shellQuote(tmpDst)} ${shellQuote(finalDst)}")
            add("chmod 0644 ${shellQuote(finalDst)}")
            add("chcon u:object_r:awesomecam_source_file:s0 ${shellQuote(finalDst)} 2>/dev/null || true")
            add(buildPrescaleVariantCommand(finalDst))
            add("echo 'STAGED selected video and normalized prescaled variants for MediaCodec playback'")
        }
    }

    private fun buildPrescaleVariantCommand(inputPath: String): String {
        val variants = listOf("1440 1080", "1280 720", "1920 1080", "640 480")
        val calls = variants.joinToString("\n") { dims ->
            val parts = dims.split(' ')
            "make_variant ${parts[0]} ${parts[1]} || exit 1"
        }
        return """
            export LD_LIBRARY_PATH=${shellQuote(RUNTIME_DIR)}:${'$'}{LD_LIBRARY_PATH}
            FFMPEG=${shellQuote("$RUNTIME_DIR/$FFMPEG_BIN_NAME")}
            SRC=${shellQuote(inputPath)}
            if [ ! -x "${'$'}FFMPEG" ]; then
              echo "ERROR: missing executable ${'$'}FFMPEG; inject/stage runtime first"
              exit 1
            fi
            if [ ! -r "${'$'}SRC" ]; then
              echo "ERROR: missing staged input ${'$'}SRC"
              exit 1
            fi
            ENCODE_TIMEOUT=8
            MANIFEST="$RUNTIME_DIR/input_normalization.txt"
            rm -f "${'$'}MANIFEST"
            echo "source=${'$'}SRC" > "${'$'}MANIFEST"
            echo "fps=30 pix_fmt=yuv420p audio=disabled filter=center-crop" >> "${'$'}MANIFEST"
            rm -f ${shellQuote("$RUNTIME_DIR/input_1440x1080.mp4")} ${shellQuote("$RUNTIME_DIR/input_1280x720.mp4")} ${shellQuote("$RUNTIME_DIR/input_1920x1080.mp4")} ${shellQuote("$RUNTIME_DIR/input_640x480.mp4")}
            make_variant() {
              W="${'$'}1"
              H="${'$'}2"
              OUT="$RUNTIME_DIR/input_${'$'}{W}x${'$'}{H}.mp4"
              TMP="$RUNTIME_DIR/.input_${'$'}{W}x${'$'}{H}.mp4.tmp"
              FILTER="scale=${'$'}{W}:${'$'}{H}:force_original_aspect_ratio=increase,crop=${'$'}{W}:${'$'}{H},setsar=1"
              rm -f "${'$'}TMP" "${'$'}OUT"
              for ENC in h264_mediacodec libx264 mpeg4; do
                LOG="$RUNTIME_DIR/ffmpeg_prescale_${'$'}{W}x${'$'}{H}_${'$'}{ENC}.log"
                rm -f "${'$'}TMP"
                echo "PRESCALE ${'$'}{W}x${'$'}{H}: trying encoder=${'$'}ENC"
                if [ "${'$'}ENC" = "h264_mediacodec" ]; then
                  timeout "${'$'}ENCODE_TIMEOUT" "${'$'}FFMPEG" -hide_banner -nostdin -y -i "${'$'}SRC" -map 0:v:0 -vf "${'$'}FILTER" -an -fps_mode cfr -r 30 -c:v h264_mediacodec -pix_fmt yuv420p -b:v 8000000 -g 30 -bf 0 -movflags +faststart -f mp4 "${'$'}TMP" > "${'$'}LOG" 2>&1
                elif [ "${'$'}ENC" = "libx264" ]; then
                  timeout "${'$'}ENCODE_TIMEOUT" "${'$'}FFMPEG" -hide_banner -nostdin -y -i "${'$'}SRC" -map 0:v:0 -vf "${'$'}FILTER" -an -fps_mode cfr -r 30 -c:v libx264 -preset veryfast -tune zerolatency -profile:v baseline -level 4.1 -g 30 -keyint_min 30 -sc_threshold 0 -bf 0 -pix_fmt yuv420p -movflags +faststart -f mp4 "${'$'}TMP" > "${'$'}LOG" 2>&1
                else
                  timeout "${'$'}ENCODE_TIMEOUT" "${'$'}FFMPEG" -hide_banner -nostdin -y -i "${'$'}SRC" -map 0:v:0 -vf "${'$'}FILTER" -an -fps_mode cfr -r 30 -c:v mpeg4 -q:v 4 -g 30 -bf 0 -pix_fmt yuv420p -movflags +faststart -f mp4 "${'$'}TMP" > "${'$'}LOG" 2>&1
                fi
                RC="${'$'}?"
                if [ "${'$'}RC" -eq 0 ] && [ -s "${'$'}TMP" ]; then
                  mv -f "${'$'}TMP" "${'$'}OUT"
                  chmod 0644 "${'$'}OUT"
                  chcon u:object_r:awesomecam_source_file:s0 "${'$'}OUT" 2>/dev/null || true
                  echo "variant=${'$'}{W}x${'$'}{H} encoder=${'$'}ENC output=${'$'}OUT" >> "${'$'}MANIFEST"
                  echo "PRESCALED ${'$'}{W}x${'$'}{H}: encoder=${'$'}ENC out=${'$'}OUT"
                  return 0
                fi
                echo "PRESCALE ${'$'}{W}x${'$'}{H}: encoder=${'$'}ENC failed rc=${'$'}RC"
                tail -n 8 "${'$'}LOG" 2>/dev/null || true
              done
              rm -f "${'$'}TMP"
              echo "ERROR: failed to prescale ${'$'}{W}x${'$'}{H}"
              return 1
            }
            $calls
            chmod 0644 "${'$'}MANIFEST"
            chcon u:object_r:awesomecam_source_file:s0 "${'$'}MANIFEST" 2>/dev/null || true
        """.trimIndent()
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

    private fun runRootChecked(commands: List<String>): String {
        val output = runRoot(commands)
        if (!output.startsWith("exit=0\n")) {
            error("Root command failed\n$output")
        }
        return output
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
        private const val FFMPEG_BIN_NAME = "ffmpeg"
        private const val FFMPEG_EXE_LIB_NAME = "libffmpegexe.so"
        private const val RUNTIME_SEPOLICY_FILE = "/data/camera/awesomecam_runtime_sepolicy.rule"
        private const val PREFS_NAME = "video_source"
        private const val PREF_SELECTED_VIDEO_NAME = "selected_video_display_name"
        private const val PREF_SOURCE_MODE = "selected_source_mode"
        private const val PREF_RTMP_URL = "rtmp_url"
        private val FFMPEG_LIB_NAMES = listOf(
            "libffmpeg.so",
            "libffmpegexe.so",
            "libavformat.so",
            "libavcodec.so",
            "libavutil.so",
            "libswscale.so",
            "libswresample.so",
        )
        private val RUNTIME_SEPOLICY_RULES = """
            type awesomecam_source_file { file_type mlstrustedobject }
            type awesomecam_config_file { file_type mlstrustedobject }
            allow su cameraserver process { ptrace getattr sigchld signal sigkill }
            allow su system_data_root_file dir search
            allow su system_data_file dir { search open read getattr write add_name remove_name }
            allow su system_data_file file { create open read write getattr setattr append map unlink rename ioctl lock }
            allow su system_lib_file file { create open read write getattr setattr append map unlink rename ioctl lock execute }
            allow su awesomecam_source_file file { open read getattr map ioctl lock }
            allow su awesomecam_config_file file { create open read write getattr setattr append map ioctl lock }
            allow cameraserver system_data_root_file dir search
            allow cameraserver system_data_file dir { search open read getattr }
            allow cameraserver system_lib_file file { open read getattr map execute }
            allow cameraserver cameraserver process execmem
            allow cameraserver default_android_service service_manager add
            allow untrusted_app default_android_service service_manager find
            allow cameraserver untrusted_app fd use
            allow cameraserver awesomecam_source_file file { open read getattr map ioctl lock }
            allow cameraserver awesomecam_config_file file { create open read write getattr setattr append map }
            allow cameraserver mediaextractor_service service_manager find
            allow cameraserver mediaextractor binder { call transfer }
            allow mediaextractor cameraserver binder transfer
            allow mediaextractor cameraserver fd use
            allow mediaextractor system_data_root_file dir search
            allow mediaextractor system_data_file dir { search open read getattr }
            allow mediaextractor awesomecam_source_file file { open read getattr map ioctl lock watch watch_reads }
            allow untrusted_app system_data_root_file dir search
            allow untrusted_app system_data_file dir { search open read getattr }
            allow untrusted_app awesomecam_source_file file { open read getattr map ioctl lock }
            allow su default_android_service service_manager find
            allow su cameraserver binder { call transfer }
            allow cameraserver su fd use
        """.trimIndent()
    }

    private enum class SourceMode(val value: String) {
        FILE("file"),
        RTMP("rtmp");

        companion object {
            fun fromValue(value: String?): SourceMode {
                return values().firstOrNull { it.value == value } ?: FILE
            }
        }
    }
}
