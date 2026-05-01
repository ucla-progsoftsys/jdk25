/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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
package jdk.internal.profilecheckpoint;

import java.io.IOException;
import java.nio.file.Path;
import java.util.Objects;

/**
 * JVM-internal API to explicitly dump and load a HotSpot profile checkpoint.
 *
 * <p>This is intended for tightly-controlled environments (e.g., serverless
 * platforms) to manage JIT profiling state.
 */
public final class ProfileCheckpoint {
    private ProfileCheckpoint() {}

    /**
     * Dumps the current profile checkpoint to {@code path}.
     *
     * @throws NullPointerException if {@code path} is null
     * @throws IOException if the dump cannot be written
     */
    public static void dump(Path path) throws IOException {
        Objects.requireNonNull(path, "path");
        dump0(path.toString());
    }

    /**
     * Loads a profile checkpoint from {@code path}.
     *
     * @throws NullPointerException if {@code path} is null
     * @throws IOException if the checkpoint cannot be read/loaded
     */
    public static void load(Path path) throws IOException {
        Objects.requireNonNull(path, "path");
        load0(path.toString());
    }

    private static native void dump0(String path) throws IOException;
    private static native void load0(String path) throws IOException;
}


