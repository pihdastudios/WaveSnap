package io.pihda.wavesnap;

import android.hardware.Camera;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.TextView;
import com.sony.scalar.hardware.CameraEx;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.ByteBuffer;

/**
 * WaveSnap's gesture-triggered camera activity.
 */
public class GestureCameraActivity
    extends BaseActivity implements SurfaceHolder.Callback, CameraSequenceFrameSource.Listener,
                                    CameraSequenceFrameSource.FrameConsumer {
  /* PicoJPEG reduced mode yields one useful luminance value per 8x8 block. */
  private static final int DETECTOR_WIDTH = 80;
  private static final int DETECTOR_HEIGHT = 60;
  private static final long COUNTDOWN_STEP_MS = 1000;
  private static final long COOLDOWN_MS = 3000;
  private static final long AUTOMATIC_SHUTTER_RELEASE_MS = 400;
  private static final long STILLNESS_REQUIRED_MS = 1000;
  private static final int STILL_MOTION_PERMILLE = 20;
  private static final int STILL_GLOBAL_PERMILLE = 35;
  private final Object cameraLock = new Object();
  private final Object stateLock = new Object();

  private SurfaceHolder surfaceHolder;
  private TextView countdownView;
  private TextView statusView;
  private Handler mainHandler;
  private CameraEx cameraEx;
  private CameraSequenceFrameSource frameSource;
  private boolean resumed;
  private boolean previewStarted;
  private boolean autoPowerOffChanged;
  private volatile boolean nativeReady;
  private boolean nativeInitialized;
  private int nativeFrameCount;
  private long nativeTotalProcessMs;
  private long lastMotionDiagnosticMs;
  private volatile int gestureState;
  private int sequenceToken;
  private long countdownStartMs;
  private long cooldownDeadlineMs;
  private long stillSinceMs;
  private Runnable countdownTwoRunnable;
  private Runnable countdownOneRunnable;
  private Runnable captureRunnable;
  private Runnable automaticShutterReleaseRunnable;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    Logger.installUncaughtExceptionHandler();
    super.onCreate(savedInstanceState);
    setContentView(R.layout.activity_gesture_camera);

    SurfaceView surfaceView = (SurfaceView) findViewById(R.id.surfaceView);
    surfaceHolder = surfaceView.getHolder();
    surfaceHolder.setType(SurfaceHolder.SURFACE_TYPE_PUSH_BUFFERS);

    countdownView = (TextView) findViewById(R.id.gestureCountdown);
    statusView = (TextView) findViewById(R.id.gestureStatus);
    mainHandler = new Handler();
    countdownView.setVisibility(View.GONE);
    setStatus(R.string.gesture_status_initializing);
    nativeFrameCount = 0;
    nativeTotalProcessMs = 0;
    lastMotionDiagnosticMs = 0;
    gestureState = GestureState.STOPPED;
    sequenceToken = 0;
  }

  @Override
  protected void onResume() {
    super.onResume();
    Logger.info("GestureCamera: onResume begin");
    Logger.info("GestureCamera: device model=" + Build.MODEL + " product=" + Build.PRODUCT
        + " display=" + Build.DISPLAY + " firmware=" + Build.VERSION.INCREMENTAL);

    synchronized (cameraLock) {
      resumed = true;
      previewStarted = false;
    }
    synchronized (stateLock) {
      gestureState = GestureState.INITIALIZING;
      sequenceToken++;
      cooldownDeadlineMs = 0;
      stillSinceMs = 0;
    }
    nativeFrameCount = 0;
    nativeTotalProcessMs = 0;
    lastMotionDiagnosticMs = 0;
    setStatus(R.string.gesture_status_initializing);
    countdownView.setVisibility(View.GONE);

    try {
      setAutoPowerOffMode(false);
      autoPowerOffChanged = true;
      Logger.info("GestureCamera: automatic power-off disabled");
    } catch (RuntimeException e) {
      logFailure("disable automatic power-off", e);
    }

    try {
      CameraEx openedCamera = CameraEx.open(0, null);
      synchronized (cameraLock) {
        if (resumed) {
          cameraEx = openedCamera;
          openedCamera = null;
        }
      }
      if (openedCamera != null) {
        try {
          openedCamera.release();
        } catch (RuntimeException e) {
          logFailure("release camera opened during pause", e);
        }
      }
      Logger.info("GestureCamera: CameraEx.open completed");
    } catch (RuntimeException e) {
      logFailure("CameraEx.open", e);
      setStatus(R.string.gesture_status_camera_unavailable);
    }

    nativeReady = NativeGestureDetector.initialize(CameraSequenceFrameSource.SOURCE_WIDTH,
        CameraSequenceFrameSource.SOURCE_HEIGHT, DETECTOR_WIDTH, DETECTOR_HEIGHT);
    nativeInitialized = nativeReady;
    if (nativeReady) {
      Logger.info("GestureCamera: native detector initialized source="
          + CameraSequenceFrameSource.SOURCE_WIDTH + "x" + CameraSequenceFrameSource.SOURCE_HEIGHT
          + " output=" + DETECTOR_WIDTH + "x" + DETECTOR_HEIGHT);
    } else {
      Logger.error("GestureCamera: native detector initialization failed");
    }

    surfaceHolder.addCallback(this);
    Logger.info("GestureCamera: surface callback registered");
  }

  @Override
  protected void onPause() {
    Logger.info("GestureCamera: onPause begin");

    CameraEx cameraToRelease;
    CameraSequenceFrameSource sourceToStop;
    synchronized (stateLock) {
      gestureState = GestureState.STOPPED;
      sequenceToken++;
      stillSinceMs = 0;
    }
    synchronized (cameraLock) {
      resumed = false;
      previewStarted = false;
      sourceToStop = frameSource;
      frameSource = null;
      cameraToRelease = cameraEx;
      cameraEx = null;
    }
    nativeReady = false;
    cancelCountdownCallbacks();
    cancelAutomaticShutterRelease();
    mainHandler.removeCallbacksAndMessages(null);

    boolean workerStopped = true;
    if (sourceToStop != null) {
      workerStopped = sourceToStop.stopAndJoin(2000);
      if (!workerStopped) {
        Logger.error("GestureCamera: frame worker did not stop within first bounded wait");
        sourceToStop.requestSequenceStop();
        workerStopped = sourceToStop.stopAndJoin(1000);
      }
      if (workerStopped) {
        sourceToStop.release();
      } else {
        Logger.error("GestureCamera: frame worker still active; CameraEx release quarantined");
      }
    }

    if (workerStopped && nativeInitialized) {
      NativeGestureDetector.destroy();
      nativeInitialized = false;
      Logger.info("GestureCamera: native detector destroyed");
    }

    if (cameraToRelease != null && workerStopped) {
      try {
        cameraToRelease.release();
        Logger.info("GestureCamera: CameraEx released");
      } catch (RuntimeException e) {
        logFailure("CameraEx.release", e);
      }
    }

    surfaceHolder.removeCallback(this);
    Logger.info("GestureCamera: surface callback removed");

    if (autoPowerOffChanged) {
      try {
        setAutoPowerOffMode(true);
        Logger.info("GestureCamera: automatic power-off restored");
      } catch (RuntimeException e) {
        logFailure("restore automatic power-off", e);
      }
      autoPowerOffChanged = false;
    }

    super.onPause();
    Logger.info("GestureCamera: onPause complete");
  }

  @Override
  public void surfaceCreated(SurfaceHolder holder) {
    Logger.info("GestureCamera: surfaceCreated");

    CameraEx activeCamera;
    synchronized (cameraLock) {
      activeCamera = resumed ? cameraEx : null;
    }
    if (activeCamera == null) {
      Logger.error("GestureCamera: surfaceCreated without an active camera");
      setStatus(R.string.gesture_status_camera_unavailable);
      return;
    }

    try {
      Camera normalCamera = activeCamera.getNormalCamera();
      normalCamera.setPreviewDisplay(holder);
      normalCamera.startPreview();
      synchronized (cameraLock) {
        if (resumed && cameraEx == activeCamera) {
          previewStarted = true;
        }
      }
      setStatus(R.string.gesture_status_manual_preview);
      Logger.info("GestureCamera: normal preview started");
      startAnalyticalPreview(activeCamera);
    } catch (IOException e) {
      logFailure("attach preview display", e);
      setStatus(R.string.gesture_status_camera_unavailable);
    } catch (RuntimeException e) {
      logFailure("start normal preview", e);
      setStatus(R.string.gesture_status_camera_unavailable);
    }
  }

  @Override
  public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    Logger.info(
        "GestureCamera: surfaceChanged format=" + format + " width=" + width + " height=" + height);
  }

  @Override
  public void surfaceDestroyed(SurfaceHolder holder) {
    synchronized (cameraLock) {
      previewStarted = false;
    }
    Logger.info("GestureCamera: surfaceDestroyed");
  }

  @Override
  public void onSequenceStarted(final CameraSequenceFrameSource source) {
    mainHandler.post(new Runnable() {
      @Override
      public void run() {
        if (isCurrentFrameSource(source) && gestureState == GestureState.INITIALIZING) {
          setStatus(R.string.gesture_status_analysis_starting);
        }
      }
    });
  }

  @Override
  public void onFirstFrame(
      final CameraSequenceFrameSource source, final String format, final int length) {
    mainHandler.post(new Runnable() {
      @Override
      public void run() {
        if (isCurrentFrameSource(source)) {
          if (nativeReady) {
            NativeGestureDetector.reset();
            boolean armed = false;
            synchronized (stateLock) {
              if (gestureState == GestureState.INITIALIZING) {
                gestureState = GestureState.ARMED;
                armed = true;
              }
            }
            if (armed) {
              setStatus(R.string.gesture_status_armed);
              Logger.info("GestureCamera: state=ARMED analytical preview ready");
            }
          } else {
            synchronized (stateLock) {
              gestureState = GestureState.ERROR;
            }
            setStatus(R.string.gesture_status_preview_unavailable);
          }
          Logger.info("GestureCamera: analytical preview ready format=" + format
              + " firstPayloadBytes=" + length);
        }
      }
    });
  }

  @Override
  public void onSequenceUnavailable(final CameraSequenceFrameSource source, final String reason) {
    mainHandler.post(new Runnable() {
      @Override
      public void run() {
        if (isCurrentFrameSource(source)) {
          synchronized (stateLock) {
            gestureState = GestureState.ERROR;
            sequenceToken++;
          }
          cancelCountdownCallbacks();
          setStatus(R.string.gesture_status_preview_unavailable);
          Logger.error("GestureCamera: analytical preview unavailable reason=" + reason);
        }
      }
    });
  }

  @Override
  public void onFrame(ByteBuffer buffer, int length, long timestampMs) {
    int state = gestureState;
    if (!nativeReady || (state != GestureState.ARMED && state != GestureState.COOLDOWN)) {
      return;
    }
    long processStartMs = android.os.SystemClock.elapsedRealtime();
    long packedResult = NativeGestureDetector.process(buffer, length, timestampMs);
    long processMs = android.os.SystemClock.elapsedRealtime() - processStartMs;
    if (packedResult < 0) {
      nativeReady = false;
      synchronized (stateLock) {
        gestureState = GestureState.ERROR;
        sequenceToken++;
      }
      Logger.error("GestureCamera: native frame processing failed result=" + packedResult
          + " jpegBytes=" + length + " processMs=" + processMs);
      mainHandler.post(new Runnable() {
        @Override
        public void run() {
          setStatus(R.string.gesture_status_preview_unavailable);
        }
      });
      return;
    }

    nativeFrameCount++;
    nativeTotalProcessMs += processMs;
    int result = NativeGestureDetector.getResult(packedResult);
    int flags = NativeGestureDetector.getFlags(packedResult);
    if (result == NativeGestureDetector.RESULT_WAVE) {
      Logger.info("GestureCamera: confirmed-wave timestampMs=" + timestampMs + " source="
          + ((flags & NativeGestureDetector.FLAG_LOCAL_FLOW) != 0 ? "local-flow" : "centroid")
          + " motionPermille=" + NativeGestureDetector.getMotionPermille(packedResult)
          + " globalPermille=" + NativeGestureDetector.getGlobalMotionPermille(packedResult)
          + " centroid=" + NativeGestureDetector.getCentroidX(packedResult) + ","
          + NativeGestureDetector.getCentroidY(packedResult)
          + " areaPermille=" + NativeGestureDetector.getCandidateAreaPermille(packedResult));
      if (state == GestureState.ARMED) {
        acceptWave(timestampMs);
      } else {
        Logger.info("GestureCamera: ignored-wave state=" + GestureState.name(state)
            + " timestampMs=" + timestampMs);
        updateCooldownStillness(packedResult, timestampMs);
      }
    } else if (state == GestureState.COOLDOWN) {
      updateCooldownStillness(packedResult, timestampMs);
    }
    boolean motionDiagnostic = result == NativeGestureDetector.RESULT_MOTION || flags != 0;
    boolean logMotionDiagnostic = motionDiagnostic && timestampMs - lastMotionDiagnosticMs >= 500;
    if (nativeFrameCount <= 5 || nativeFrameCount % 50 == 0 || logMotionDiagnostic) {
      if (logMotionDiagnostic) {
        lastMotionDiagnosticMs = timestampMs;
      }
      long averageMs = nativeTotalProcessMs / nativeFrameCount;
      Logger.info("GestureCamera: nativeFrame=" + nativeFrameCount + " meanLuma="
          + NativeGestureDetector.getMeanLuminance(packedResult) + " result=" + result
          + " motionPermille=" + NativeGestureDetector.getMotionPermille(packedResult)
          + " globalPermille=" + NativeGestureDetector.getGlobalMotionPermille(packedResult)
          + " centroid=" + NativeGestureDetector.getCentroidX(packedResult) + ","
          + NativeGestureDetector.getCentroidY(packedResult) + " areaPermille="
          + NativeGestureDetector.getCandidateAreaPermille(packedResult) + " flags=" + flags
          + " cameraShift=" + NativeGestureDetector.getCameraShiftX(packedResult) + ","
          + NativeGestureDetector.getCameraShiftY(packedResult)
          + " localFlow=" + NativeGestureDetector.getLocalFlowX(packedResult) + " blocks="
          + NativeGestureDetector.getLocalFlowBlocks(packedResult) + " processMs=" + processMs
          + " averageProcessMs=" + averageMs + " jpegBytes=" + length);
    }
  }

  private void acceptWave(final long timestampMs) {
    final int token;
    synchronized (stateLock) {
      if (gestureState != GestureState.ARMED) {
        return;
      }
      gestureState = GestureState.COUNTDOWN;
      token = ++sequenceToken;
    }
    NativeGestureDetector.reset();
    Logger.info(
        "GestureCamera: countdown accepted-wave timestampMs=" + timestampMs + " token=" + token);
    mainHandler.post(new Runnable() {
      @Override
      public void run() {
        beginCountdown(token);
      }
    });
  }

  private void beginCountdown(final int token) {
    if (!isSequenceState(token, GestureState.COUNTDOWN)) {
      return;
    }
    cancelCountdownCallbacks();
    countdownStartMs = android.os.SystemClock.elapsedRealtime();
    statusView.setText(R.string.gesture_status_detected_countdown);
    countdownView.setText(R.string.gesture_countdown_3);
    countdownView.setVisibility(View.VISIBLE);
    Logger.info(
        "GestureCamera: countdown start timestampMs=" + countdownStartMs + " token=" + token);

    countdownTwoRunnable = new Runnable() {
      @Override
      public void run() {
        if (isSequenceState(token, GestureState.COUNTDOWN)) {
          countdownView.setText(R.string.gesture_countdown_2);
        }
      }
    };
    countdownOneRunnable = new Runnable() {
      @Override
      public void run() {
        synchronized (stateLock) {
          if (sequenceToken != token || gestureState != GestureState.COUNTDOWN) {
            return;
          }
          gestureState = GestureState.AUTOFOCUSING;
        }
        countdownView.setText(R.string.gesture_countdown_1);
        startAutoFocus("automatic countdown");
      }
    };
    captureRunnable = new Runnable() {
      @Override
      public void run() {
        performAutomatedCapture(token);
      }
    };

    postAtElapsedDeadline(countdownTwoRunnable, countdownStartMs + COUNTDOWN_STEP_MS);
    postAtElapsedDeadline(countdownOneRunnable, countdownStartMs + COUNTDOWN_STEP_MS * 2);
    postAtElapsedDeadline(captureRunnable, countdownStartMs + COUNTDOWN_STEP_MS * 3);
  }

  private void performAutomatedCapture(int token) {
    synchronized (stateLock) {
      if (sequenceToken != token || gestureState != GestureState.AUTOFOCUSING) {
        return;
      }
      gestureState = GestureState.CAPTURING;
    }
    countdownView.setVisibility(View.GONE);
    statusView.setText(R.string.gesture_status_capturing);
    boolean requested = requestCapture("automatic");
    if (requested) {
      scheduleAutomaticShutterRelease();
    }
    enterCooldown("automatic", requested);
  }

  private void postAtElapsedDeadline(Runnable runnable, long deadlineMs) {
    long delayMs = deadlineMs - android.os.SystemClock.elapsedRealtime();
    mainHandler.postDelayed(runnable, delayMs > 0 ? delayMs : 0);
  }

  private boolean isSequenceState(int token, int requiredState) {
    synchronized (stateLock) {
      return sequenceToken == token && gestureState == requiredState;
    }
  }

  private void cancelCountdownCallbacks() {
    if (mainHandler == null) {
      return;
    }
    if (countdownTwoRunnable != null) {
      mainHandler.removeCallbacks(countdownTwoRunnable);
      countdownTwoRunnable = null;
    }
    if (countdownOneRunnable != null) {
      mainHandler.removeCallbacks(countdownOneRunnable);
      countdownOneRunnable = null;
    }
    if (captureRunnable != null) {
      mainHandler.removeCallbacks(captureRunnable);
      captureRunnable = null;
    }
  }

  private void scheduleAutomaticShutterRelease() {
    cancelAutomaticShutterRelease();
    automaticShutterReleaseRunnable = new Runnable() {
      @Override
      public void run() {
        automaticShutterReleaseRunnable = null;
        if (gestureState == GestureState.STOPPED) {
          return;
        }
        CameraEx activeCamera = getReadyCamera();
        if (activeCamera == null) {
          Logger.error(
              "GestureCamera: automatic shutter release ignored because preview is not ready");
          return;
        }
        try {
          activeCamera.cancelTakePicture();
          Logger.info("GestureCamera: automatic shutter released timestampMs="
              + android.os.SystemClock.elapsedRealtime());
        } catch (RuntimeException e) {
          logFailure("automatic cancelTakePicture", e);
        }
        stopAutoFocus("automatic post-capture");
      }
    };
    mainHandler.postDelayed(automaticShutterReleaseRunnable, AUTOMATIC_SHUTTER_RELEASE_MS);
  }

  private void cancelAutomaticShutterRelease() {
    if (mainHandler != null && automaticShutterReleaseRunnable != null) {
      mainHandler.removeCallbacks(automaticShutterReleaseRunnable);
      automaticShutterReleaseRunnable = null;
    }
  }

  private void enterCooldown(String source, boolean captureRequested) {
    cancelCountdownCallbacks();
    long nowMs = android.os.SystemClock.elapsedRealtime();
    synchronized (stateLock) {
      if (gestureState == GestureState.STOPPED) {
        return;
      }
      gestureState = nativeReady ? GestureState.COOLDOWN : GestureState.ERROR;
      sequenceToken++;
      cooldownDeadlineMs = nowMs + COOLDOWN_MS;
      stillSinceMs = 0;
    }
    countdownView.setVisibility(View.GONE);
    if (nativeReady) {
      NativeGestureDetector.reset();
      statusView.setText(R.string.gesture_status_cooldown);
      Logger.info("GestureCamera: cooldown start source=" + source
          + " captureRequested=" + captureRequested + " deadlineMs=" + cooldownDeadlineMs);
    } else {
      statusView.setText(R.string.gesture_status_preview_unavailable);
      Logger.error("GestureCamera: cooldown unavailable after " + source
          + " capture because native preview is unavailable");
    }
  }

  private void updateCooldownStillness(long packedResult, long timestampMs) {
    int flags = NativeGestureDetector.getFlags(packedResult);
    int disruptiveFlags = NativeGestureDetector.FLAG_GLOBAL_MOTION
        | NativeGestureDetector.FLAG_EXPOSURE_CHANGE
        | NativeGestureDetector.FLAG_COMPONENT_TOO_LARGE
        | NativeGestureDetector.FLAG_TRAJECTORY_REJECTED
        | NativeGestureDetector.FLAG_CAMERA_TRANSLATION | NativeGestureDetector.FLAG_LOCAL_FLOW;
    boolean still = (flags & disruptiveFlags) == 0
        && NativeGestureDetector.getMotionPermille(packedResult) <= STILL_MOTION_PERMILLE
        && NativeGestureDetector.getGlobalMotionPermille(packedResult) <= STILL_GLOBAL_PERMILLE;
    boolean rearmed = false;
    synchronized (stateLock) {
      if (gestureState != GestureState.COOLDOWN) {
        return;
      }
      if (still) {
        if (stillSinceMs == 0) {
          stillSinceMs = timestampMs;
        }
        if (timestampMs >= cooldownDeadlineMs
            && timestampMs - stillSinceMs >= STILLNESS_REQUIRED_MS) {
          gestureState = GestureState.ARMED;
          sequenceToken++;
          stillSinceMs = 0;
          rearmed = true;
        }
      } else {
        stillSinceMs = 0;
      }
    }
    if (rearmed) {
      NativeGestureDetector.reset();
      Logger.info("GestureCamera: cooldown end and rearm timestampMs=" + timestampMs);
      mainHandler.post(new Runnable() {
        @Override
        public void run() {
          if (gestureState == GestureState.ARMED) {
            countdownView.setVisibility(View.GONE);
            statusView.setText(R.string.gesture_status_armed);
          }
        }
      });
    }
  }

  @Override
  protected boolean onFocusKeyDown() {
    startAutoFocus("manual key");
    return true;
  }

  @Override
  protected boolean onFocusKeyUp() {
    stopAutoFocus("manual key");
    return true;
  }

  @Override
  protected boolean onShutterKeyDown() {
    int previousState;
    synchronized (stateLock) {
      previousState = gestureState;
      if (previousState == GestureState.STOPPED || previousState == GestureState.CAPTURING) {
        Logger.info(
            "GestureCamera: manual shutter ignored state=" + GestureState.name(previousState));
        return true;
      }
      if (previousState != GestureState.ERROR) {
        gestureState = GestureState.CAPTURING;
        sequenceToken++;
      }
    }
    if (previousState == GestureState.COUNTDOWN || previousState == GestureState.AUTOFOCUSING) {
      Logger.info("GestureCamera: manual shutter cancelled automatic countdown");
    }
    cancelCountdownCallbacks();
    cancelAutomaticShutterRelease();
    countdownView.setVisibility(View.GONE);
    statusView.setText(R.string.gesture_status_capturing);
    NativeGestureDetector.reset();
    boolean requested = requestCapture("manual");
    if (previousState != GestureState.ERROR) {
      enterCooldown("manual", requested);
    } else {
      statusView.setText(R.string.gesture_status_preview_unavailable);
    }
    return true;
  }

  @Override
  protected boolean onShutterKeyUp() {
    CameraEx activeCamera = getReadyCamera();
    if (activeCamera != null) {
      try {
        activeCamera.cancelTakePicture();
        Logger.info("GestureCamera: manual shutter release");
      } catch (RuntimeException e) {
        logFailure("cancelTakePicture", e);
      }
    }
    return true;
  }

  private void startAutoFocus(String source) {
    CameraEx activeCamera = getReadyCamera();
    if (activeCamera == null) {
      Logger.error("GestureCamera: autofocus ignored because preview is not ready");
      return;
    }
    try {
      activeCamera.getNormalCamera().autoFocus(null);
      Logger.info("GestureCamera: autofocus requested source=" + source
          + " timestampMs=" + android.os.SystemClock.elapsedRealtime());
    } catch (RuntimeException e) {
      logFailure("autoFocus", e);
    }
  }

  private void stopAutoFocus(String source) {
    CameraEx activeCamera = getReadyCamera();
    if (activeCamera == null) {
      return;
    }
    try {
      activeCamera.getNormalCamera().cancelAutoFocus();
      Logger.info("GestureCamera: autofocus cancelled source=" + source);
    } catch (RuntimeException e) {
      logFailure("cancelAutoFocus", e);
    }
  }

  private boolean requestCapture(String source) {
    CameraEx activeCamera = getReadyCamera();
    if (activeCamera == null) {
      Logger.error("GestureCamera: capture ignored because preview is not ready");
      return false;
    }
    try {
      activeCamera.getNormalCamera().takePicture(null, null, null);
      Logger.info("GestureCamera: capture requested source=" + source
          + " timestampMs=" + android.os.SystemClock.elapsedRealtime());
      return true;
    } catch (RuntimeException e) {
      logFailure("takePicture", e);
      return false;
    }
  }

  private CameraEx getReadyCamera() {
    synchronized (cameraLock) {
      return resumed && previewStarted ? cameraEx : null;
    }
  }

  private void startAnalyticalPreview(CameraEx activeCamera) {
    CameraSequenceFrameSource source;
    synchronized (cameraLock) {
      if (!resumed || !previewStarted || cameraEx != activeCamera || frameSource != null) {
        return;
      }
      source = new CameraSequenceFrameSource(this, nativeReady ? this : null);
      frameSource = source;
    }
    if (source.start(activeCamera)) {
      Logger.info("GestureCamera: analytical frame worker started");
    } else {
      synchronized (cameraLock) {
        if (frameSource == source) {
          frameSource = null;
        }
      }
      Logger.error("GestureCamera: analytical frame worker refused duplicate start");
      setStatus(R.string.gesture_status_preview_unavailable);
    }
  }

  private boolean isCurrentFrameSource(CameraSequenceFrameSource source) {
    synchronized (cameraLock) {
      return resumed && frameSource == source;
    }
  }

  private void setStatus(int stringResource) {
    statusView.setText(stringResource);
  }

  private void logFailure(String operation, Throwable throwable) {
    StringWriter writer = new StringWriter();
    PrintWriter printer = new PrintWriter(writer);
    throwable.printStackTrace(printer);
    printer.close();
    Logger.error("GestureCamera: state=" + GestureState.name(gestureState)
        + " lifecycle=" + (resumed ? "RESUMED" : "STOPPED") + " operation=" + operation
        + " exception=" + throwable.getClass().getName() + " message=" + throwable.getMessage()
        + "\n" + writer.toString());
  }

  @Override
  protected void setColorDepth(boolean highQuality) {
    super.setColorDepth(false);
  }
}
