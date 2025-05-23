/*
 * Copyright (c) 2019, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package sun.awt;

import java.awt.GraphicsEnvironment;
import java.awt.Toolkit;

import sun.awt.windows.WToolkit;

public final class PlatformGraphicsInfo {

    private static final boolean hasDisplays;

    static {
        loadAWTLibrary();
        hasDisplays = hasDisplays0();
    }

    @SuppressWarnings("restricted")
    private static void loadAWTLibrary() {
        System.loadLibrary("awt");
    }

    private static native boolean hasDisplays0();

    public static GraphicsEnvironment createGE() {
        return new Win32GraphicsEnvironment();
    }

    public static Toolkit createToolkit() {
        return new WToolkit();
    }

    public static boolean getDefaultHeadlessProperty() {
        // If we don't find usable displays, we run headless.
        return !hasDisplays;
    }

    /*
     * Called from java.awt.GraphicsEnvironment when
     * getDefaultHeadlessProperty() has returned true, and
     * the application has called an API that requires headful.
     */
    public static String getDefaultHeadlessMessage() {
        return
            "\nThe application does not have desktop access,\n" +
            "but this program performed an operation which requires it.";
    }
}
