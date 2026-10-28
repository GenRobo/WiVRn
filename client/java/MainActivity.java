package org.meumeu.wivrn;

public class MainActivity extends android.app.NativeActivity
{
	static
	{
		System.loadLibrary("wivrn");
	}

	@Override
	public native void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults);

	@Override
	public native void onNewIntent(android.content.Intent intent);

	@Override
	public native void onActivityResult(int requestCode, int resultCode, android.content.Intent data);

	// Application data channels: byte streams relayed by the server to TCP sinks (docs/configuration.md, "app-data")
	public interface AppDataListener
	{
		// Called on the native network thread while a stream session is active
		void onAppData(String channel, byte[] data);
	}

	private final java.util.Map<String, AppDataListener> appDataListeners = new java.util.concurrent.ConcurrentHashMap<>();

	// Sends data on a channel, split into packets of at most 1 MiB; one call is never interleaved with another.
	// Returns false, dropping the data, when no stream session is active.
	public native boolean sendAppData(String channel, byte[] data);

	// Receives the data the server relays on a channel; a null listener unsubscribes
	public void setAppDataListener(String channel, AppDataListener listener)
	{
		if (listener == null)
			appDataListeners.remove(channel);
		else
			appDataListeners.put(channel, listener);
	}

	// Called from native code on the network thread for channels without a native handler
	public void onAppData(String channel, byte[] data)
	{
		AppDataListener listener = appDataListeners.get(channel);
		if (listener != null)
			listener.onAppData(channel, data);
	}

	private android.content.BroadcastReceiver BatteryInfoReceiver = null;
	public android.net.ConnectivityManager.NetworkCallback netcb = null;

	@Override
	protected void onCreate(android.os.Bundle savedInstanceState)
	{
		super.onCreate(savedInstanceState);

		this.BatteryInfoReceiver = new BroadcastReceiver();
		this.netcb = new org.meumeu.wivrn.NetworkInfoCallback();

		this.registerReceiver(this.BatteryInfoReceiver, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED));
	}
}
