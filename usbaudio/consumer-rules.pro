# Keep the JNI bridge class and its native methods. The native library looks
# these up by their fully-qualified name, so they must not be renamed/removed.
-keep class com.salt.usb.audio.UsbAudioOutput { *; }
-keepclasseswithmembernames class * {
    native <methods>;
}
