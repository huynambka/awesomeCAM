package com.namnh.awesomecam

object VideoBridge {
    init {
        System.loadLibrary("streamclient")
    }

    external fun nativeConnect(): Boolean
    external fun nativeStartFeed(path: String): Boolean
    external fun nativeStopFeed()
    external fun nativeIsFeedRunning(): Boolean
    external fun nativeClear(): Boolean

    fun connect(): Boolean = nativeConnect()
    fun startFeed(path: String): Boolean = nativeStartFeed(path)
    fun stopFeed() = nativeStopFeed()
    fun isFeedRunning(): Boolean = nativeIsFeedRunning()
    fun clear(): Boolean = nativeClear()
}
