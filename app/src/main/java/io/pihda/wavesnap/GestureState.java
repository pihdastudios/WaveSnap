package io.pihda.wavesnap;

/** Integer states avoid per-transition allocation on the legacy runtime. */
public final class GestureState {
  public static final int INITIALIZING = 0;
  public static final int ARMED = 1;
  public static final int TRACKING = 2;
  public static final int COUNTDOWN = 3;
  public static final int AUTOFOCUSING = 4;
  public static final int CAPTURING = 5;
  public static final int COOLDOWN = 6;
  public static final int STOPPED = 7;
  public static final int ERROR = 8;

  public static String name(int state) {
    switch (state) {
      case INITIALIZING:
        return "INITIALIZING";
      case ARMED:
        return "ARMED";
      case TRACKING:
        return "TRACKING";
      case COUNTDOWN:
        return "COUNTDOWN";
      case AUTOFOCUSING:
        return "AUTOFOCUSING";
      case CAPTURING:
        return "CAPTURING";
      case COOLDOWN:
        return "COOLDOWN";
      case STOPPED:
        return "STOPPED";
      case ERROR:
        return "ERROR";
      default:
        return "UNKNOWN(" + state + ")";
    }
  }

  private GestureState() {}
}
