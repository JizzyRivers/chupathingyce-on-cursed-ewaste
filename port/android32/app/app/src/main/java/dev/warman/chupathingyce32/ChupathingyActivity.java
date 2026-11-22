package dev.warman.chupathingyce32;

import android.os.Bundle;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/* SDL3's activity, loading the game's libmain.so (its SDL_main is
port/android32/src/android32_main.c) after SDL3, with the on-screen touch
controls over its surface (TouchControls.java: upstream's overlay, with a
compact layout for a small, wide screen) */
public class ChupathingyActivity extends SDLActivity {
    private TouchControls touchControls;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mLayout != null) {
            touchControls = new TouchControls(this);
            mLayout.addView(touchControls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (touchControls != null && getWindow().getDecorView().hasWindowFocus())
            touchControls.startDeviceInput();
    }

    @Override
    protected void onPause() {
        if (touchControls != null) touchControls.stopDeviceInput();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (touchControls != null) {
            if (hasFocus) touchControls.startDeviceInput();
            else touchControls.stopDeviceInput();
        }
        super.onWindowFocusChanged(hasFocus);
    }

    @Override
    protected void onDestroy() {
        if (touchControls != null) touchControls.stopDeviceInput();
        super.onDestroy();
    }

    /** the system's gesture insets {left, top, right, bottom} in pixels: none are asked of the system here */
    public int[] getSystemGestureInsetsPixels() {
        return new int[] { 0, 0, 0, 0 };
    }

    /** the touch layout's export and import (upstream's own file picker): not in this build yet */
    public void chooseLayoutFile(boolean export, String configuration) {
        Toast.makeText(this, "Layout export and import are not in this build yet.", Toast.LENGTH_LONG).show();
    }

    /** upstream's import of Halo PC maps: not in this build (the maps go in the app's files folder) */
    public void chooseMapFiles() {
        Toast.makeText(this, "Put the maps in the app's files folder.", Toast.LENGTH_LONG).show();
    }
}
