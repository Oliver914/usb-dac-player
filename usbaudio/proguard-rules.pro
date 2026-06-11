# Keep JNI entry points used by the native usb_audio_output library.
-keep class com.salt.usb.audio.UsbAudioOutput { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
