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

package gc.stress.ihash;

/*
 * @test id=Shenandoah
 * @bug 8387285
 * @summary Full GC must not underflow the humongous compaction window when an
 *          identity-hashed, exact-region-multiple humongous object needs one more
 *          region after hash expansion.
 * @requires vm.gc.Shenandoah
 * @run main/othervm
 *      -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+VerifyDuringGC -XX:+ShenandoahVerify
 *      -XX:-ExplicitGCInvokesConcurrent
 *      -Xms16m -Xmx16m
 *      -XX:+UnlockExperimentalVMOptions -XX:ShenandoahRegionSize=256K
 *      gc.stress.ihash.TestHumongousHashFullGC
 */

/*
 * @test id=Shenandoah-aggressive
 * @bug 8387285
 * @summary Full GC must not underflow the humongous compaction window when an
 *          identity-hashed, exact-region-multiple humongous object needs one more
 *          region after hash expansion.
 * @requires vm.gc.Shenandoah
 * @run main/othervm
 *      -XX:+UseCompactObjectHeaders -XX:+UseShenandoahGC
 *      -XX:+UnlockDiagnosticVMOptions -XX:+VerifyDuringGC -XX:+ShenandoahVerify
 *      -XX:-ExplicitGCInvokesConcurrent
 *      -XX:ShenandoahGCHeuristics=aggressive
 *      -Xms16m -Xmx16m
 *      -XX:+UnlockExperimentalVMOptions -XX:ShenandoahRegionSize=256K
 *      gc.stress.ihash.TestHumongousHashFullGC
 */

/**
 * Regression test for the Full-GC half of JDK-8387285.
 *
 * ShenandoahFullGC::calculate_target_humongous_objects() slides movable humongous
 * objects toward the end of the heap within a window [to_begin, to_end). With
 * compact object headers an identity-hashed humongous object grows by one word when
 * relocated, so an object whose size is an exact multiple of the region size needs
 * one extra region after expansion (k regions -> k+1).
 *
 * The bug: the fit test computed "start = to_end - num_regions" before checking it
 * fits. When the expanded num_regions exceeds the remaining window, the unsigned
 * subtraction underflows to ~SIZE_MAX, the guards pass, and the code dereferences a
 * bogus region -> SIGSEGV during STW Full GC.
 *
 * Reproduction (verified to SIGSEGV on the unfixed VM, exit cleanly on the fix):
 * a small heap with the minimum (256K) region size keeps the humongous objects at
 * the bottom of the heap, so the backward compaction scan reaches a movable
 * exact-2-region humongous start while the remaining window is smaller than its
 * expanded (3-region) size. The arrays are identity-hashed so relocation must
 * expand them, and -XX:-ExplicitGCInvokesConcurrent makes System.gc() a STW Full GC.
 */
public class TestHumongousHashFullGC {

    static final int REGION_SIZE = 256 * 1024;

    // byte[] allocation size (with compact headers) = align_up(8 + length, 8).
    // length = k * REGION_SIZE - 8 yields an allocation of exactly k regions.
    static byte[] exactRegions(int k) {
        return new byte[k * REGION_SIZE - 8];
    }

    static final int COUNT = 6;

    static Object[] keep;

    public static void main(String[] args) {
        // Allocate exact-2-region humongous arrays at the bottom of the small heap
        // and identity-hash each so a relocating GC must expand it by one word.
        keep = new Object[COUNT];
        int[] hashes = new int[COUNT];
        for (int i = 0; i < COUNT; i++) {
            byte[] o = exactRegions(2);
            hashes[i] = System.identityHashCode(o);
            keep[i] = o;
        }

        // Fragment: drop a couple of arrays low in the heap so the backward Full GC
        // compaction scan reaches a movable exact-2-region humongous start while the
        // remaining window is smaller than its expanded (3-region) size.
        keep[1] = null;
        keep[3] = null;

        // STW Full GC compaction. With the bug this SIGSEGVs in
        // calculate_target_humongous_objects(); with the fix it slides cleanly.
        System.gc();

        // Validate surviving objects are intact across expansion/relocation.
        for (int i = 0; i < COUNT; i++) {
            byte[] o = (byte[]) keep[i];
            if (o == null) {
                continue;
            }
            if (System.identityHashCode(o) != hashes[i]) {
                throw new RuntimeException("hash mismatch at " + i);
            }
        }
        System.out.println("OK");
    }
}
