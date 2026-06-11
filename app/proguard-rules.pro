# Keep the native JNI bridge (also covered by the library's consumer rules).
-keep class com.salt.usb.audio.UsbAudioOutput { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
