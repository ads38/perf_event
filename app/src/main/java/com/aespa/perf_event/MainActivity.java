package com.aespa.perf_event;

import androidx.appcompat.app.AppCompatActivity;

import android.os.Bundle;
import android.view.View;
import android.widget.TextView;

import com.aespa.perf_event.databinding.ActivityMainBinding;

public class MainActivity extends AppCompatActivity {

    // Used to load the 'perf_event' library on application startup.
    static {
        System.loadLibrary("perf_event");
    }

    private ActivityMainBinding binding;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        // Example of a call to a native method
        TextView tv = binding.sampleText;
        tv.setText("lalala");
    }

    public void ccc(View v)
    {
       // this.getRegs();
        this.getRegs_Mem();
    }

    /**
     * A native method that is implemented by the 'perf_event' native library,
     * which is packaged with this application.
     */
   // public native String stringFromJNI();
    public native void getRegs();
    public native void getRegs_Mem();
}