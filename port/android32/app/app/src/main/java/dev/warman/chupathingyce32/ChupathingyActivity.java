package dev.warman.chupathingyce32;

import org.libsdl.app.SDLActivity;

/* SDL3's activity, loading the game's libmain.so (its SDL_main is
port/android32/src/android32_main.c) after SDL3 */
public class ChupathingyActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
