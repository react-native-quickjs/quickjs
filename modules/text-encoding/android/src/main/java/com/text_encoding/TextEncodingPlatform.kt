package com.text_encoding

import android.content.Context

/**
 * The Android half of the platform seam.
 *
 * Implement whatever the shared C++ needs from Android here — locale data via
 * android.icu, secure random via KeyStore, content resolution, and so on — and
 * call it from TextEncodingPlatform.cpp over JNI.
 *
 * Keep this surface small and coarse-grained. Every JNI crossing is expensive
 * relative to the C++ around it, so one call returning a batch beats many calls
 * returning scalars.
 */
object TextEncodingPlatform {
  private var appContext: Context? = null

  init {
    System.loadLibrary("text_encoding_platform")
  }

  @JvmStatic
  fun attach(context: Context) {
    appContext = context
    nativeAttach()
  }

  /** Example: replace with something your module actually needs. */
  @JvmStatic
  fun deviceLocale(): String =
    java.util.Locale.getDefault().toLanguageTag()

  private external fun nativeAttach()
}
