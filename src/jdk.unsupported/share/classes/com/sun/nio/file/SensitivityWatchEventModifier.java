/*
 * Copyright (c) 2007, 2023, Oracle and/or its affiliates. All rights reserved.
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

package com.sun.nio.file;

import java.nio.file.WatchEvent.Modifier;
import jdk.internal.misc.FileSystemOption;

/**
 * Defines the <em>sensitivity levels</em> when registering objects with a
 * watch service implementation that polls the file system.
 *
 * @deprecated
 * The sensitivity levels were historically used by polling-based
 * {@link java.nio.file.WatchService WatchService} implementations to configure
 * the polling interval. They are are no longer used. The {@code WatchService}
 * implementations in the JDK ignore these {@link java.nio.file.WatchEvent
 * WatchEvent} modifiers if they are specified when registering a directory
 * to be watched.
 *
 * @since 1.7
 */

@Deprecated(since="21", forRemoval = true)
public enum SensitivityWatchEventModifier implements Modifier {
    /**
     * High sensitivity.
     */
    HIGH(FileSystemOption.SENSITIVITY_HIGH, 2),
    /**
     * Medium sensitivity.
     */
    MEDIUM(FileSystemOption.SENSITIVITY_MEDIUM, 10),
    /**
     * Low sensitivity.
     */
    LOW(FileSystemOption.SENSITIVITY_LOW, 30);

    /**
     * Returns the sensitivity in seconds.
     */
    public int sensitivityValueInSeconds() {
        return sensitivity;
    }

    private final int sensitivity;
    private SensitivityWatchEventModifier(FileSystemOption<Integer> option,
                                          int sensitivity) {
        this.sensitivity = sensitivity;
        option.register(this, sensitivity);
    }
}
