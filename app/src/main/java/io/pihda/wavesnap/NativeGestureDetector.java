package io.pihda.wavesnap;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.ByteBuffer;

/** Small guarded JNI boundary shared by all detector stages. */
public final class NativeGestureDetector {
  public static final int RESULT_NONE = 0;
  public static final int RESULT_MOTION = 1;
  public static final int RESULT_WAVE = 2;
  public static final int RESULT_MASK = 0x03;
  public static final int MEAN_LUMA_SHIFT = 4;
  public static final int MEAN_LUMA_MASK = 0xff;
  public static final int MOTION_PERMILLE_SHIFT = 12;
  public static final int GLOBAL_PERMILLE_SHIFT = 22;
  public static final int CENTROID_X_SHIFT = 32;
  public static final int CENTROID_Y_SHIFT = 40;
  public static final int AREA_PERMILLE_SHIFT = 48;
  public static final int FLAGS_SHIFT = 58;

  public static final int FLAG_GLOBAL_MOTION = 1;
  public static final int FLAG_EXPOSURE_CHANGE = 2;
  public static final int FLAG_COMPONENT_TOO_LARGE = 4;
  public static final int FLAG_NO_PLAUSIBLE_COMPONENT = 8;
  public static final int FLAG_TRAJECTORY_REJECTED = 16;
  public static final int FLAG_CAMERA_TRANSLATION = 32;
  public static final int FLAG_LOCAL_FLOW = 64;

  public static final int ERROR_NOT_INITIALIZED = -1;
  public static final int ERROR_INVALID_ARGUMENT = -2;
  public static final int ERROR_JPEG_DECODE = -3;
  public static final int ERROR_JPEG_DIMENSIONS = -4;
  public static final int ERROR_LIBRARY_UNAVAILABLE = -5;

  private static final boolean libraryLoaded;

  static {
    boolean loaded = false;
    try {
      System.loadLibrary("gesture_detector");
      loaded = true;
      Logger.info("NativeGesture: libgesture_detector loaded");
    } catch (Throwable throwable) {
      logFailure("load native library", throwable);
    }
    libraryLoaded = loaded;
  }

  public static boolean initialize(
      int sourceWidth, int sourceHeight, int outputWidth, int outputHeight) {
    if (!libraryLoaded) {
      return false;
    }
    try {
      return nativeInitialize(sourceWidth, sourceHeight, outputWidth, outputHeight);
    } catch (Throwable throwable) {
      logFailure("initialize", throwable);
      return false;
    }
  }

  public static long process(ByteBuffer jpegBuffer, int jpegLength, long timestampMs) {
    if (!libraryLoaded) {
      return ERROR_LIBRARY_UNAVAILABLE;
    }
    if (jpegBuffer == null || !jpegBuffer.isDirect() || jpegLength <= 0
        || jpegLength > jpegBuffer.capacity()) {
      return ERROR_INVALID_ARGUMENT;
    }
    try {
      return nativeProcess(jpegBuffer, jpegLength, timestampMs);
    } catch (Throwable throwable) {
      logFailure("process frame", throwable);
      return ERROR_LIBRARY_UNAVAILABLE;
    }
  }

  public static void reset() {
    if (!libraryLoaded) {
      return;
    }
    try {
      nativeReset();
    } catch (Throwable throwable) {
      logFailure("reset", throwable);
    }
  }

  public static void destroy() {
    if (!libraryLoaded) {
      return;
    }
    try {
      nativeDestroy();
    } catch (Throwable throwable) {
      logFailure("destroy", throwable);
    }
  }

  public static int getResult(long packedResult) {
    return packedResult < 0 ? (int) packedResult : (int) packedResult & RESULT_MASK;
  }

  public static int getMeanLuminance(long packedResult) {
    return packedResult < 0 ? -1 : (int) ((packedResult >> MEAN_LUMA_SHIFT) & MEAN_LUMA_MASK);
  }

  public static int getMotionPermille(long packedResult) {
    return packedResult < 0 ? -1 : (int) ((packedResult >> MOTION_PERMILLE_SHIFT) & 0x3ff);
  }

  public static int getGlobalMotionPermille(long packedResult) {
    return packedResult < 0 ? -1 : (int) ((packedResult >> GLOBAL_PERMILLE_SHIFT) & 0x3ff);
  }

  public static int getCentroidX(long packedResult) {
    if ((getFlags(packedResult) & (FLAG_CAMERA_TRANSLATION | FLAG_LOCAL_FLOW)) != 0) {
      return -1;
    }
    int value = packedResult < 0 ? 255 : (int) ((packedResult >> CENTROID_X_SHIFT) & 0xff);
    return value == 255 ? -1 : value;
  }

  public static int getCentroidY(long packedResult) {
    if ((getFlags(packedResult) & (FLAG_CAMERA_TRANSLATION | FLAG_LOCAL_FLOW)) != 0) {
      return -1;
    }
    int value = packedResult < 0 ? 255 : (int) ((packedResult >> CENTROID_Y_SHIFT) & 0xff);
    return value == 255 ? -1 : value;
  }

  public static int getCandidateAreaPermille(long packedResult) {
    return packedResult < 0 ? -1 : (int) ((packedResult >> AREA_PERMILLE_SHIFT) & 0x3ff);
  }

  public static int getFlags(long packedResult) {
    if (packedResult < 0) {
      return 0;
    }
    int flags = (int) ((packedResult >> FLAGS_SHIFT) & 0x1f);
    if ((packedResult & 0x4) != 0) {
      flags |= FLAG_CAMERA_TRANSLATION;
    }
    if ((packedResult & 0x8) != 0) {
      flags |= FLAG_LOCAL_FLOW;
    }
    return flags;
  }

  public static int getCameraShiftX(long packedResult) {
    return (getFlags(packedResult) & FLAG_CAMERA_TRANSLATION) == 0
        ? 0
        : (int) ((packedResult >> CENTROID_X_SHIFT) & 0xff) - 16;
  }

  public static int getCameraShiftY(long packedResult) {
    return (getFlags(packedResult) & FLAG_CAMERA_TRANSLATION) == 0
        ? 0
        : (int) ((packedResult >> CENTROID_Y_SHIFT) & 0xff) - 16;
  }

  public static int getLocalFlowX(long packedResult) {
    return (getFlags(packedResult) & FLAG_LOCAL_FLOW) == 0
        ? 0
        : (int) ((packedResult >> CENTROID_X_SHIFT) & 0xff) - 32;
  }

  public static int getLocalFlowBlocks(long packedResult) {
    return (getFlags(packedResult) & FLAG_LOCAL_FLOW) == 0
        ? 0
        : (int) ((packedResult >> CENTROID_Y_SHIFT) & 0xff);
  }

  private static native boolean nativeInitialize(
      int sourceWidth, int sourceHeight, int outputWidth, int outputHeight);
  private static native long nativeProcess(ByteBuffer jpegBuffer, int jpegLength, long timestampMs);
  private static native void nativeReset();
  private static native void nativeDestroy();

  private static void logFailure(String operation, Throwable throwable) {
    StringWriter writer = new StringWriter();
    PrintWriter printer = new PrintWriter(writer);
    throwable.printStackTrace(printer);
    printer.close();
    Logger.error("NativeGesture: operation=" + operation
        + " exception=" + throwable.getClass().getName() + " message=" + throwable.getMessage()
        + "\n" + writer.toString());
  }

  private NativeGestureDetector() {}
}
