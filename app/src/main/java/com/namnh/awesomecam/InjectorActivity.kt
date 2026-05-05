package com.namnh.awesomecam

import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
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

    private val listener = object : TelemetryStore.Listener {
        override fun onTelemetryChanged(snapshot: TelemetryStore.Snapshot) {
            runOnUiThread {
                summaryText.text = buildSummary(snapshot.status)
                summaryScroll.post {
                    summaryScroll.fullScroll(NestedScrollView.FOCUS_DOWN)
                }
                feedButton.text = getString(
                    if (feedController.isRunning) R.string.feed_button_stop else R.string.feed_button_start,
                )
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

        val buildIdentity = buildIdentityLabel()
        buildTitleText.text = buildIdentity
        title = buildIdentity

        TelemetryStore.setInitialStatusIfEmpty(
            "Build: $buildIdentity\n\nRequired assets: $HELPER_ASSET, $SHADOWHOOK_LIB_NAME, $HOOK_ASSET\n" +
                "Stage 1: $RUNTIME_DIR/$SHADOWHOOK_LIB_NAME\n" +
                "Stage 2: $RUNTIME_DIR/$HOOK_ASSET (call main_hook)\n\n" +
                "File source: ${Mp4FeedController.DEFAULT_VIDEO_PATH}",
        )

        findViewById<Button>(R.id.injectButton).setOnClickListener {
            runAsync {
                appendStatus("Preparing runtime files")
                val localHelper = extractAsset(HELPER_ASSET)
                // libhook.so must come from APK native lib/, not assets/.
                // AGP packages assets before CMake refreshes app/src/main/assets/libhook.so,
                // so assets/libhook.so can be one build stale and crash cameraserver.
                val localHook = extractBundledNativeLib(HOOK_ASSET)
                val localShadowHook = extractBundledNativeLib(SHADOWHOOK_LIB_NAME)

                appendStatus(
                    "App-private staging ready:\n" +
                        (listOf(localHelper.absolutePath, localHook.absolutePath) +
                            listOf(localShadowHook.absolutePath))
                            .joinToString("\n"),
                )
                val commands = buildRuntimeCommands(localHelper, localShadowHook, localHook)

                appendStatus("Running injector as root")
                appendStatus(runRoot(commands))
            }
        }

        feedButton.setOnClickListener {
            if (feedController.isRunning) {
                appendStatus("Stopping MP4 feed")
                feedController.stop()
                feedButton.text = getString(R.string.feed_button_start)
            } else {
                appendStatus("Starting MP4 feed from ${Mp4FeedController.DEFAULT_VIDEO_PATH}")
                feedController.start(Mp4FeedController.DEFAULT_VIDEO_PATH)
                feedButton.text = getString(R.string.feed_button_stop)
            }
        }

        findViewById<Button>(R.id.resetButton).setOnClickListener {
            runAsync {
                appendStatus("Restarting cameraserver")
                feedController.stop()
                appendStatus(runRoot(listOf("pkill cameraserver || killall cameraserver || true")))
                runOnUiThread { feedButton.text = getString(R.string.feed_button_start) }
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
    }

    override fun onStop() {
        TelemetryStore.removeListener(listener)
        TelemetryStore.releaseLogcat()
        super.onStop()
    }

    override fun onDestroy() {
        feedController.stop()
        super.onDestroy()
    }

    private fun buildIdentityLabel(): String {
        return "awesomeCAM ${BuildConfig.VERSION_NAME} · ${BuildConfig.ARCH_LABEL} · ${BuildConfig.GIT_SHA} · ${BuildConfig.BUILD_STAMP}"
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
                ?: error("Missing bundled native lib in APK: $libName")

            zip.getInputStream(entry).use { input ->
                outFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }
        }

        return outFile
    }

    private fun buildRuntimeCommands(helper: File, shadowHook: File, hook: File): List<String> {
        val helperDst = "$RUNTIME_DIR/$HELPER_ASSET"
        val shadowHookDst = "$RUNTIME_DIR/$SHADOWHOOK_LIB_NAME"
        val hookDst = "$RUNTIME_DIR/$HOOK_ASSET"
        val offsetConfigDst = "$RUNTIME_DIR/$OFFSET_CONFIG_NAME"

        return buildList {
            add("mkdir -p ${shellQuote(RUNTIME_DIR)}")
            add("touch ${shellQuote(offsetConfigDst)}")
            add("chmod 0666 ${shellQuote(offsetConfigDst)}")
            add("chcon u:object_r:awesomecam_config_file:s0 ${shellQuote(offsetConfigDst)} || true")
            add("cp ${shellQuote(helper.absolutePath)} ${shellQuote(helperDst)}")
            add("cp ${shellQuote(shadowHook.absolutePath)} ${shellQuote(shadowHookDst)}")
            add("cp ${shellQuote(hook.absolutePath)} ${shellQuote(hookDst)}")
            add("chmod 0755 ${shellQuote(helperDst)}")
            add("chmod 0644 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
            add("chcon u:object_r:system_lib_file:s0 ${shellQuote(shadowHookDst)} ${shellQuote(hookDst)}")
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
        private const val HOOK_ASSET = "libhook.so"
        private const val SHADOWHOOK_LIB_NAME = "libshadowhook.so"
        private const val OFFSET_CONFIG_NAME = "awesomecam_offsets.conf"
    }
}
