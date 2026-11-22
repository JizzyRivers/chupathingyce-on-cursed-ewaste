package dev.warman.chupathingyce32;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The launcher: starts the game when its data (the Xbox maps, with ui.map) is in the app's files folder,
 * and otherwise asks for it: a disc image of Halo (an .iso or .xiso, copied out by XisoExtractor) or a folder
 * that has the maps (a copy from a computer, a USB drive), each chosen with the system's file picker, in
 * large buttons a remote's d-pad reaches. The data can also be put there with adb, which the screen says.
 *
 * Its own start of the game is the native code's data check (src/android32_main.c) made first and made
 * gentle: that one only quits with a message.
 */
public class SetupActivity extends Activity {
    private static final int PICK_IMAGE = 1, PICK_FOLDER = 2;
    private static final int COPY_BUFFER = 1 << 20;
    /** the maps are about 1.8 GB (the discs' whole set) */
    private static final long SPACE_NEEDED = 2L << 30;
    /** room left over after a map */
    private static final long SPACE_MARGIN = 64L << 20;
    private volatile String skippedNote;

    private File dataRoot;
    private TextView status;
    private ProgressBar progress;
    private Button pickImage, pickFolder, recheck;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean gameStarted, working;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        // made by the app, so that it owns the folder (one adb creates belongs to the shell user)
        if (dataRoot != null)
            new File(dataRoot, "maps").mkdirs();
        if (haveData()) {
            startGame();
            return;
        }
        buildInterface();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // data pushed with adb while this screen was open
        if (!gameStarted && !working && haveData())
            startGame();
    }

    private boolean haveData() {
        return dataRoot != null && new File(dataRoot, "maps/ui.map").isFile();
    }

    private void startGame() {
        if (gameStarted)
            return;
        gameStarted = true;
        if (skippedNote != null)
            android.widget.Toast.makeText(getApplicationContext(), skippedNote, android.widget.Toast.LENGTH_LONG).show();
        startActivity(new Intent(this, ChupathingyActivity.class));
        finish();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }

    private Button button(String text) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        button.setAllCaps(false);
        button.setFocusable(true);
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(dp(360), LinearLayout.LayoutParams.WRAP_CONTENT);
        layout.topMargin = dp(6);
        button.setLayoutParams(layout);
        return button;
    }

    private void buildInterface() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(32), dp(12), dp(32), dp(12));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Halo needs its game data");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("Choose an Xbox disc image of Halo: Combat Evolved (an .iso or .xiso file), or a folder "
            + "that has the maps (the Xbox's, with ui.map). They are copied into the app's storage "
            + "(about 1.8 GB); the image can be deleted afterwards.\n\n"
            + "Or copy them from a computer:  adb push maps " + (dataRoot != null ? dataRoot.getAbsolutePath() : "")
            + "/");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(8), 0, dp(6));
        layout.addView(message);

        pickImage = button("Choose a disc image");
        pickImage.setOnClickListener(v -> {
            // (disc images have no MIME type of their own: any file, checked when it is read)
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            launchPicker(intent, PICK_IMAGE);
        });
        layout.addView(pickImage);

        pickFolder = button("Choose a folder with the maps");
        pickFolder.setOnClickListener(v -> launchPicker(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER));
        layout.addView(pickFolder);

        recheck = button("I copied them: check again");
        recheck.setOnClickListener(v -> {
            if (haveData())
                startGame();
            else
                status.setText("maps/ui.map is not in " + dataRoot + " yet.");
        });
        layout.addView(recheck);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(480), LinearLayout.LayoutParams.WRAP_CONTENT);
        progressLayout.topMargin = dp(14);
        layout.addView(progress, progressLayout);

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        layout.addView(status);

        // (a short, wide screen scrolls: the d-pad's focus brings each button into view)
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(12, 16, 20));
        scroll.addView(layout, new ScrollView.LayoutParams(ScrollView.LayoutParams.MATCH_PARENT,
            ScrollView.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
        pickImage.requestFocus();
    }

    private void launchPicker(Intent intent, int request) {
        // start in Downloads, where a copied disc image or maps folder usually is
        try {
            intent.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Download"));
        } catch (RuntimeException e) {
            // (the picker's own first place then)
        }
        try {
            startActivityForResult(intent, request);
        } catch (ActivityNotFoundException e) {
            // (some boxes have no file picker: the data goes in with adb, as the screen says)
            status.setText("This device has no file picker. Copy the maps with adb, as above.");
        }
    }

    private void setBusy(boolean busy) {
        working = busy;
        pickImage.setEnabled(!busy);
        pickFolder.setEnabled(!busy);
        recheck.setEnabled(!busy);
        progress.setVisibility(busy ? View.VISIBLE : View.GONE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri chosen = data.getData();
        // (a disc image is copied whole, so it needs the room; a folder's maps are copied as far as they fit)
        if (requestCode == PICK_IMAGE && dataRoot != null && dataRoot.getUsableSpace() < SPACE_NEEDED && !haveData()) {
            status.setText(String.format(Locale.US, "Not enough free space for the whole disc: the maps need about 2 GB, "
                + "%d MB is free. A folder with some of the maps (ui.map and the multiplayer ones) can be copied instead.",
                dataRoot.getUsableSpace() >> 20));
            return;
        }
        if (requestCode == PICK_IMAGE) {
            setBusy(true);
            status.setText("Reading the disc image...");
            new Thread(() -> importImage(chosen)).start();
        } else if (requestCode == PICK_FOLDER) {
            setBusy(true);
            status.setText("Looking for the maps...");
            new Thread(() -> importFolder(chosen)).start();
        }
    }

    private void report(String text, int permille) {
        handler.post(() -> {
            if (status != null)
                status.setText(text);
            if (permille >= 0 && progress != null)
                progress.setProgress(permille);
        });
    }

    private void fail(String text) {
        handler.post(() -> {
            status.setText(text);
            setBusy(false);
            pickImage.requestFocus();
        });
    }

    private void finished() {
        handler.post(() -> {
            if (haveData())
                startGame();
            else
                fail("The copy finished but maps/ui.map is missing.");
        });
    }

    /* ---------- a disc image */

    private void importImage(Uri image) {
        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(image, "r")) {
            if (descriptor == null)
                throw new IOException("the file could not be opened");
            try (FileInputStream in = new FileInputStream(descriptor.getFileDescriptor())) {
                FileChannel channel = in.getChannel();

                XisoExtractor.extractMaps(channel, dataRoot, (file, done, total) ->
                    report("Copying maps/" + file + " (" + (done >> 20) + " of " + (total >> 20) + " MB)",
                        total > 0 ? (int) (done * 1000 / total) : 0));
            }
            finished();
        } catch (XisoExtractor.ExtractException exception) {
            fail(exception.getMessage());
        } catch (Exception exception) {
            fail("Copying failed: " + exception.getMessage());
        }
    }

    /* ---------- a folder (the Storage Access Framework's tree) */

    private static final class Document {
        final String id, name;
        final boolean directory;
        final long size;

        Document(String id, String name, String type, long size) {
            this.id = id; this.name = name; this.size = size;
            this.directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(type);
        }
    }

    private List<Document> children(Uri tree, String parent) {
        List<Document> list = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE };
        try (Cursor cursor = getContentResolver().query(uri, columns, null, null, null)) {
            while (cursor != null && cursor.moveToNext())
                list.add(new Document(cursor.getString(0), cursor.getString(1), cursor.getString(2),
                    cursor.isNull(3) ? 0 : cursor.getLong(3)));
        } catch (RuntimeException e) {
            // (a folder the provider will not list: no maps found in it)
        }
        return list;
    }

    private static boolean named(Document document, String name) {
        return document.name.equalsIgnoreCase(name);
    }

    /** the folder with ui.map: the chosen one, or a maps folder in it or one folder further in */
    private Document findMaps(Uri tree, String root, int depth, Document[] binkOut) {
        List<Document> list = children(tree, root);
        boolean here = false;
        for (Document document : list)
            if (!document.directory && named(document, "ui.map"))
                here = true;
        if (here)
            return new Document(root, "maps", DocumentsContract.Document.MIME_TYPE_DIR, 0);
        if (depth >= 2)
            return null;
        for (Document document : list) {
            if (document.directory && named(document, "bink"))
                binkOut[0] = document;
        }
        for (Document document : list) {
            if (document.directory && named(document, "maps")) {
                Document found = findMaps(tree, document.id, depth + 1, binkOut);
                if (found != null)
                    return found;
            }
        }
        for (Document document : list) {
            if (document.directory && !named(document, "bink")) {
                Document found = findMaps(tree, document.id, depth + 1, binkOut);
                if (found != null)
                    return found;
            }
        }
        return null;
    }

    private void importFolder(Uri tree) {
        try {
            String root = DocumentsContract.getTreeDocumentId(tree);
            Document[] bink = new Document[1];
            Document maps = findMaps(tree, root, 0, bink);

            if (maps == null) {
                fail("No ui.map in that folder. Choose the folder that has the Xbox maps (or the one with maps inside).");
                return;
            }
            List<Document> files = new ArrayList<>();
            long total = 0;
            for (Document document : children(tree, maps.id)) {
                if (!document.directory && document.name.toLowerCase(Locale.US).endsWith(".map")
                    && XisoExtractor.isPlainName(document.name)) {
                    files.add(document);
                    total += document.size;
                }
            }
            // ui.map first, then the smallest maps first: where the room runs out the rest are left, and the
            // game starts with the maps there are (the multiplayer maps are about 20 MB, the campaign's 130 to 190)
            files.sort((a, b) -> named(a, "ui.map") != named(b, "ui.map") ? (named(a, "ui.map") ? -1 : 1)
                : Long.compare(a.size, b.size));
            File mapsFolder = new File(dataRoot, "maps");
            mapsFolder.mkdirs();
            long done = 0;
            int copiedMaps = 0, skippedMaps = 0;
            for (Document document : files) {
                if (dataRoot.getUsableSpace() < document.size + SPACE_MARGIN) {
                    if (named(document, "ui.map")) {
                        fail("Not enough free space: even ui.map (" + (document.size >> 20) + " MB) does not fit.");
                        return;
                    }
                    skippedMaps++;
                    continue;
                }
                final long before = done;
                final long all = total;
                copyDocument(tree, document, new File(mapsFolder, document.name), (copied) -> report(
                    "Copying maps/" + document.name + " (" + ((before + copied) >> 20) + " of " + (all >> 20) + " MB)",
                    all > 0 ? (int) ((before + copied) * 1000 / all) : 0));
                done += document.size;
                copiedMaps++;
            }
            if (skippedMaps > 0)
                skippedNote = skippedMaps + " of " + (copiedMaps + skippedMaps) + " maps did not fit and were left out.";
            // the movies, where the disc's bink folder came along (never over a file already there)
            if (bink[0] != null) {
                File binkFolder = new File(dataRoot, "bink");
                binkFolder.mkdirs();
                for (Document document : children(tree, bink[0].id)) {
                    File target = new File(binkFolder, document.name);
                    if (!document.directory && document.name.toLowerCase(Locale.US).endsWith(".bik")
                        && XisoExtractor.isPlainName(document.name) && !target.exists()) {
                        try {
                            copyDocument(tree, document, target, (copied) -> report("Copying bink/" + document.name, -1));
                        } catch (IOException e) {
                            // (a movie that cannot be copied does not fail the maps)
                        }
                    }
                }
            }
            finished();
        } catch (Exception exception) {
            fail("Copying failed: " + exception.getMessage());
        }
    }

    private interface Copied {
        void report(long bytes);
    }

    /** a file written as name.partial and renamed when whole */
    private void copyDocument(Uri tree, Document document, File target, Copied copied) throws IOException {
        Uri uri = DocumentsContract.buildDocumentUriUsingTree(tree, document.id);
        File partial = new File(target.getPath() + ".partial");
        ContentResolver resolver = getContentResolver();
        byte[] buffer = new byte[COPY_BUFFER];
        long bytes = 0, lastReport = 0;

        try (InputStream in = resolver.openInputStream(uri); OutputStream out = new FileOutputStream(partial)) {
            if (in == null)
                throw new IOException("cannot open " + document.name);
            int count;
            while ((count = in.read(buffer)) > 0) {
                out.write(buffer, 0, count);
                bytes += count;
                if (bytes - lastReport >= (8 << 20)) {
                    copied.report(bytes);
                    lastReport = bytes;
                }
            }
        } catch (IOException e) {
            partial.delete();
            throw e;
        }
        target.delete();
        if (!partial.renameTo(target)) {
            partial.delete();
            throw new IOException("cannot write " + target.getName());
        }
        copied.report(bytes);
    }
}
