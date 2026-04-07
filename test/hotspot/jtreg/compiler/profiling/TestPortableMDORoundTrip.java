/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
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
 *
 */

/**
 * @test
 * @summary Test portable MDO export/import round-trip
 * @requires vm.flavor == "server"
 * @requires vm.compMode != "Xcomp"
 * @library /test/lib
 * @run driver compiler.profiling.TestPortableMDORoundTrip
 */

package compiler.profiling;

import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class TestPortableMDORoundTrip {

    // A simple workload class whose methods will be profiled
    static class Workload {
        static int hotMethod(int x) {
            int sum = 0;
            for (int i = 0; i < x; i++) {
                sum += i;
            }
            return sum;
        }

        static String polymorphicCall(Object o) {
            return o.toString();
        }

        public static void main(String[] args) throws Exception {
            String mode = args.length > 0 ? args[0] : "export";

            // Exercise the hot methods to build up profiles
            int total = 0;
            for (int i = 0; i < 50_000; i++) {
                total += hotMethod(10);
                if (i % 2 == 0) {
                    polymorphicCall("hello");
                } else {
                    polymorphicCall(Integer.valueOf(42));
                }
            }

            // Force compilation
            Thread.sleep(500);

            System.out.println("Workload completed, total=" + total + ", mode=" + mode);
        }
    }

    public static void main(String[] args) throws Exception {
        Path mdoFile = Files.createTempFile("portableMDO", ".mdo");
        String mdoPath = mdoFile.toAbsolutePath().toString();

        try {
            // Step 1: Export run — execute workload and export profiles at shutdown
            ProcessBuilder pb1 = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:ExportMDOFile=" + mdoPath,
                "-XX:MDOExportDeoptDecayPercent=50",
                "-Xlog:aot+training=info",
                "-Xmx128m",
                Workload.class.getName(),
                "export"
            );
            OutputAnalyzer out1 = new OutputAnalyzer(pb1.start());
            out1.shouldHaveExitValue(0);
            out1.shouldContain("Workload completed");

            // Verify the file was created and is non-empty
            File f = new File(mdoPath);
            if (!f.exists()) {
                throw new RuntimeException("MDO file was not created: " + mdoPath);
            }
            long fileSize = f.length();
            System.out.println("Exported MDO file size: " + fileSize + " bytes");
            if (fileSize < 64) {
                throw new RuntimeException("MDO file is suspiciously small: " + fileSize);
            }

            // Verify magic number
            byte[] header = Files.readAllBytes(mdoFile);
            if (header.length < 4) {
                throw new RuntimeException("MDO file too short for magic number");
            }
            // Magic "JMDO" = 0x4A4D444F in little-endian
            int magic = (header[0] & 0xFF) |
                        ((header[1] & 0xFF) << 8) |
                        ((header[2] & 0xFF) << 16) |
                        ((header[3] & 0xFF) << 24);
            if (magic != 0x4A4D444F) {
                throw new RuntimeException("Bad magic number: 0x" + Integer.toHexString(magic));
            }
            System.out.println("Magic number verified: 0x" + Integer.toHexString(magic));

            // Step 2: Import run — start a fresh JVM and import the profiles
            ProcessBuilder pb2 = ProcessTools.createTestJavaProcessBuilder(
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:ImportMDOFile=" + mdoPath,
                "-Xlog:aot+training=info",
                "-Xmx128m",
                Workload.class.getName(),
                "import"
            );
            OutputAnalyzer out2 = new OutputAnalyzer(pb2.start());
            out2.shouldHaveExitValue(0);
            out2.shouldContain("Workload completed");

            System.out.println("Round-trip test PASSED");

        } finally {
            Files.deleteIfExists(mdoFile);
        }
    }
}
