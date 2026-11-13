# Rules every app that packages :corehost inherits (R8).

# JNI: the native library binds these methods by name (Java_com_retronest_bridge_LibretroBridge_*) and
# looks classes up by name, so the bridge must never be renamed, shrunk or have members stripped.
-keep class com.retronest.bridge.LibretroBridge { *; }
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# The service is started by class name from the app (CoreHostProtocol.HOST_SERVICE_CLASS) and declared in the
# manifest: it must keep its name.
-keep class com.retronest.corehost.engine.CoreHostService { *; }
-keep class com.retronest.corehost.engine.CoreHostServiceAlt { *; }
