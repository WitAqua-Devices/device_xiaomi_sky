package org.lineageos.dt2w.sky;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.UserHandle;

public class OnBootCompleteReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        Intent sIntent = new Intent(context, DT2WServiceSky.class);
        context.startServiceAsUser(sIntent, UserHandle.CURRENT);
    }
}
