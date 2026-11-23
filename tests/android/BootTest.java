package org.kyujo2.boottest;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class BootTest extends Activity {
    static { System.loadLibrary("kyujo2boot"); }
    private static native String runTest();
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView result = new TextView(this);
        result.setTextIsSelectable(true);
        result.setText("Kyujo2 US - native boot regression r2\n\n" + runTest());
        setContentView(result);
    }
}
