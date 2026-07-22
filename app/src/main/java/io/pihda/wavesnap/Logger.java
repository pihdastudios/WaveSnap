package io.pihda.wavesnap;

import android.os.Environment;
import java.io.*;

public class Logger {
  public static File getFile() {
    return new File(Environment.getExternalStorageDirectory(), "WAVESNAP/LOG.TXT");
  }

  public static void installUncaughtExceptionHandler() {
    Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
      @Override
      public void uncaughtException(Thread thread, Throwable throwable) {
        StringWriter writer = new StringWriter();
        writer.append(throwable.toString());
        writer.append("\n");
        throwable.printStackTrace(new PrintWriter(writer));
        error(writer.toString());
        System.exit(0);
      }
    });
  }

  protected static synchronized void log(String msg) {
    try {
      getFile().getParentFile().mkdirs();
      BufferedWriter writer = new BufferedWriter(new FileWriter(getFile(), true));
      writer.append(msg);
      writer.newLine();
      writer.close();
    } catch (IOException e) {
    }
  }
  protected static void log(String type, String msg) {
    log("[" + type + "] " + msg);
  }

  public static void info(String msg) {
    log("INFO", msg);
  }
  public static void error(String msg) {
    log("ERROR", msg);
  }
}
