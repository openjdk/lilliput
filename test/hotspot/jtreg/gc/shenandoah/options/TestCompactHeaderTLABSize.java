/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
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
 */

/*
 * @test id=compact-headers
 * @summary With compact object headers the max TLAB size must be strictly below a
 *          region, so a region-sized object can never be served from a TLAB (which
 *          would bypass the humongous path). The cap is only observable indirectly:
 *          a region-sized -XX:TLABSize is rejected at startup under compact headers,
 *          but accepted without them (where the max TLAB equals the region size).
 * @bug 8387285
 * @requires vm.gc.Shenandoah
 * @library /test/lib
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @run driver TestCompactHeaderTLABSize
 */

import jdk.test.lib.process.ProcessTools;
import jdk.test.lib.process.OutputAnalyzer;

public class TestCompactHeaderTLABSize {

    // Minimum Shenandoah region size; the only size at which the un-capped max TLAB
    // would otherwise equal the region size.
    static final String REGION_SIZE = "256K";

    public static void main(String[] args) throws Exception {
        // With compact object headers, the max TLAB is capped one word below the
        // region, so requesting a region-sized TLAB must be rejected at startup.
        OutputAnalyzer coh = ProcessTools.executeLimitedTestJava(
                "-XX:+UnlockExperimentalVMOptions",
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:+UseShenandoahGC",
                "-XX:+UseCompactObjectHeaders",
                "-XX:ShenandoahRegionSize=" + REGION_SIZE,
                "-XX:TLABSize=" + REGION_SIZE,
                "-Xmx512m",
                "-version");
        coh.shouldContain("must be less than or equal to ergonomic TLAB maximum size");
        coh.shouldHaveExitValue(1);

        // Without compact object headers there is no hash-code expansion, so the max
        // TLAB equals the region size and the same region-sized TLAB is accepted.
        OutputAnalyzer noCoh = ProcessTools.executeLimitedTestJava(
                "-XX:+UnlockExperimentalVMOptions",
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:+UseShenandoahGC",
                "-XX:-UseCompactObjectHeaders",
                "-XX:ShenandoahRegionSize=" + REGION_SIZE,
                "-XX:TLABSize=" + REGION_SIZE,
                "-Xmx512m",
                "-version");
        noCoh.shouldHaveExitValue(0);

        // Under compact headers, a TLAB one word below the region is still accepted:
        // the cap is exactly one (object-aligned) word, not an arbitrary reduction.
        OutputAnalyzer atCap = ProcessTools.executeLimitedTestJava(
                "-XX:+UnlockExperimentalVMOptions",
                "-XX:+UnlockDiagnosticVMOptions",
                "-XX:+UseShenandoahGC",
                "-XX:+UseCompactObjectHeaders",
                "-XX:ShenandoahRegionSize=" + REGION_SIZE,
                "-XX:TLABSize=" + (256 * 1024 - 8), // one HeapWord below the region
                "-Xmx512m",
                "-version");
        atCap.shouldHaveExitValue(0);
    }
}
