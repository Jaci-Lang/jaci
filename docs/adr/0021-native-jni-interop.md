# ADR 0021: Native JNI In-Process Interop & Type Conversion Engine

## Context

Many enterprise, data science, and systems programming workloads depend on Java libraries, JDBC database drivers, Apache Spark/Hadoop ecosystems, JVM machine learning models, and Android platform APIs. Existing scripting approaches frequently rely on external Java child processes communicating across JSON-RPC or REST protocols, which suffer from substantial latency, serialization overhead, process isolation complexities, and lack of direct memory sharing.

Similar to Julia's high-performance [JavaCall.jl](https://github.com/JuliaInterop/JavaCall.jl), Jaci requires a native in-process Java Native Interface (`jni`) subsystem that loads the JVM shared library (`libjvm.so` / `libjvm.dylib` / `jvm.dll`) directly into the Jaci runtime process, manages the JVM lifecycle, and provides seamless zero-copy type translation between Luau and Java.

## Decision

Implement a comprehensive, self-contained native JNI interop library (`jni`) built directly into the Jaci VM with zero external compile-time JDK header dependencies.

### 1. Multiplatform Dynamic JVM Library Discovery & Lifecycle
- `jni.find_jvm_path()`: Intelligent auto-detection of JVM dynamic libraries across Linux (`/usr/lib/jvm/**/libjvm.so`), macOS (`/Library/Java/JavaVirtualMachines/**/libjvm.dylib`), and Windows (`JAVA_HOME/bin/server/jvm.dll` or registry keys).
- `jni.init(config?)`: Dynamically loads the JVM library via `dlopen`/`LoadLibraryA`, resolves `JNI_CreateJavaVM` and `JNI_GetCreatedJavaVMs`, sets user-defined `-Djava.class.path=` options and arbitrary JVM flags, attaches the current thread, and preserves the C numeric locale (`setlocale(LC_NUMERIC, "C")`).
- `jni.is_initialized()`, `jni.get_version()`, `jni.destroy()`, `jni.attach_current_thread()`, `jni.detach_current_thread()`.
- `jni.with_local_frame([capacity], fn)` / `jni.local_frame([capacity], fn)`: High-efficiency JNI local frame scoping with `PushLocalFrame` / `PopLocalFrame` preventing local reference table overflow in tight loops.

### 2. Standalone JNI C/C++ Header Definitions (`VM/src/ljni.h`)
- Standalone, binary-accurate JNI type and interface definitions (`jboolean`, `jbyte`, `jchar`, `jshort`, `jint`, `jlong`, `jfloat`, `jdouble`, `jobject`, `jclass`, `jarray`, `jvalue`, `jfieldID`, `jmethodID`, `JNINativeInterface_`, `JNIInvokeInterface_`, `JNIEnv`, `JavaVM`).
- Allows Jaci to build seamlessly on systems without a JDK installed at compile time, while running against any modern JVM (Java 8 through Java 26+) at runtime.

### 3. Rich Userdata Metatypes & Ergonomic Object System
- **`JClass` (`jni.find_class` / `jni.import`)**:
  - `Class:new(...)` or `Class(...)`: Dispatches to constructor overloads based on argument types.
  - `Class.staticField`: Read and write static fields directly.
  - `Class.staticMethod(...)`: Dispatches to static method overloads with automatic parameter conversion.
  - Built-in reflection queries: `:getName()`, `:getSimpleName()`, `:getSuperclass()`, `:getInterfaces()`, `:isInterface()`, `:isArray()`, `:isPrimitive()`, `:getMethods()`, `:getFields()`, `:getConstructors()`.
- **`JObject`**:
  - `obj:method(...)`: Direct invocation of instance methods using reflection-based overload scoring with direct `JNIEnv` method call acceleration (`GetMethodID` + `Call<Type>MethodA`).
  - `obj.field`: Instance field get and set.
  - `__tostring`: Invokes Java `toString()`.
  - `__eq`: Invokes Java `equals(Object)`.
  - `__gc`: Releases underlying JNI global reference via `env->DeleteGlobalRef`.
- **`JArray` (`jni.array(type, size_or_table)`)**:
  - Direct support for primitive arrays (`boolean[]`, `byte[]`, `char[]`, `short[]`, `int[]`, `long[]`, `float[]`, `double[]`) and object arrays (`Object[]`, `String[]`, etc.).
  - 1-based indexing: `arr[i]` reads and `arr[i] = val` writes elements.
  - `#arr`: Returns array length.
  - `arr:to_table()`: Converts elements to a native Luau table.
  - `arr:to_buffer()`: Converts byte/primitive array to a native Luau `buffer`.
- **`JTypedValue`**:
  - Explicit typed value constructors (`jni.jboolean`, `jni.jbyte`, `jni.jchar`, `jni.jshort`, `jni.jint`, `jni.jlong`, `jni.jfloat`, `jni.jdouble`, `jni.jstring`, `jni.null`) for unambiguous overload selection.

### 4. Zero-Copy Buffer Interoperability
- `jni.wrap_buffer(buf)`: Wraps a Luau `buffer` with `env->NewDirectByteBuffer`, creating a `java.nio.DirectByteBuffer` sharing the exact memory address without memory copying.

### 5. Collections Conversion & Deep Serialization
- `jni.to_java(val)`: Recursively converts Luau arrays to `java.util.ArrayList`, Luau dictionaries to `java.util.HashMap`, primitives to boxed Java objects.
- `jni.to_luau(obj)`: Recursively converts Java `List` collections to Luau array tables and `Map` collections to Luau dictionary tables.

### 6. Java Exception Interception
- Intercepts pending JNI exceptions with `env->ExceptionOccurred()`, captures the full formatted Java stack trace via `StringWriter` and `PrintWriter`, clears the JVM exception flag, and re-raises the error as a standard Luau catchable error (`pcall`).

### 7. Direct Signature Invocations
- `jni.call_method(obj, name, sig, ...args)` and `jni.call_static(class, name, sig, ...args)` for low-level, high-throughput method invocation bypassing reflection scoring.

## Consequences

- Direct, in-process interoperability with the entire Java and JVM ecosystem.
- No external IPC or JSON-RPC serialization bottleneck.
- Zero compile-time JDK dependency for building Jaci binaries.
- Safe GC integration preventing memory leaks through global reference tracking.
- Complete compatibility across Linux, macOS, and Windows.
